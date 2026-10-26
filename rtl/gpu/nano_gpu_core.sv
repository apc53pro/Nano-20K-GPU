// Functional command-to-pixel RTL core. The initial implementation serializes
// tile transfers; it is the actual circuit simulated by Verilator and synthesized.
// External SDRAM is accessed only through nano_sdram_service's physical pins.
module nano_gpu_core #(
    parameter integer POWERUP_CYCLES = 10080,
    parameter integer REFRESH_INTERVAL = 1575
)(
    input wire clk, input wire reset, input wire host_clk, input wire abort_request,
    input wire host_wr_valid, input wire [7:0] host_wr_data,
    output wire host_wr_ready,
    input wire host_rd_req, input wire [6:0] host_reg_addr,
    output wire [7:0] host_rd_data, output wire host_rd_valid,
    input wire [31:0] sd_dq_in,
    output wire sd_clk, output wire sd_cke, output wire sd_cs_n,
    output wire sd_ras_n, output wire sd_cas_n, output wire sd_we_n,
    output wire [1:0] sd_ba, output wire [10:0] sd_addr,
    output wire [31:0] sd_dq_out, output wire sd_dq_oe, output wire [3:0] sd_dqm,
    output wire [15:0] video_rgb565, output wire video_hsync,
    output wire video_vsync, output wire video_de,
    output wire [9:0] video_x, output wire [8:0] video_y,
    output reg video_pixel_ce,
    output wire busy, output wire ready, output wire fault,
    output reg [7:0] fault_code
);
    localparam [22:0] COLOR_BYTES=23'd614400, DEPTH_BASE=23'h12c000,
        HEAP_BASE=23'h258000;
    localparam [31:0] MEMORY_BYTES=32'h00800000;
    localparam [7:0] S_BOOT_WAIT=0, S_BOOT_CMD=1, S_BOOT_DATA=2, S_BOOT_DONE=3,
        S_IDLE=4, S_CAPTURE_REQ=5, S_CAPTURE_WAIT=6, S_EXECUTE=7,
        S_UPLOAD_PREP=8, S_UPLOAD_CMD=9, S_UPLOAD_BYTE_REQ=10,
        S_UPLOAD_BYTE_WAIT=11, S_UPLOAD_WORD=12, S_UPLOAD_DONE=13,
        S_DESC_CMD=14, S_DESC_DATA=15, S_DESC_DONE=16,
        S_ENTRY_CMD=17, S_ENTRY_DATA=18, S_ENTRY_DONE=19,
        S_TILE_INIT=20, S_LOAD_COLOR_CMD=21, S_LOAD_COLOR_DATA=22,
        S_LOAD_COLOR_DONE=23, S_LOAD_DEPTH_CMD=24, S_LOAD_DEPTH_DATA=25,
        S_LOAD_DEPTH_DONE=26, S_LOAD_PIXEL=27,
        S_REF_CMD=28, S_REF_DATA=29, S_REF_DONE=30,
        S_RECORD_CMD=31, S_RECORD_DATA=32, S_RECORD_DONE=33,
        S_DRAW_START=34, S_DRAW_WAIT=35,
        S_STORE_CMD=36, S_STORE_PIXEL_REQ=37, S_STORE_PIXEL_WAIT=38,
        S_STORE_SECOND_REQ=39, S_STORE_SECOND_WAIT=40,
        S_STORE_WORD=41, S_STORE_DONE=42,
        S_PRESENT=43, S_REPLY=44, S_REPLY_RAM_REQ=45, S_REPLY_RAM_WAIT=46,
        S_READ_CMD=47, S_READ_DATA=48, S_READ_BYTES=49, S_READ_DONE=50,
        S_COPY_VALIDATE=51, S_COPY_INIT_ADDR=52,
        S_COPY_READ_CMD=53, S_COPY_READ_DATA=54, S_COPY_READ_DONE=55,
        S_COPY_RAM_REQ=56, S_COPY_RAM_WAIT=57, S_COPY_WRITE_CMD=58,
        S_COPY_WRITE_DATA=59, S_COPY_WRITE_DONE=60,
        S_FAULT=61, S_RECOVER=62,
        S_UPLOAD_STAGE_LOW=63, S_UPLOAD_STAGE_HIGH=64,
        S_UPLOAD_RAM_LOW=65, S_UPLOAD_RAM_HIGH=66, S_COPY_CLIP=67,
        S_COPY_FETCH_REQ=68, S_COPY_FETCH_WAIT=69, S_COPY_CLIP_CHECK=70,
        S_UPLOAD_STAGE_MASK=71, S_UPLOAD_RAM_MASK=72;
    reg [7:0] state;
    reg initialized, front_id, sealed, depth_valid, pass_active, copy_active;
    reg [1:0] color_valid;
    reg [31:0] epoch, completed_sequence, fence_token, present_token;
    reg [63:0] cycle_counter;
    reg [1:0] pixel_phase;
    wire pixel_tick = pixel_phase == 2'd3;
    reg abort_sync1, abort_sync2, abort_previous, abort_pending, fault_pending;
    reg renderer_reset, host_abort;
    reg [5:0] pending_memory;

    wire [5:0] mem_cmd_ready, mem_w_ready, mem_r_valid, mem_r_last, mem_done, mem_error;
    wire [191:0] mem_r_data;
    reg [5:0] mem_cmd_valid, mem_cmd_write, mem_w_valid, mem_r_ready;
    reg [137:0] mem_cmd_addr;
    reg [29:0] mem_cmd_words;
    reg [191:0] mem_w_data;
    reg [23:0] mem_w_strb;
    wire memory_initialized;
    // pending_memory records only commands accepted on earlier edges. A cache
    // miss may be accepted on the very edge that recovery observes it empty;
    // do not reset that client until this new physical transaction drains.
    wire foreground_command_accepted = |(mem_cmd_valid[5:1] & mem_cmd_ready[5:1]);

    wire host_cmd_valid, payload_valid, response_ready, host_recovering;
    wire [15:0] host_cmd_opcode;
    wire [31:0] host_cmd_sequence;
    wire [10:0] host_cmd_length;
    wire [7:0] payload_data, host_fault;
    assign ready = initialized && !host_recovering;
    assign fault = fault_pending || state == S_FAULT;
    assign busy = !ready || state != S_IDLE || host_cmd_valid;
    reg host_cmd_pop, payload_req, response_valid;
    reg [9:0] payload_offset;
    reg [7:0] response_data;
    reg [511:0] status_snapshot, capabilities;
    // Native packet metadata stays immutable at the queue head until the
    // command retires and cmd_pop is consumed. No duplicate header FF bank is
    // needed; recovery abandons the command before flushing its lease.
    wire [15:0] opcode = host_cmd_opcode;
    wire [31:0] command_sequence = host_cmd_sequence;
    wire [10:0] payload_length = host_cmd_length;
    // The native host packet bank retains the complete payload until command
    // retirement. Decode one u32 at a time instead of copying a 56-byte prefix.
    reg [31:0] packet_word;
    wire [31:0] capture_word_data = {payload_data,packet_word[31:8]};
    reg capture_invalid, copy_command_invalid;
    reg [5:0] capture_index, capture_length;

    wire scan_cmd_valid, scan_cmd_write, scan_r_ready;
    wire [22:0] scan_cmd_addr;
    wire [4:0] scan_cmd_words;
    wire [31:0] scanout_underflows;
    wire vblank_pulse;
    wire tex_cmd_valid, tex_cmd_write, tex_w_valid, tex_r_ready;
    wire [22:0] tex_cmd_addr;
    wire [4:0] tex_cmd_words;
    wire [31:0] tex_w_data;
    wire [3:0] tex_w_strb;
    wire tex_cfg_valid, tex_cfg_ready, tex_cfg_repeat_u, tex_cfg_repeat_v;
    wire [22:0] tex_cfg_base, tex_cfg_pitch;
    wire [10:0] tex_cfg_width, tex_cfg_height;
    wire tex_req_valid, tex_req_ready, tex_rsp_valid, tex_rsp_ready, tex_fault;
    wire signed [31:0] tex_req_x, tex_req_y;
    wire [5:0] tex_req_tag, tex_rsp_tag;
    wire [15:0] tex_rsp_texel;
    reg tex_flush;

    reg tile_cmd_word_we, tile_cmd_valid, tile_pix_we, tile_pix_re;
    reg [6:0] tile_cmd_word_index;
    reg [31:0] tile_cmd_word_data;
    wire tile_cmd_ready, tile_busy, tile_done, tile_fault, tile_pix_rvalid;
    reg [9:0] tile_pix_addr, tile_pix_raddr;
    reg [15:0] tile_pix_color;
    reg [23:0] tile_pix_depth;
    wire [15:0] tile_pix_rcolor;
    wire [23:0] tile_pix_rdepth;

    reg descriptor_invalid;
    reg target_id, clear_color_enable, clear_depth_enable;
    reg [15:0] clear_color;
    reg [23:0] clear_depth;
    reg [22:0] table_address, list_address, record_address;
    reg [20:0] reference_count, reference_index, references_seen;
    reg [4:0] tile_x_index;
    reg [3:0] tile_y_index;
    wire [9:0] tile_origin_x = {tile_x_index,5'b0};
    wire [8:0] tile_origin_y = {tile_y_index,5'b0};
    reg [8:0] tile_index;
    reg [10:0] pixel_index;
    reg [6:0] record_word;
    reg [4:0] beat;
    reg [4:0] row;
    reg [1:0] store_kind;
    reg [15:0] held_color;
    reg [23:0] held_depth;
    reg [31:0] packed_word;
    reg [22:0] boot_address;

    reg [22:0] upload_address;
    reg [10:0] upload_length, upload_cursor;
    reg [6:0] upload_chunk, upload_fill;
    reg [1:0] upload_prefix;
    reg [4:0] upload_words;
    reg [3:0] packed_mask;
    wire [22:0] upload_current = upload_address + {12'd0,upload_cursor};
    wire [10:0] upload_remaining = upload_length - upload_cursor;
    wire [6:0] upload_room = 7'd64 - {5'd0,upload_current[1:0]};
    wire [6:0] upload_next_chunk = upload_remaining > {4'd0,upload_room} ?
        upload_room : upload_remaining[6:0];
    wire [6:0] upload_rounded_bytes = {5'd0,upload_current[1:0]} + upload_next_chunk + 7'd3;
    wire [1:0] upload_lane = (upload_prefix + upload_fill) & 2'b11;
    // Finish fetching a bounded host chunk before reserving the shared SDRAM
    // service. Slow byte reads must never hold its sole transaction owner and
    // starve the independent display FIFO. One native bank stores the sixteen
    // u32 payload words as pairs of u16. Their four-bit masks occupy unused
    // native words32..47, avoiding a duplicated64-bit register/mux bank.
    reg upload_ram_we, upload_ram_re;
    reg [9:0] upload_ram_waddr, upload_ram_raddr;
    reg [15:0] upload_ram_wdata;
    wire [15:0] upload_ram_rdata;

    reg [22:0] read_address;
    reg [9:0] read_length, read_cursor;
    reg [22:0] read_next_address;
    reg [7:0] read_words_remaining;
    reg [4:0] read_burst_words;
    reg [31:0] read_word_hold;
    reg [1:0] read_lane;
    // Full request bounds are checked before these narrowed transfer counters
    // are used. A read of 1..512 bytes spans at most 129 aligned u32 words.
    wire [9:0] read_initial_rounded = read_length + {8'd0,read_address[1:0]} + 10'd3;
    wire [7:0] read_initial_words = read_initial_rounded[9:2];
    wire [7:0] read_words_after_burst = read_words_remaining - {3'd0,read_burst_words};
    reg read_first, read_done_hold;
    reg [10:0] reply_index, reply_length;
    reg [31:0] reply_token;
    reg reply_caps;
    reg [63:0] reply_cycle_snapshot;
    reg [31:0] reply_underflow_snapshot;
    reg [7:0] reply_flags_snapshot, reply_host_fault_snapshot;
    reg reply_has_snapshot, reply_has_readback;
    wire [10:0] reply_payload_bytes = reply_length - 11'd16;
    reg [7:0] reply_data_hold;
    reg reply_ram_we, reply_ram_re;
    reg [9:0] reply_ram_waddr, reply_ram_raddr;
    reg [15:0] reply_ram_wdata;
    wire [15:0] reply_ram_rdata;

    reg copy_source_kind, copy_source_id, copy_target_id;
    reg copy_reverse;
    reg copy_previous_valid;
    reg [22:0] copy_base, copy_pitch;
    reg [10:0] copy_image_width, copy_image_height;
    reg [22:0] copy_source_row, copy_destination_row;
    reg signed [32:0] copy_low, copy_high;
    reg [3:0] copy_setup_step;
    wire copy_clip_minimum = copy_setup_step==3 || copy_setup_step==4 ||
        copy_setup_step==10 || copy_setup_step==11;
    wire signed [32:0] copy_raw_coordinate = {packet_word[31],packet_word};
    reg signed [32:0] copy_clip_left, copy_clip_right, copy_clip_bound;
    wire signed [32:0] copy_clip_candidate = copy_clip_left - copy_clip_right;
    wire copy_clip_less = copy_clip_candidate < copy_clip_bound;
    wire copy_clip_equal = copy_clip_candidate == copy_clip_bound;
    wire copy_clip_better = copy_clip_minimum ? copy_clip_less :
        !(copy_clip_less || copy_clip_equal);
    reg [10:0] copy_count_x, copy_count_y, copy_column, copy_row_number;
    reg [10:0] copy_address_steps, copy_extent_steps;
    reg [23:0] copy_extent_address;
    reg [15:0] copy_pixel_hold;
    reg copy_ram_we, copy_ram_re;
    reg [9:0] copy_ram_waddr, copy_ram_raddr;
    reg [15:0] copy_ram_wdata;
    wire [15:0] copy_ram_rdata;
    wire [22:0] copy_pixel_source_address = copy_source_row + {11'd0,copy_column,1'b0};
    wire [22:0] copy_pixel_destination_address = copy_destination_row + {11'd0,copy_column,1'b0};

    // These bounded origins are retained only after complete signed clipping.
    // Raw coordinates/extents remain in the owned native packet bank and are
    // re-read when setup needs them, including INT32_MIN/MAX cases.
    reg [10:0] copy_first_source_x, copy_first_source_y;
    reg [9:0] copy_first_destination_x;
    reg [8:0] copy_first_destination_y;
    wire [10:0] copy_source_start_y = copy_first_source_y + copy_row_number;
    wire copy_should_reverse = !copy_source_kind && copy_source_id==copy_target_id &&
        {2'd0,copy_first_destination_y}>copy_first_source_y;
    wire [8:0] copy_last_destination_y = copy_first_destination_y + copy_count_y - 11'd1;
    wire [8:0] copy_destination_start_y = copy_should_reverse ? copy_last_destination_y : copy_first_destination_y;
    wire [18:0] copy_destination_start_pixel = global_pixel(copy_first_destination_x,copy_destination_start_y);
    wire [21:0] pass_reference_total = {1'b0,references_seen} + {1'b0,reference_count};

    integer i;
    function automatic [5:0] copy_parameter_offset(input [3:0] step);
        begin
            case(step)
            0: copy_parameter_offset=28; // requested width
            1,3,5: copy_parameter_offset=12; // source X
            2,4,6: copy_parameter_offset=20; // destination X
            7: copy_parameter_offset=32; // requested height
            8,10,12: copy_parameter_offset=16; // source Y
            9,11,13: copy_parameter_offset=24; // destination Y
            default: copy_parameter_offset=0;
            endcase
        end
    endfunction
    function automatic [22:0] color_base(input id);
        color_base = id ? COLOR_BYTES : 23'd0;
    endfunction
    function automatic [22:0] global_pixel(input [9:0] x, input [8:0] y);
        global_pixel = ({14'd0,y} << 9) + ({14'd0,y} << 7) + {13'd0,x};
    endfunction
    function automatic valid_heap(input [31:0] address, input [32:0] length, input aligned);
        // Any accepted range starts above zero, so neither its address nor its
        // length can reach 8 MiB. Reject high bits before the 24-bit sum: two
        // 23-bit operands cannot overflow this checked addition.
        valid_heap = !(|address[31:23]) && address[22:0] >= HEAP_BASE && !(|length[32:23]) &&
                     {1'b0,address[22:0]}+{1'b0,length[22:0]} <= 24'h800000 &&
                     (!aligned || address[1:0] == 0);
    endfunction
    function automatic [7:0] reply_snapshot_byte(input [5:0] index);
        reg [511:0] held_status;
        begin
            // Ordered query execution blocks all foreground state changes.
            // Capture the independently advancing counters and status flags;
            // the remaining fields are stable until this reply retires.
            // Abort/fault abandons an incomplete reply before reusing state.
            held_status=status_snapshot;
            held_status[8 +:8]=reply_flags_snapshot;
            held_status[128 +:64]=reply_cycle_snapshot;
            held_status[192 +:64]={32'd0,reply_underflow_snapshot};
            held_status[448 +:8]=reply_host_fault_snapshot;
            // Serialize each source independently before selecting eight bits;
            // immutable capabilities never need a duplicated snapshot bank.
            if(reply_caps) reply_snapshot_byte=capabilities[{index,3'b0} +:8];
            else reply_snapshot_byte=held_status[{index,3'b0} +:8];
        end
    endfunction
    function automatic [7:0] reply_byte(input [10:0] index);
        reg [31:0] word_value;
        begin
            word_value=0;
            if (index<4) word_value=32'h5550474e;
            else if(index<8) word_value={opcode|16'h8000,8'd1,8'd0};
            else if(index<12) word_value={21'd0,reply_payload_bytes};
            else if(index<16) word_value=command_sequence;
            else if(index<20) word_value=epoch;
            else if(index<24) word_value=0;
            else if(index<28) word_value=reply_token;
            else if(reply_has_readback && index<32) word_value=read_address;
            else if(reply_has_readback && index<36) word_value={22'd0,read_length};
            if(reply_has_snapshot && index>=28 && index<92)
                reply_byte=reply_snapshot_byte(index-11'd28);
            else reply_byte=word_value[(index[1:0])*8 +:8];
        end
    endfunction
    task automatic complete_command;
        begin
            completed_sequence<=command_sequence;
            host_cmd_pop<=1;
            state<=S_IDLE;
        end
    endtask
    task automatic raise_fault(input [7:0] code);
        begin
            if(!fault_pending) fault_code<=code;
            fault_pending<=1;
            if(pass_active) begin color_valid[target_id]<=0; depth_valid<=0; end
            if(copy_active) color_valid[copy_target_id]<=0;
            sealed<=0;
        end
    endtask
    task automatic start_reply(input [31:0] token, input snapshot, input readback);
        begin
            reply_token<=token; reply_has_snapshot<=snapshot; reply_has_readback<=readback;
            reply_length<=readback ? 11'd36+{1'b0,read_length} : (snapshot ? 11'd92 : 11'd28);
            reply_index<=0; state<=S_REPLY;
        end
    endtask
    task automatic next_tile;
        begin
            if(tile_index==299) begin
                color_valid[target_id]<=1; depth_valid<=1; pass_active<=0;
                complete_command();
            end else begin
                tile_index<=tile_index+1;
                if(tile_x_index==19) begin tile_x_index<=0; tile_y_index<=tile_y_index+1; end
                else tile_x_index<=tile_x_index+1;
                state<=S_ENTRY_CMD;
            end
        end
    endtask

    // COPY setup is off the fragment path. Each candidate reuses one signed
    // subtract/compare datapath and one raw word fetched from native packet RAM.
    always @* begin
        copy_clip_left=0; copy_clip_right=copy_raw_coordinate; copy_clip_bound=copy_low;
        case(copy_setup_step)
        3: begin copy_clip_left=$signed({22'd0,copy_image_width}); copy_clip_bound=copy_high; end
        4: begin copy_clip_left=33'sd640; copy_clip_bound=copy_high; end
        10: begin copy_clip_left=$signed({22'd0,copy_image_height}); copy_clip_bound=copy_high; end
        11: begin copy_clip_left=33'sd480; copy_clip_bound=copy_high; end
        default: begin end
        endcase
    end

    always @* begin
        status_snapshot=0; capabilities=0;
        status_snapshot[8 +:8]={4'b0,fault,sealed,busy,ready};
        status_snapshot[16 +:32]=epoch; status_snapshot[48 +:32]=completed_sequence;
        status_snapshot[80 +:32]=fence_token; status_snapshot[112 +:8]={7'd0,front_id};
        status_snapshot[120 +:8]={5'd0,depth_valid,color_valid};
        status_snapshot[128 +:64]=cycle_counter;
        status_snapshot[192 +:64]={32'd0,scanout_underflows};
        status_snapshot[256 +:32]={24'd0,fault_code};
        status_snapshot[288 +:32]=present_token;
        status_snapshot[448 +:8]=host_fault;
        capabilities[0 +:32]=32'h20080100;
        capabilities[32 +:32]=MEMORY_BYTES;
        capabilities[64 +:32]={9'd0,HEAP_BASE};
        capabilities[96 +:32]=1024; capabilities[128 +:32]=320;
        capabilities[160 +:32]=1048576; capabilities[192 +:32]=100800000;
        capabilities[224 +:32]=4; capabilities[256 +:32]=4;
        capabilities[288 +:32]=256; capabilities[320 +:32]=2;
        capabilities[352 +:32]=1024; capabilities[384 +:32]=2048;
        capabilities[416 +:32]=640; capabilities[448 +:32]=480;
        capabilities[480 +:32]=32'hff;
    end

    wire [22:0] current_global_pixel = global_pixel(tile_origin_x+{5'd0,pixel_index[4:0]},
        tile_origin_y+{4'd0,pixel_index[9:5]});
    wire [22:0] current_row_pixel = global_pixel(tile_origin_x,tile_origin_y+{4'd0,row});
    wire [22:0] store_address = store_kind==0 ? color_base(target_id)+(current_row_pixel<<1) :
        DEPTH_BASE+(current_row_pixel<<2)+(store_kind==2 ? 23'd64 : 23'd0);
    wire [9:0] store_pixel_address = {row,5'd0}+
        (store_kind==0 ? {4'd0,beat,1'b0} : (store_kind==2 ? 10'd16 : 10'd0)+{5'd0,beat});

    always @* begin
        mem_cmd_valid=0; mem_cmd_write=0; mem_cmd_addr=0; mem_cmd_words=0;
        mem_w_valid=0; mem_w_data=0; mem_w_strb=0; mem_r_ready=0;
        mem_cmd_valid[0]=scan_cmd_valid; mem_cmd_write[0]=scan_cmd_write;
        mem_cmd_addr[0 +:23]=scan_cmd_addr; mem_cmd_words[0 +:5]=scan_cmd_words;
        mem_r_ready[0]=scan_r_ready;
        mem_cmd_valid[4]=tex_cmd_valid; mem_cmd_write[4]=tex_cmd_write;
        mem_cmd_addr[4*23 +:23]=tex_cmd_addr; mem_cmd_words[4*5 +:5]=tex_cmd_words;
        mem_w_valid[4]=tex_w_valid; mem_w_data[4*32 +:32]=tex_w_data; mem_w_strb[4*4 +:4]=tex_w_strb;
        mem_r_ready[4]=tex_r_ready;
        payload_req=0; payload_offset=0; response_valid=0; response_data=0;
        tile_cmd_word_we=0; tile_cmd_word_index=record_word+{2'd0,beat}; tile_cmd_word_data=mem_r_data[2*32 +:32];
        tile_cmd_valid=0; tile_pix_we=0; tile_pix_addr=pixel_index[9:0];
        tile_pix_color=clear_color; tile_pix_depth=clear_depth;
        tile_pix_re=0; tile_pix_raddr=store_pixel_address;
        reply_ram_we=0; reply_ram_re=0; reply_ram_waddr=read_cursor;
        reply_ram_raddr=reply_index-36; reply_ram_wdata={8'd0,read_word_hold[read_lane*8 +:8]};
        copy_ram_we=0; copy_ram_re=0; copy_ram_waddr=copy_column[9:0];
        copy_ram_raddr=copy_column[9:0];
        copy_ram_wdata=copy_pixel_source_address[1] ? mem_r_data[5*32+16 +:16] : mem_r_data[5*32 +:16];
        upload_ram_we=0; upload_ram_re=0;
        upload_ram_waddr={5'd0,beat[3:0],1'b0};
        upload_ram_raddr={5'd0,beat[3:0],1'b0};
        upload_ram_wdata=packed_word[15:0];
        case(state)
        S_BOOT_CMD: begin
            mem_cmd_valid[3]=1; mem_cmd_write[3]=1;
            mem_cmd_addr[3*23 +:23]=boot_address; mem_cmd_words[3*5 +:5]=16;
        end
        S_BOOT_DATA: begin
            mem_w_valid[3]=1; mem_w_data[3*32 +:32]=boot_address>=DEPTH_BASE ? 32'h00ffffff : 0;
            mem_w_strb[3*4 +:4]=15;
        end
        S_CAPTURE_REQ,S_COPY_FETCH_REQ: begin payload_req=1; payload_offset={4'd0,capture_index}; end
        S_UPLOAD_CMD: begin
            mem_cmd_valid[1]=1; mem_cmd_write[1]=1;
            mem_cmd_addr[23 +:23]=upload_current[22:0]&23'h7ffffc; mem_cmd_words[5 +:5]=upload_words;
        end
        S_UPLOAD_BYTE_REQ: begin payload_req=1; payload_offset=10'd8+upload_cursor[9:0]+upload_fill; end
        S_UPLOAD_STAGE_LOW: upload_ram_we=1;
        S_UPLOAD_STAGE_HIGH: begin
            upload_ram_we=1; upload_ram_waddr={5'd0,beat[3:0],1'b1};
            upload_ram_wdata=packed_word[31:16];
        end
        S_UPLOAD_STAGE_MASK: begin
            upload_ram_we=1; upload_ram_waddr={6'd2,beat[3:0]};
            upload_ram_wdata={12'd0,packed_mask};
        end
        S_UPLOAD_RAM_LOW: upload_ram_re=1;
        S_UPLOAD_RAM_HIGH: begin upload_ram_re=1; upload_ram_raddr={5'd0,beat[3:0],1'b1}; end
        S_UPLOAD_RAM_MASK: begin upload_ram_re=1; upload_ram_raddr={6'd2,beat[3:0]}; end
        S_UPLOAD_WORD: begin
            mem_w_valid[1]=1;
            mem_w_data[32 +:32]=packed_word;
            mem_w_strb[4 +:4]=upload_ram_rdata[3:0];
            // A successful handoff frees the read port to prefetch the next
            // low half. Until then the native mask and packed word remain stable.
            if(mem_w_ready[1] && beat+1!=upload_words) begin
                upload_ram_re=1;
                upload_ram_raddr={5'd0,(beat[3:0]+4'd1),1'b0};
            end
        end
        S_DESC_CMD: begin
            mem_cmd_valid[2]=1; mem_cmd_addr[46 +:23]=upload_address; mem_cmd_words[10 +:5]=8;
        end
        S_ENTRY_CMD: begin
            mem_cmd_valid[2]=1; mem_cmd_addr[46 +:23]=table_address+{11'd0,tile_index,3'b0}; mem_cmd_words[10 +:5]=2;
        end
        S_REF_CMD: begin
            mem_cmd_valid[2]=1; mem_cmd_addr[46 +:23]=list_address+{reference_index,2'b0}; mem_cmd_words[10 +:5]=1;
        end
        S_RECORD_CMD: begin
            mem_cmd_valid[2]=1; mem_cmd_addr[46 +:23]=record_address+{14'd0,record_word,2'b0}; mem_cmd_words[10 +:5]=16;
        end
        S_DESC_DATA,S_ENTRY_DATA,S_REF_DATA: mem_r_ready[2]=1;
        S_RECORD_DATA: begin mem_r_ready[2]=1; tile_cmd_word_we=mem_r_valid[2]; end
        S_TILE_INIT: tile_pix_we=1;
        S_LOAD_COLOR_CMD: begin
            mem_cmd_valid[3]=1; mem_cmd_addr[69 +:23]=(color_base(target_id)+(current_global_pixel<<1))&23'h7ffffc;
            mem_cmd_words[15 +:5]=1;
        end
        S_LOAD_DEPTH_CMD: begin
            mem_cmd_valid[3]=1; mem_cmd_addr[69 +:23]=DEPTH_BASE+(current_global_pixel<<2);
            mem_cmd_words[15 +:5]=1;
        end
        S_LOAD_COLOR_DATA,S_LOAD_DEPTH_DATA: mem_r_ready[3]=1;
        S_LOAD_PIXEL: begin
            tile_pix_we=1;
            tile_pix_color=clear_color_enable ? clear_color : held_color;
            tile_pix_depth=clear_depth_enable ? clear_depth : held_depth;
        end
        S_DRAW_START: tile_cmd_valid=1;
        S_STORE_CMD: begin
            mem_cmd_valid[3]=1; mem_cmd_write[3]=1; mem_cmd_addr[69 +:23]=store_address;
            mem_cmd_words[15 +:5]=16;
        end
        S_STORE_PIXEL_REQ: tile_pix_re=1;
        S_STORE_SECOND_REQ: begin tile_pix_re=1; tile_pix_raddr=store_pixel_address+1; end
        S_STORE_WORD: begin mem_w_valid[3]=1; mem_w_data[96 +:32]=packed_word; mem_w_strb[12 +:4]=15; end
        S_REPLY: begin
            response_valid=1;
            response_data=(reply_has_readback && reply_index>=36) ? reply_data_hold : reply_byte(reply_index);
        end
        S_REPLY_RAM_REQ: reply_ram_re=1;
        S_READ_CMD: begin
            mem_cmd_valid[2]=1; mem_cmd_addr[46 +:23]=read_next_address;
            mem_cmd_words[10 +:5]=read_burst_words;
        end
        S_READ_DATA: mem_r_ready[2]=1;
        S_READ_BYTES: reply_ram_we=read_cursor<read_length;
        S_COPY_READ_CMD: begin
            mem_cmd_valid[5]=1; mem_cmd_addr[115 +:23]=copy_pixel_source_address&23'h7ffffc;
            mem_cmd_words[25 +:5]=1;
        end
        S_COPY_READ_DATA: begin mem_r_ready[5]=1; copy_ram_we=mem_r_valid[5]; end
        S_COPY_RAM_REQ: copy_ram_re=1;
        S_COPY_WRITE_CMD: begin
            mem_cmd_valid[5]=1; mem_cmd_write[5]=1;
            mem_cmd_addr[115 +:23]=copy_pixel_destination_address&23'h7ffffc; mem_cmd_words[25 +:5]=1;
        end
        S_COPY_WRITE_DATA: begin
            mem_w_valid[5]=1;
            mem_w_data[160 +:32]=copy_pixel_destination_address[1] ? {copy_pixel_hold,16'd0} : {16'd0,copy_pixel_hold};
            mem_w_strb[20 +:4]=copy_pixel_destination_address[1] ? 4'b1100 : 4'b0011;
        end
        default: begin end
        endcase
        // Stop issuing foreground work during recovery, but continue the
        // accepted payload/response handshakes so native transactions drain.
        if(abort_pending || fault_pending) begin
            mem_cmd_valid[1]=0; mem_cmd_valid[2]=0; mem_cmd_valid[3]=0; mem_cmd_valid[5]=0;
            tile_cmd_valid=0;
        end
    end

    always @(posedge clk) begin
        if(reset) begin
            state<=S_BOOT_WAIT; initialized<=0; front_id<=0; sealed<=0; color_valid<=0; depth_valid<=0;
            pass_active<=0; copy_active<=0; epoch<=1; completed_sequence<=0; fence_token<=0; present_token<=0;
            cycle_counter<=0; pixel_phase<=0; video_pixel_ce<=0;
            abort_sync1<=0; abort_sync2<=0; abort_previous<=0; abort_pending<=0; fault_pending<=0; fault_code<=0;
            renderer_reset<=0; host_abort<=0; host_cmd_pop<=0; tex_flush<=0; pending_memory<=0;
            capture_index<=0; capture_length<=0;
            packet_word<=0; capture_invalid<=0; copy_command_invalid<=0;
            target_id<=1; clear_color_enable<=1; clear_depth_enable<=1; clear_color<=0; clear_depth<=24'hffffff;
            descriptor_invalid<=0;
            table_address<=0; list_address<=0; record_address<=0; reference_count<=0; reference_index<=0; references_seen<=0;
            tile_x_index<=0; tile_y_index<=0; tile_index<=0; pixel_index<=0; record_word<=0; beat<=0; row<=0; store_kind<=0;
            held_color<=0; held_depth<=0; packed_word<=0; packed_mask<=0; boot_address<=0;
            upload_address<=0; upload_length<=0; upload_cursor<=0; upload_chunk<=0; upload_fill<=0; upload_prefix<=0; upload_words<=0;
            read_address<=0; read_length<=0; read_cursor<=0; read_next_address<=0;
            read_words_remaining<=0; read_burst_words<=0; read_word_hold<=0; read_lane<=0; read_first<=0; read_done_hold<=0;
            reply_index<=0; reply_length<=0; reply_token<=0; reply_caps<=0;
            reply_cycle_snapshot<=0; reply_underflow_snapshot<=0;
            reply_flags_snapshot<=0; reply_host_fault_snapshot<=0;
            reply_has_snapshot<=0; reply_has_readback<=0; reply_data_hold<=0;
            copy_source_kind<=0; copy_source_id<=0; copy_target_id<=0;
            copy_base<=0; copy_pitch<=0; copy_image_width<=0; copy_image_height<=0;
            copy_reverse<=0; copy_previous_valid<=0; copy_source_row<=0; copy_destination_row<=0;
            copy_low<=0; copy_high<=0; copy_setup_step<=0;
            copy_first_source_x<=0; copy_first_source_y<=0;
            copy_first_destination_x<=0; copy_first_destination_y<=0;
            copy_count_x<=0; copy_count_y<=0; copy_column<=0; copy_row_number<=0;
            copy_address_steps<=0; copy_extent_steps<=0; copy_extent_address<=0; copy_pixel_hold<=0;
        end else begin
            cycle_counter<=cycle_counter+1;
            pixel_phase<=pixel_phase+1; video_pixel_ce<=pixel_tick;
            renderer_reset<=0; host_abort<=0; host_cmd_pop<=0; tex_flush<=0;
            abort_sync1<=abort_request; abort_sync2<=abort_sync1; abort_previous<=abort_sync2;
            if(abort_sync2 && !abort_previous) begin
                abort_pending<=1;
                if(pass_active) begin color_valid[target_id]<=0; depth_valid<=0; end
                if(copy_active) color_valid[copy_target_id]<=0;
                sealed<=0;
            end
            for(i=0;i<6;i=i+1) begin
                if(mem_done[i]) pending_memory[i]<=0;
                if(mem_cmd_valid[i] && mem_cmd_ready[i]) pending_memory[i]<=1;
            end
            if(mem_done[2]) read_done_hold<=1;
            if(!abort_pending && state!=S_RECOVER && !host_abort && ((|mem_error) || host_fault!=0 || tile_fault || tex_fault))
                raise_fault(host_fault!=0 ? 8'd1 : ((|mem_error) ? 8'd8 : 8'd6));
            if(abort_pending && !tile_busy && pending_memory[5:1]==0 && !foreground_command_accepted) begin
                state<=S_RECOVER; renderer_reset<=1; host_abort<=1;
                abort_pending<=0; fault_pending<=0; fault_code<=0;
                completed_sequence<=0; fence_token<=0; present_token<=0;
                pass_active<=0; copy_active<=0; sealed<=0;
                if(epoch==32'hffffffff) begin fault_pending<=1; fault_code<=8'hff; state<=S_FAULT; host_abort<=0; end
                else epoch<=epoch+1;
            end else if(fault_pending && pending_memory[5:1]==0 && !foreground_command_accepted) begin
                state<=S_FAULT; renderer_reset<=1;
            end else begin
                case(state)
                S_BOOT_WAIT: if(memory_initialized) begin boot_address<=0; state<=S_BOOT_CMD; end
                S_BOOT_CMD: if(mem_cmd_ready[3]) begin beat<=0; state<=S_BOOT_DATA; end
                S_BOOT_DATA: if(mem_w_ready[3]) begin if(beat==15) state<=S_BOOT_DONE; else beat<=beat+1; end
                S_BOOT_DONE: if(mem_done[3]) begin
                    if(boot_address+64==HEAP_BASE) begin initialized<=1; color_valid<=3; depth_valid<=1; state<=S_IDLE; end
                    else begin boot_address<=boot_address+64; state<=S_BOOT_CMD; end
                end
                S_IDLE: if(initialized && !host_recovering && host_cmd_valid && !host_cmd_pop) begin
                    capture_index<=0; capture_invalid<=0; copy_command_invalid<=0;
                    case(host_cmd_opcode)
                    1,2: begin capture_length<=0; if(host_cmd_length!=0) raise_fault(2); else state<=S_EXECUTE; end
                    3: begin capture_length<=8; if(host_cmd_length<=8 || host_cmd_length>1024) raise_fault(2); else state<=S_CAPTURE_REQ; end
                    4,6: begin capture_length<=4; if(host_cmd_length!=4) raise_fault(2); else state<=S_CAPTURE_REQ; end
                    5: begin capture_length<=56; if(host_cmd_length!=56) raise_fault(2); else state<=S_CAPTURE_REQ; end
                    7: begin capture_length<=8; if(host_cmd_length!=8) raise_fault(2); else state<=S_CAPTURE_REQ; end
                    8: begin capture_length<=12; if(host_cmd_length!=12) raise_fault(2); else state<=S_CAPTURE_REQ; end
                    default: raise_fault(2);
                    endcase
                end
                S_CAPTURE_REQ: state<=S_CAPTURE_WAIT;
                S_CAPTURE_WAIT: if(payload_valid) begin
                    packet_word<=capture_word_data;
                    if(capture_index[1:0]==3) case(opcode)
                    3: case(capture_index[5:2])
                        0: begin
                            upload_address<=capture_word_data[22:0];
                            if(!valid_heap(capture_word_data,33'd1,0)) capture_invalid<=1;
                        end
                        1: begin
                            upload_length<=capture_word_data[10:0];
                            if(capture_word_data!={21'd0,payload_length}-32'd8 || capture_word_data==0 ||
                               {1'b0,upload_address}+{13'd0,capture_word_data[10:0]}>24'h800000) capture_invalid<=1;
                        end
                        default: capture_invalid<=1;
                    endcase
                    4: begin
                        upload_address<=capture_word_data[22:0];
                        if(!valid_heap(capture_word_data,33'd32,1)) capture_invalid<=1;
                    end
                    7: if(capture_index[5:2]==0) begin
                        target_id<=capture_word_data[0];
                        if(|capture_word_data[31:1]) capture_invalid<=1;
                    end
                    8: case(capture_index[5:2])
                        0: begin
                            read_address<=capture_word_data[22:0];
                            if(|capture_word_data[31:23]) capture_invalid<=1;
                        end
                        1: begin
                            read_length<=capture_word_data[9:0];
                            if(capture_word_data==0 || capture_word_data>512 ||
                               {1'b0,read_address}+{14'd0,capture_word_data[9:0]}>24'h800000) capture_invalid<=1;
                        end
                        default: begin end // token is the final packet_word
                    endcase
                    5: case(capture_index[5:2])
                        0: begin
                            copy_source_kind<=capture_word_data[0];
                            if(|capture_word_data[31:1]) copy_command_invalid<=1;
                        end
                        1: begin
                            copy_source_id<=capture_word_data[0];
                            if((|capture_word_data[31:1]) || (copy_source_kind && capture_word_data!=0)) copy_command_invalid<=1;
                        end
                        2: begin
                            copy_target_id<=capture_word_data[0];
                            if(|capture_word_data[31:1]) copy_command_invalid<=1;
                        end
                        7,8: if(capture_word_data[31]) copy_command_invalid<=1;
                        9: begin
                            copy_base<=copy_source_kind ? capture_word_data[22:0] : color_base(copy_source_id);
                            if(copy_source_kind) begin
                                if(!valid_heap(capture_word_data,33'd1,1)) capture_invalid<=1;
                            end else if(capture_word_data!=0) copy_command_invalid<=1;
                        end
                        10: begin
                            copy_image_width<=copy_source_kind ? capture_word_data[10:0] : 11'd640;
                            if(copy_source_kind) begin
                                if(capture_word_data==0 || capture_word_data>1024) capture_invalid<=1;
                            end else if(capture_word_data!=0) copy_command_invalid<=1;
                        end
                        11: begin
                            copy_image_height<=copy_source_kind ? capture_word_data[10:0] : 11'd480;
                            if(copy_source_kind) begin
                                if(capture_word_data==0 || capture_word_data>1024) capture_invalid<=1;
                            end else if(capture_word_data!=0) copy_command_invalid<=1;
                        end
                        12: begin
                            copy_pitch<=copy_source_kind ? capture_word_data[22:0] : 23'd1280;
                            if(copy_source_kind) begin
                                if(capture_word_data[1:0]!=0 || (|capture_word_data[31:23]) ||
                                   capture_word_data[22:0] < {11'd0,copy_image_width,1'b0}) capture_invalid<=1;
                            end else if(capture_word_data!=0) copy_command_invalid<=1;
                        end
                        13: if(capture_word_data!=0) copy_command_invalid<=1;
                        default: begin end // coordinate words are re-fetched for setup
                    endcase
                    default: begin end
                    endcase
                    if(capture_index+1==capture_length) state<=S_EXECUTE;
                    else begin capture_index<=capture_index+1; state<=S_CAPTURE_REQ; end
                end
                S_EXECUTE: case(opcode)
                    1: begin reply_caps<=1; start_reply(0,1,0); end
                    2: begin
                        reply_caps<=0; reply_cycle_snapshot<=cycle_counter;
                        reply_underflow_snapshot<=scanout_underflows;
                        reply_flags_snapshot<=status_snapshot[8 +:8];
                        reply_host_fault_snapshot<=host_fault;
                        start_reply(0,1,0);
                    end
                    3: if(capture_invalid) raise_fault(3);
                       else begin upload_cursor<=0; state<=S_UPLOAD_PREP; end
                    4: if(capture_invalid) raise_fault(3);
                       else begin tex_flush<=1; state<=S_DESC_CMD; end
                    6: begin fence_token<=packet_word; start_reply(packet_word,0,0); end
                    7: if(capture_invalid || target_id==front_id || sealed || !color_valid[target_id]) raise_fault(4);
                       else begin sealed<=1; state<=S_PRESENT; end
                    8: if(capture_invalid) raise_fault(3);
                       else begin
                           read_cursor<=0; read_next_address<=read_address&23'h7ffffc;
                           read_words_remaining<=read_initial_words;
                           read_burst_words<=read_initial_words>8'd16 ? 5'd16 : read_initial_words[4:0];
                           read_first<=1; state<=S_READ_CMD;
                       end
                    5: if(copy_command_invalid || copy_target_id==front_id || sealed) raise_fault(4);
                       else begin
                           copy_previous_valid<=color_valid[copy_target_id];
                           copy_extent_steps<=0; copy_extent_address<=copy_source_kind ? {1'b0,copy_base} : 0;
                           state<=S_COPY_VALIDATE;
                       end
                    default: raise_fault(2);
                endcase
                S_UPLOAD_PREP: begin
                    upload_prefix<=upload_current[1:0];
                    upload_chunk<=upload_next_chunk;
                    upload_words<=upload_rounded_bytes[6:2];
                    upload_fill<=0; beat<=0; packed_word<=0; packed_mask<=0; state<=S_UPLOAD_BYTE_REQ;
                end
                S_UPLOAD_CMD: if(mem_cmd_ready[1]) state<=S_UPLOAD_RAM_LOW;
                S_UPLOAD_BYTE_REQ: state<=S_UPLOAD_BYTE_WAIT;
                S_UPLOAD_BYTE_WAIT: if(payload_valid) begin
                    packed_word[upload_lane*8 +:8]<=payload_data; packed_mask[upload_lane]<=1;
                    upload_fill<=upload_fill+1;
                    if(upload_lane==3 || upload_fill+1==upload_chunk) state<=S_UPLOAD_STAGE_LOW;
                    else state<=S_UPLOAD_BYTE_REQ;
                end
                S_UPLOAD_STAGE_LOW: state<=S_UPLOAD_STAGE_HIGH;
                S_UPLOAD_STAGE_HIGH: state<=S_UPLOAD_STAGE_MASK;
                S_UPLOAD_STAGE_MASK: begin
                    packed_word<=0; packed_mask<=0; beat<=beat+1;
                    if(upload_fill==upload_chunk) begin beat<=0; state<=S_UPLOAD_CMD; end
                    else state<=S_UPLOAD_BYTE_REQ;
                end
                S_UPLOAD_RAM_LOW: state<=S_UPLOAD_RAM_HIGH;
                S_UPLOAD_RAM_HIGH: begin packed_word[15:0]<=upload_ram_rdata; state<=S_UPLOAD_RAM_MASK; end
                S_UPLOAD_RAM_MASK: begin packed_word[31:16]<=upload_ram_rdata; state<=S_UPLOAD_WORD; end
                S_UPLOAD_WORD: if(mem_w_ready[1]) begin
                    if(beat+1==upload_words) state<=S_UPLOAD_DONE;
                    else begin beat<=beat+1; state<=S_UPLOAD_RAM_HIGH; end
                end
                S_UPLOAD_DONE: if(mem_done[1]) begin
                    upload_cursor<=upload_cursor+upload_chunk;
                    if(upload_cursor+upload_chunk==upload_length) complete_command(); else state<=S_UPLOAD_PREP;
                end
                S_DESC_CMD: if(mem_cmd_ready[2]) begin beat<=0; descriptor_invalid<=0; state<=S_DESC_DATA; end
                S_DESC_DATA: if(mem_r_valid[2]) begin
                    // Validate each full u32 on arrival, retaining only decoded
                    // pass fields. No attachment is mutated before DONE checks
                    // all words and ownership/LOAD validity below.
                    case(beat)
                    0: if(mem_r_data[64 +:32]!=32'h3153504e) descriptor_invalid<=1;
                    1: begin target_id<=mem_r_data[64]; if(mem_r_data[64 +:32]>1) descriptor_invalid<=1; end
                    2: begin
                        clear_color_enable<=mem_r_data[64]; clear_depth_enable<=mem_r_data[65];
                        if(mem_r_data[64 +:32]>3) descriptor_invalid<=1;
                    end
                    3: begin clear_color<=mem_r_data[64 +:16]; if(|mem_r_data[80 +:16]) descriptor_invalid<=1; end
                    4: begin clear_depth<=mem_r_data[64 +:24]; if(|mem_r_data[88 +:8]) descriptor_invalid<=1; end
                    5: begin
                        table_address<=mem_r_data[64 +:23];
                        if(!valid_heap(mem_r_data[64 +:32],33'd2400,1)) descriptor_invalid<=1;
                    end
                    6: if(mem_r_data[64 +:32]!=300) descriptor_invalid<=1;
                    7: if(mem_r_data[64 +:32]!=0) descriptor_invalid<=1;
                    default: descriptor_invalid<=1;
                    endcase
                    beat<=beat+1; if(mem_r_last[2]) state<=S_DESC_DONE;
                end
                S_DESC_DONE: if(mem_done[2]) begin
                    if(descriptor_invalid || target_id==front_id || sealed) raise_fault(6);
                    else if((!clear_color_enable && !color_valid[target_id]) || (!clear_depth_enable && !depth_valid)) raise_fault(5);
                    else begin
                        color_valid[target_id]<=0; depth_valid<=0; pass_active<=1;
                        tile_index<=0; tile_x_index<=0; tile_y_index<=0; references_seen<=0; state<=S_ENTRY_CMD;
                    end
                end
                S_ENTRY_CMD: if(mem_cmd_ready[2]) begin beat<=0; state<=S_ENTRY_DATA; end
                S_ENTRY_DATA: if(mem_r_valid[2]) begin
                    if(beat==0) list_address<=mem_r_data[64 +:23]; else reference_count<=mem_r_data[64 +:21];
                    if((beat==0 && |mem_r_data[64+23 +:9]) || (beat==1 && |mem_r_data[64+21 +:11])) raise_fault(6);
                    beat<=beat+1; if(mem_r_last[2]) state<=S_ENTRY_DONE;
                end
                S_ENTRY_DONE: if(mem_done[2]) begin
                    if((reference_count==0 && list_address!=0) || reference_count>21'd1048576 || pass_reference_total>22'd1048576 ||
                       (reference_count!=0 && !valid_heap({9'd0,list_address},{10'd0,reference_count,2'b0},1))) raise_fault(7);
                    else begin
                        references_seen<=pass_reference_total[20:0]; reference_index<=0; pixel_index<=0;
                        if(clear_color_enable && clear_depth_enable) state<=S_TILE_INIT;
                        else if(!clear_color_enable) state<=S_LOAD_COLOR_CMD; else state<=S_LOAD_DEPTH_CMD;
                    end
                end
                S_TILE_INIT: begin
                    if(pixel_index==1023) begin row<=0; beat<=0; store_kind<=0; if(reference_count==0) state<=S_STORE_CMD; else state<=S_REF_CMD; end
                    else pixel_index<=pixel_index+1;
                end
                S_LOAD_COLOR_CMD: if(mem_cmd_ready[3]) state<=S_LOAD_COLOR_DATA;
                S_LOAD_COLOR_DATA: if(mem_r_valid[3]) begin
                    held_color<=current_global_pixel[0] ? mem_r_data[96+16 +:16] : mem_r_data[96 +:16]; state<=S_LOAD_COLOR_DONE;
                end
                S_LOAD_COLOR_DONE: if(mem_done[3]) state<=clear_depth_enable ? S_LOAD_PIXEL : S_LOAD_DEPTH_CMD;
                S_LOAD_DEPTH_CMD: if(mem_cmd_ready[3]) state<=S_LOAD_DEPTH_DATA;
                S_LOAD_DEPTH_DATA: if(mem_r_valid[3]) begin held_depth<=mem_r_data[96 +:24]; state<=S_LOAD_DEPTH_DONE; end
                S_LOAD_DEPTH_DONE: if(mem_done[3]) state<=S_LOAD_PIXEL;
                S_LOAD_PIXEL: begin
                    if(pixel_index==1023) begin row<=0; beat<=0; store_kind<=0; if(reference_count==0) state<=S_STORE_CMD; else state<=S_REF_CMD; end
                    else begin pixel_index<=pixel_index+1; state<=clear_color_enable ? S_LOAD_DEPTH_CMD : S_LOAD_COLOR_CMD; end
                end
                S_REF_CMD: if(mem_cmd_ready[2]) state<=S_REF_DATA;
                S_REF_DATA: if(mem_r_valid[2]) begin
                    record_address<=mem_r_data[64 +:23];
                    if(!valid_heap(mem_r_data[64 +:32],33'd320,1)) raise_fault(3);
                    state<=S_REF_DONE;
                end
                S_REF_DONE: if(mem_done[2]) begin record_word<=0; state<=S_RECORD_CMD; end
                S_RECORD_CMD: if(mem_cmd_ready[2]) begin beat<=0; state<=S_RECORD_DATA; end
                S_RECORD_DATA: if(mem_r_valid[2]) begin beat<=beat+1; if(mem_r_last[2]) state<=S_RECORD_DONE; end
                S_RECORD_DONE: if(mem_done[2]) begin
                    if(record_word==64) state<=S_DRAW_START;
                    else begin record_word<=record_word+16; state<=S_RECORD_CMD; end
                end
                S_DRAW_START: if(tile_cmd_ready) state<=S_DRAW_WAIT;
                S_DRAW_WAIT: if(tile_done || !tile_busy) begin
                    if(reference_index+1==reference_count) begin row<=0; beat<=0; store_kind<=0; state<=S_STORE_CMD; end
                    else begin reference_index<=reference_index+1; state<=S_REF_CMD; end
                end
                S_STORE_CMD: if(mem_cmd_ready[3]) begin beat<=0; state<=S_STORE_PIXEL_REQ; end
                S_STORE_PIXEL_REQ: state<=S_STORE_PIXEL_WAIT;
                S_STORE_PIXEL_WAIT: if(tile_pix_rvalid) begin
                    if(store_kind==0) begin held_color<=tile_pix_rcolor; state<=S_STORE_SECOND_REQ; end
                    else begin packed_word<={8'd0,tile_pix_rdepth}; state<=S_STORE_WORD; end
                end
                S_STORE_SECOND_REQ: state<=S_STORE_SECOND_WAIT;
                S_STORE_SECOND_WAIT: if(tile_pix_rvalid) begin packed_word<={tile_pix_rcolor,held_color}; state<=S_STORE_WORD; end
                S_STORE_WORD: if(mem_w_ready[3]) begin if(beat==15) state<=S_STORE_DONE; else begin beat<=beat+1; state<=S_STORE_PIXEL_REQ; end end
                S_STORE_DONE: if(mem_done[3]) begin
                    if(store_kind!=2) begin store_kind<=store_kind+1; state<=S_STORE_CMD; end
                    else if(row!=31) begin row<=row+1; store_kind<=0; state<=S_STORE_CMD; end
                    else next_tile();
                end
                S_PRESENT: if(vblank_pulse) begin front_id<=target_id; present_token<=packet_word; sealed<=0; start_reply(packet_word,0,0); end
                S_REPLY: if(response_ready) begin
                    if(reply_index+1==reply_length) complete_command();
                    else begin
                        reply_index<=reply_index+1;
                        if(reply_has_readback && reply_index+1>=36) state<=S_REPLY_RAM_REQ;
                    end
                end
                S_REPLY_RAM_REQ: state<=S_REPLY_RAM_WAIT;
                S_REPLY_RAM_WAIT: begin reply_data_hold<=reply_ram_rdata[7:0]; state<=S_REPLY; end
                S_READ_CMD: if(mem_cmd_ready[2]) begin beat<=0; read_done_hold<=0; state<=S_READ_DATA; end
                S_READ_DATA: if(mem_r_valid[2]) begin
                    read_word_hold<=mem_r_data[64 +:32]; read_lane<=read_first ? read_address[1:0] : 0;
                    read_first<=0; beat<=beat+1; state<=S_READ_BYTES;
                end
                S_READ_BYTES: begin
                    if(read_cursor<read_length) read_cursor<=read_cursor+1;
                    if(read_lane==3 || read_cursor+1==read_length) begin
                        if(beat==read_burst_words) state<=S_READ_DONE; else state<=S_READ_DATA;
                    end else read_lane<=read_lane+1;
                end
                S_READ_DONE: if(mem_done[2] || read_done_hold) begin
                    if(read_words_remaining==read_burst_words) start_reply(packet_word,0,1);
                    else begin
                        read_words_remaining<=read_words_after_burst;
                        read_next_address<=read_next_address+{16'd0,read_burst_words,2'b0};
                        read_burst_words<=read_words_after_burst>8'd16 ? 5'd16 : read_words_after_burst[4:0];
                        state<=S_READ_CMD;
                    end
                end
                S_COPY_VALIDATE: begin
                    if(copy_source_kind && capture_invalid) raise_fault(3);
                    else if(!copy_source_kind && !color_valid[copy_source_id]) raise_fault(4);
                    else if(copy_source_kind && copy_extent_steps<copy_image_height) begin
                        copy_extent_address<=copy_extent_address+{1'b0,copy_pitch}; copy_extent_steps<=copy_extent_steps+1;
                        if(copy_extent_address+{1'b0,copy_pitch}>24'h800000) raise_fault(3);
                    end else begin
                        copy_low<=0; copy_setup_step<=0;
                        copy_address_steps<=0; copy_source_row<=copy_base; copy_row_number<=0; copy_column<=0;
                        capture_index<=copy_parameter_offset(0); state<=S_COPY_FETCH_REQ;
                    end
                end
                S_COPY_FETCH_REQ: state<=S_COPY_FETCH_WAIT;
                S_COPY_FETCH_WAIT: if(payload_valid) begin
                    packet_word<=capture_word_data;
                    if(capture_index[1:0]==3) state<=S_COPY_CLIP;
                    else begin capture_index<=capture_index+1; state<=S_COPY_FETCH_REQ; end
                end
                S_COPY_CLIP: begin
                    // Clip X completely, retain bounded X origins/count, then
                    // reuse the same signed interval for Y. No raw coordinate
                    // narrowing occurs until that axis has a nonempty interval.
                    case(copy_setup_step)
                    0,7: begin copy_high<=copy_raw_coordinate; copy_low<=0; end
                    1,2,8,9: if(copy_clip_better) copy_low<=copy_clip_candidate;
                    3,4,10,11: if(copy_clip_better) copy_high<=copy_clip_candidate;
                    5: copy_first_source_x<=copy_raw_coordinate+copy_low;
                    6: copy_first_destination_x<=copy_raw_coordinate+copy_low;
                    12: copy_first_source_y<=copy_raw_coordinate+copy_low;
                    13: copy_first_destination_y<=copy_raw_coordinate+copy_low;
                    default: raise_fault(9);
                    endcase
                    if(copy_setup_step==4 || copy_setup_step==11) state<=S_COPY_CLIP_CHECK;
                    else if(copy_setup_step==13) state<=S_COPY_INIT_ADDR;
                    else begin
                        copy_setup_step<=copy_setup_step+1;
                        capture_index<=copy_parameter_offset(copy_setup_step+4'd1); state<=S_COPY_FETCH_REQ;
                    end
                end
                S_COPY_CLIP_CHECK: begin
                    if(copy_high<=copy_low) complete_command();
                    else begin
                        if(copy_setup_step==4) copy_count_x<=copy_high-copy_low;
                        else copy_count_y<=copy_high-copy_low;
                        copy_setup_step<=copy_setup_step+1;
                        capture_index<=copy_parameter_offset(copy_setup_step+4'd1); state<=S_COPY_FETCH_REQ;
                    end
                end
                S_COPY_INIT_ADDR: begin
                    if(copy_address_steps==0) begin
                        copy_reverse<=copy_should_reverse;
                        copy_row_number<=copy_should_reverse ? copy_count_y-11'd1 : 0;
                        copy_destination_row<=color_base(copy_target_id)+{3'd0,copy_destination_start_pixel,1'b0};
                        copy_address_steps<=1;
                    end else if(copy_address_steps-11'd1 < copy_source_start_y) begin
                        copy_source_row<=copy_source_row+copy_pitch; copy_address_steps<=copy_address_steps+1;
                    end else begin
                        copy_source_row<=copy_source_row+{11'd0,copy_first_source_x,1'b0}; copy_column<=0; copy_active<=1;
                        color_valid[copy_target_id]<=0; state<=S_COPY_READ_CMD;
                    end
                end
                S_COPY_READ_CMD: if(mem_cmd_ready[5]) state<=S_COPY_READ_DATA;
                S_COPY_READ_DATA: if(mem_r_valid[5]) state<=S_COPY_READ_DONE;
                S_COPY_READ_DONE: if(mem_done[5]) begin
                    if(copy_column+1==copy_count_x) begin copy_column<=0; state<=S_COPY_RAM_REQ; end
                    else begin copy_column<=copy_column+1; state<=S_COPY_READ_CMD; end
                end
                S_COPY_RAM_REQ: state<=S_COPY_RAM_WAIT;
                S_COPY_RAM_WAIT: begin copy_pixel_hold<=copy_ram_rdata; state<=S_COPY_WRITE_CMD; end
                S_COPY_WRITE_CMD: if(mem_cmd_ready[5]) state<=S_COPY_WRITE_DATA;
                S_COPY_WRITE_DATA: if(mem_w_ready[5]) state<=S_COPY_WRITE_DONE;
                S_COPY_WRITE_DONE: if(mem_done[5]) begin
                    if(copy_column+1!=copy_count_x) begin copy_column<=copy_column+1; state<=S_COPY_RAM_REQ; end
                    else if((!copy_reverse && copy_row_number+1==copy_count_y) || (copy_reverse && copy_row_number==0)) begin
                        color_valid[copy_target_id]<=copy_previous_valid ||
                            (copy_first_destination_x==0 && copy_first_destination_y==0 && copy_count_x==640 && copy_count_y==480);
                        copy_active<=0; complete_command();
                    end else begin
                        copy_row_number<=copy_reverse ? copy_row_number-1 : copy_row_number+1;
                        copy_source_row<=copy_reverse ? copy_source_row-copy_pitch : copy_source_row+copy_pitch;
                        copy_destination_row<=copy_reverse ? copy_destination_row-1280 : copy_destination_row+1280;
                        copy_column<=0; state<=S_COPY_READ_CMD;
                    end
                end
                S_RECOVER: if(!host_recovering) begin if(initialized) state<=S_IDLE; else state<=S_BOOT_WAIT; end
                S_FAULT: begin end
                default: raise_fault(9);
                endcase
            end
        end
    end

`ifndef SYNTHESIS
    // Check the storage optimization against a full execution-time snapshot in
    // simulation. This checker is absent from the synthesized circuit.
    reg [511:0] checked_status_snapshot;
    always @(posedge clk) begin
        if(reset) checked_status_snapshot<=0;
        else begin
            if(state==S_EXECUTE && opcode==2) checked_status_snapshot<=status_snapshot;
            if(state==S_REPLY && reply_has_snapshot && !reply_caps &&
               reply_index>=28 && reply_index<92 && response_ready &&
               !abort_pending && !fault_pending && host_fault==0 && !host_recovering)
                if(response_data !== checked_status_snapshot[(reply_index-28)*8 +:8])
                    $fatal(1,"GET_STATUS reply lost its execution-time snapshot");
        end
    end
`endif

    nano_host_link host_link (
        .clk(clk),.reset(reset),.host_clk(host_clk),.core_abort(host_abort),
        .host_wr_valid(host_wr_valid),.host_wr_data(host_wr_data),.host_wr_ready(host_wr_ready),
        .host_rd_req(host_rd_req),.host_reg_addr(host_reg_addr),.host_rd_data(host_rd_data),.host_rd_valid(host_rd_valid),
        .status_snapshot(status_snapshot),.capabilities(capabilities),
        .cmd_valid(host_cmd_valid),.cmd_opcode(host_cmd_opcode),.cmd_sequence(host_cmd_sequence),.cmd_payload_length(host_cmd_length),
        .cmd_pop(host_cmd_pop),.payload_req(payload_req),.payload_offset(payload_offset),.payload_valid(payload_valid),.payload_data(payload_data),
        .response_valid(response_valid),.response_ready(response_ready),.response_data(response_data),.fault(host_fault),.recovering(host_recovering));
    nano_sdram_service #(.POWERUP_CYCLES(POWERUP_CYCLES),.REFRESH_INTERVAL(REFRESH_INTERVAL)) memory_service (
        .clk(clk),.reset(reset),.initialized(memory_initialized),
        .cmd_valid(mem_cmd_valid),.cmd_ready(mem_cmd_ready),.cmd_write(mem_cmd_write),.cmd_addr(mem_cmd_addr),.cmd_words(mem_cmd_words),
        .w_valid(mem_w_valid),.w_ready(mem_w_ready),.w_data(mem_w_data),.w_strb(mem_w_strb),
        .r_valid(mem_r_valid),.r_ready(mem_r_ready),.r_data(mem_r_data),.r_last(mem_r_last),.done(mem_done),.error(mem_error),
        .sd_clk(sd_clk),.sd_cke(sd_cke),.sd_cs_n(sd_cs_n),.sd_ras_n(sd_ras_n),.sd_cas_n(sd_cas_n),.sd_we_n(sd_we_n),
        .sd_ba(sd_ba),.sd_addr(sd_addr),.sd_dq_in(sd_dq_in),.sd_dq_out(sd_dq_out),.sd_dq_oe(sd_dq_oe),.sd_dqm(sd_dqm));
    nano_scanout scanout (
        .clk(clk),.reset(reset),.pixel_ce(pixel_tick),.memory_ready(initialized),.front_id(front_id),
        .pixel(video_rgb565),.hsync(video_hsync),.vsync(video_vsync),.de(video_de),.x(video_x),.y(video_y),
        .vblank_pulse(vblank_pulse),.underflows(scanout_underflows),
        .mem_cmd_valid(scan_cmd_valid),.mem_cmd_ready(mem_cmd_ready[0]),.mem_cmd_write(scan_cmd_write),
        .mem_cmd_addr(scan_cmd_addr),.mem_cmd_words(scan_cmd_words),.mem_w_valid(),.mem_w_ready(mem_w_ready[0]),.mem_w_data(),.mem_w_strb(),
        .mem_r_valid(mem_r_valid[0]),.mem_r_ready(scan_r_ready),.mem_r_data(mem_r_data[0 +:32]),.mem_r_last(mem_r_last[0]),
        .mem_done(mem_done[0]),.mem_error(mem_error[0]));
    nano_texture_cache texture_cache (
        .clk(clk),.reset(reset||renderer_reset),.flush(tex_flush),
        .cfg_valid(tex_cfg_valid),.cfg_ready(tex_cfg_ready),.cfg_base(tex_cfg_base),.cfg_width(tex_cfg_width),.cfg_height(tex_cfg_height),
        .cfg_pitch(tex_cfg_pitch),.cfg_repeat_u(tex_cfg_repeat_u),.cfg_repeat_v(tex_cfg_repeat_v),
        .req_valid(tex_req_valid),.req_ready(tex_req_ready),.req_x(tex_req_x),.req_y(tex_req_y),.req_tag(tex_req_tag),
        .rsp_valid(tex_rsp_valid),.rsp_ready(tex_rsp_ready),.rsp_texel(tex_rsp_texel),.rsp_tag(tex_rsp_tag),.fault(tex_fault),
        .mem_cmd_valid(tex_cmd_valid),.mem_cmd_ready(mem_cmd_ready[4]),.mem_cmd_write(tex_cmd_write),.mem_cmd_addr(tex_cmd_addr),.mem_cmd_words(tex_cmd_words),
        .mem_w_valid(tex_w_valid),.mem_w_ready(mem_w_ready[4]),.mem_w_data(tex_w_data),.mem_w_strb(tex_w_strb),
        .mem_r_valid(mem_r_valid[4]),.mem_r_ready(tex_r_ready),.mem_r_data(mem_r_data[128 +:32]),.mem_r_last(mem_r_last[4]),.mem_done(mem_done[4]),.mem_error(mem_error[4]));
    nano_tile_renderer tile_renderer (
        .clk(clk),.reset(reset||renderer_reset),.cmd_word_we(tile_cmd_word_we),.cmd_word_index(tile_cmd_word_index),.cmd_word_data(tile_cmd_word_data),
        .cmd_valid(tile_cmd_valid),.cmd_ready(tile_cmd_ready),.cmd_tile_x(tile_origin_x),.cmd_tile_y(tile_origin_y),
        .busy(tile_busy),.done(tile_done),.fault(tile_fault),
        .pix_we(tile_pix_we),.pix_addr(tile_pix_addr),.pix_color(tile_pix_color),.pix_depth(tile_pix_depth),
        .pix_re(tile_pix_re),.pix_raddr(tile_pix_raddr),.pix_rvalid(tile_pix_rvalid),.pix_rcolor(tile_pix_rcolor),.pix_rdepth(tile_pix_rdepth),
        .tex_flush(tex_flush),.tex_cfg_valid(tex_cfg_valid),.tex_cfg_ready(tex_cfg_ready),.tex_cfg_base(tex_cfg_base),
        .tex_cfg_width(tex_cfg_width),.tex_cfg_height(tex_cfg_height),.tex_cfg_pitch(tex_cfg_pitch),.tex_cfg_repeat_u(tex_cfg_repeat_u),.tex_cfg_repeat_v(tex_cfg_repeat_v),
        .tex_req_valid(tex_req_valid),.tex_req_ready(tex_req_ready),.tex_req_x(tex_req_x),.tex_req_y(tex_req_y),.tex_req_tag(tex_req_tag),
        .tex_rsp_valid(tex_rsp_valid),.tex_rsp_ready(tex_rsp_ready),.tex_rsp_texel(tex_rsp_texel),.tex_rsp_tag(tex_rsp_tag),.tex_fault(tex_fault));
    nano_sdp16 reply_ram (.wclk(clk),.wce(1'b1),.we(reply_ram_we),.waddr(reply_ram_waddr),.wdata(reply_ram_wdata),
        .rclk(clk),.rce(1'b1),.re(reply_ram_re),.raddr(reply_ram_raddr),.reset_output(reset),.rdata(reply_ram_rdata));
    nano_sdp16 copy_row_ram (.wclk(clk),.wce(1'b1),.we(copy_ram_we),.waddr(copy_ram_waddr),.wdata(copy_ram_wdata),
        .rclk(clk),.rce(1'b1),.re(copy_ram_re),.raddr(copy_ram_raddr),.reset_output(reset),.rdata(copy_ram_rdata));
    nano_sdp16 upload_ram (.wclk(clk),.wce(1'b1),.we(upload_ram_we),.waddr(upload_ram_waddr),.wdata(upload_ram_wdata),
        .rclk(clk),.rce(1'b1),.re(upload_ram_re),.raddr(upload_ram_raddr),.reset_output(reset),.rdata(upload_ram_rdata));
endmodule
