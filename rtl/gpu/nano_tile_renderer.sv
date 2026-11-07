// Functional tiled rasterizer. The command format is N20K-SYS-0.1 (80 words).
// All attachment state uses native Gowin-compatible synchronous RAM ports.
// The untextured path accepts one covered sample per edge after setup.
// Textured fragments use a 64-context native-RAM pipeline with ordered
// retirement. The RTL simulator supplies the authoritative cycle count.
module nano_tile_renderer (
    input wire clk, input wire reset,
    input wire cmd_word_we, input wire [6:0] cmd_word_index,
    input wire [31:0] cmd_word_data,
    input wire cmd_valid, output wire cmd_ready,
    input wire [9:0] cmd_tile_x, input wire [8:0] cmd_tile_y,
    output wire busy, output reg done, output wire fault,
    // External attachment port: pix_we[0] writes color, pix_we[1] depth.
    input wire [1:0] pix_we, input wire [9:0] pix_addr,
    input wire [15:0] pix_color, input wire [23:0] pix_depth,
    input wire pix_re, input wire [9:0] pix_raddr,
    output reg pix_rvalid, output wire [15:0] pix_rcolor,
    output wire [23:0] pix_rdepth,
    input wire tex_flush,
    output wire tex_cfg_valid, input wire tex_cfg_ready,
    output wire [22:0] tex_cfg_base, output wire [10:0] tex_cfg_width,
    output wire [10:0] tex_cfg_height, output wire [22:0] tex_cfg_pitch,
    output wire tex_cfg_repeat_u, output wire tex_cfg_repeat_v,
    output wire tex_req_valid, input wire tex_req_ready,
    output wire signed [31:0] tex_req_x, output wire signed [31:0] tex_req_y,
    output wire [5:0] tex_req_tag,
    input wire tex_rsp_valid, output wire tex_rsp_ready,
    input wire [15:0] tex_rsp_texel, input wire [5:0] tex_rsp_tag,
    input wire tex_fault
);
    localparam [4:0] IDLE=0, START=1, CONFIG=2, EDGE_LAUNCH=3,
        EDGE_WAIT=4, EDGE_CAPTURE=5, OFFSET_Y=6, OFFSET_X=7, RASTER=8,
        SEED_CAPTURE=9, CLIP_SCISSOR=10, CLIP_TILE=11, PREPARE=12,
        DRAIN=17, FAILED=18;
    reg [4:0] state;
    reg sticky_fault;
    // Individual sticky arithmetic flags terminate the wide-adder paths.
    // Their OR is visible immediately after the same edge as the old fault
    // register, suppressing pending writes before the next attachment edge.
    // The controller enters FAILED on the following edge; busy is already low.
    reg [6:0] plane_fault;
    reg [2:0] edge_fault;
    assign fault=sticky_fault||(|plane_fault)||(|edge_fault);
    // Header/edge words remain direct registers. The seven seed/dx/dy
    // triplets use 14 native 512x32 banks instead of 1344 coefficient FFs.
    // Per-word nonzero flags preserve reserved-field checks and permit command
    // words to be loaded in arbitrary order, including rewrites to zero.
    // Retain only semantically live payload bits. Malformed high/reserved
    // bits become a per-word sentinel, preserving exact validation without
    // spending one fabric FF (and its physical feed LUT) per discarded bit.
    wire [31:0] words [0:37];
    reg [41:0] coefficient_nonzero;
    wire signed [63:0] coefficient_data [0:6];
    reg [9:0] origin_x;
    reg [8:0] origin_y;
    // Header words are immutable while a draw is active. DRAIN waits for
    // every pending fragment before IDLE accepts another word, so these live
    // fields need no duplicate fabric FF bank or physical passthrough LUTs.
    wire [1:0] kind=words[0][1:0];
    wire [2:0] shade=words[1][2:0];
    wire [1:0] depth_func=words[1][9:8];
    wire color_write=words[0][16],depth_test=words[0][17];
    wire depth_write=words[0][18],key_enable=words[0][19];
    wire need_texture=(words[0][7:0]==3||(words[0][7:0]==0&&(|words[1][7:1])))&&
        (words[0][16]||words[0][19]);
    wire [15:0] key_color=words[3][15:0];
    wire [23:0] constant_color=words[2][23:0],constant_depth=words[16][23:0];
    // Raw fields keep their full-width checks below. Retain raster positions
    // only after viewport intersection; valid nonempty windows are bounded.
    reg signed [10:0] x, x0, x1, line_end_x;
    reg signed [9:0] y, y0, y1, line_end_y;
    // Every nonempty clipped tile span is at most 32 candidates per axis.
    // Store candidates remaining after the current sample, never raw bounds.
    // These small counters provide row-end/lookahead without a widened
    // x+1 comparison feeding each 64-bit plane adder in the same clock.
    reg [4:0] remaining_x,remaining_y,row_remaining_x;
    reg window_empty,setup_bad,sprite_nonempty,sprite_wide;
    reg [10:0] sprite_span_x,sprite_span_y;
    reg signed [10:0] offset_x;
    reg signed [9:0] offset_y;
    reg signed [11:0] source_x, source_y, source_start_x;
    reg signed [15:0] line_dx, line_dy, line_sx, line_sy, line_error;
    reg signed [31:0] edge_row [0:2], edge_current [0:2];
    reg signed [63:0] plane_row [0:6], plane_current [0:6];
    wire [6:0] plane_enable={
        {3{words[0][7:0]==0&&(|words[1][7:1])&&(words[0][16]||words[0][19])}},
        {3{words[0][16]&&(words[1][7:0]==1||words[1][7:0]==4)}},
        (words[0][17]||words[0][18])};
    reg [2:0] edge_term;
    reg [1:0] wait_count;
    reg signed [35:0] edge_a_product;

    // Explicit bit predicates avoid carry chains for constant viewport and
    // small-enum validation. Keep all high raw bits in these checks: a field
    // must never become valid merely because the retained datapath is narrow.
    function automatic beyond_x;
        input [31:0] a;
        begin beyond_x=(|a[31:10])||(a[9]&&(a[8]||(a[7]&&(|a[6:0])))); end
    endfunction
    function automatic beyond_y;
        input [31:0] a;
        begin beyond_y=(|a[31:9])||((&a[8:5])&&(|a[4:0])); end
    endfunction
    function automatic outside_x;
        input [31:0] a;
        begin outside_x=(|a[31:10])||(a[9]&&(a[8]||a[7])); end
    endfunction
    function automatic outside_y;
        input [31:0] a;
        begin outside_y=(|a[31:9])||(&a[8:5]); end
    endfunction
    // Clamping all interval endpoints into the viewport preserves every
    // nonempty intersection. Out-of-range lower/upper endpoints still produce
    // an empty interval. It avoids building chains of 32-bit comparators for
    // geometry that can only select one of 641/481 screen boundary positions.
    function automatic signed [10:0] clip_x;
        input signed [31:0] a;
        begin
            if(a[31]) clip_x=0;
            else if(beyond_x(a)) clip_x=11'sd640;
            else clip_x=$signed({1'b0,a[9:0]});
        end
    endfunction
    function automatic signed [9:0] clip_y;
        input signed [31:0] a;
        begin
            if(a[31]) clip_y=0;
            else if(beyond_y(a)) clip_y=10'sd480;
            else clip_y=$signed({1'b0,a[8:0]});
        end
    endfunction
    function automatic signed [10:0] maximum_x;
        input signed [10:0] a,b;
        begin maximum_x=(a>b)?a:b; end
    endfunction
    function automatic signed [10:0] minimum_x;
        input signed [10:0] a,b;
        begin minimum_x=(a<b)?a:b; end
    endfunction
    function automatic signed [9:0] maximum_y;
        input signed [9:0] a,b;
        begin maximum_y=(a>b)?a:b; end
    endfunction
    function automatic signed [9:0] minimum_y;
        input signed [9:0] a,b;
        begin minimum_y=(a<b)?a:b; end
    endfunction
    // Saturate before narrowing. The old generic quantizer built a 49-bit
    // rounded integer and comparison even though only D24 or RGB8 can escape.
    // Negative fixed-point values clamp to zero; nonnegative upper bits or a
    // rounded carry saturate. Guard/sticky/LSB implements exact ties-to-even.
    function automatic [23:0] quantize_depth;
        input signed [63:0] value;
        reg [24:0] rounded;
        begin
            rounded={1'b0,value[39:16]}+
                (value[15]&&((|value[14:0])||value[16]));
            if(value[63]) quantize_depth=0;
            else if((|value[62:40])||rounded[24]) quantize_depth=24'hffffff;
            else quantize_depth=rounded[23:0];
        end
    endfunction
    function automatic [15:0] pack_color;
        input [23:0] rgb;
        begin pack_color={rgb[7:3],rgb[15:10],rgb[23:19]}; end
    endfunction
    function automatic [7:0] quantize_color;
        input signed [63:0] value;
        reg [8:0] rounded;
        begin
            rounded={1'b0,value[23:16]}+
                (value[15]&&((|value[14:0])||value[16]));
            if(value[63]) quantize_color=0;
            else if((|value[62:24])||rounded[8]) quantize_color=8'hff;
            else quantize_color=rounded[7:0];
        end
    endfunction
    function automatic depth_pass;
        input [23:0] incoming,stored;
        begin
            case(depth_func)
            0:depth_pass=incoming<stored;
            1:depth_pass=incoming<=stored;
            default:depth_pass=1'b1;
            endcase
        end
    endfunction
    // Each bank has its own write enable; no write order is assumed and a
    // rewrite recomputes its sentinel, including clearing an old error/NZ bit.
    generate for(genvar w=0;w<38;w=w+1) begin:header_words
        wire write_word=!reset&&cmd_word_we&&(state==IDLE)&&(cmd_word_index==w);
        if(w==0) begin:h0
            reg [6:0] value;
            always @(posedge clk) if(write_word) value<={
                (cmd_word_data[15:8]!=1)||(|cmd_word_data[7:2])||(|cmd_word_data[31:20]),
                cmd_word_data[19:16],cmd_word_data[1:0]};
            assign words[w]={11'd0,value[6],value[5:2],8'h01,6'd0,value[1:0]};
        end else if(w==1) begin:h1
            reg [8:0] value;
            always @(posedge clk) if(write_word) value<={
                (|cmd_word_data[31:19])||(|cmd_word_data[15:10])||(&cmd_word_data[9:8])||
                (|cmd_word_data[7:3])||(cmd_word_data[2]&&(|cmd_word_data[1:0])),
                cmd_word_data[18:16],cmd_word_data[9:8],cmd_word_data[2:0]};
            assign words[w]={12'd0,value[8],value[7:5],6'd0,value[4:3],5'd0,value[2:0]};
        end else if(w==2||w==16) begin:color_depth
            reg [24:0] value;
            always @(posedge clk) if(write_word) value<={|cmd_word_data[31:24],cmd_word_data[23:0]};
            assign words[w]={7'd0,value};
        end else if(w==3) begin:key
            reg [16:0] value;
            always @(posedge clk) if(write_word) value<={|cmd_word_data[31:16],cmd_word_data[15:0]};
            assign words[w]={15'd0,value};
        end else if(w==4||w==6) begin:scissor_x
            reg [10:0] value;
            always @(posedge clk) if(write_word) value<=clip_x($signed(cmd_word_data));
            assign words[w]={21'd0,value};
        end else if(w==5||w==7) begin:scissor_y
            reg [9:0] value;
            always @(posedge clk) if(write_word) value<=clip_y($signed(cmd_word_data));
            assign words[w]={22'd0,value};
        end else if(w==12||w==15) begin:texture_address
            reg [23:0] value;
            always @(posedge clk) if(write_word) value<={|cmd_word_data[31:23],cmd_word_data[22:0]};
            assign words[w]={8'd0,value};
        end else if(w==13||w==14) begin:texture_size
            reg [11:0] value;
            always @(posedge clk) if(write_word) value<={|cmd_word_data[31:11],cmd_word_data[10:0]};
            assign words[w]={20'd0,value};
        end else if(w==17||w==18) begin:source_a
            reg [12:0] value;
            always @(posedge clk) if(write_word) value<={cmd_word_data[31],|cmd_word_data[30:11],cmd_word_data[10:0]};
            assign words[w]={value[12],19'd0,value[11:0]};
        end else if(w==20) begin:line_b_x
            reg [10:0] value;
            always @(posedge clk) if(write_word) value<={|cmd_word_data[31:10],cmd_word_data[9:0]};
            assign words[w]={21'd0,value};
        end else if(w==21) begin:line_b_y
            reg [9:0] value;
            always @(posedge clk) if(write_word) value<={|cmd_word_data[31:9],cmd_word_data[8:0]};
            assign words[w]={22'd0,value};
        end else if(w==19||(w>=22&&w<=25)) begin:unused_nz
            reg value;
            always @(posedge clk) if(write_word) value<=|cmd_word_data;
            assign words[w]={31'd0,value};
        end else if(w==26||w==27||w==30||w==31||w==34||w==35) begin:edge_coefficient
            reg [18:0] value;
            always @(posedge clk) if(write_word) value<={
                (cmd_word_data[31:18]!={14{cmd_word_data[17]}}),cmd_word_data[17:0]};
            assign words[w]={{13{value[17]}},value[17]^value[18],value[17:0]};
        end else if(w==29||w==33||w==37) begin:inclusive
            reg [1:0] value;
            always @(posedge clk) if(write_word) value<={|cmd_word_data[31:1],cmd_word_data[0]};
            assign words[w]={30'd0,value};
        end else begin:full_word
            reg [31:0] value;
            always @(posedge clk) if(write_word) value<=cmd_word_data;
            assign words[w]=value;
        end
    end endgenerate

    // Line endpoints are separately validated as unsigned viewport pixels.
    // Truncating them here affects no accepted record and keeps line setup
    // within the same bounded coordinate domain as the raster walker.
    wire signed [10:0] a_x=$signed({1'b0,words[17][9:0]});
    wire signed [9:0] a_y=$signed({1'b0,words[18][8:0]});
    wire signed [10:0] b_x=$signed({1'b0,words[20][9:0]});
    wire signed [9:0] b_y=$signed({1'b0,words[21][8:0]});
    wire swap_line=(b_x<a_x)||((b_x==a_x)&&(b_y<a_y));
    wire signed [10:0] first_x=swap_line?b_x:a_x, last_x=swap_line?a_x:b_x;
    wire signed [9:0] first_y=swap_line?b_y:a_y, last_y=swap_line?a_y:b_y;
    wire line_kind=words[0][7:0]==2;
    wire signed [31:0] bounds_x0=line_kind?minimum_x(a_x,b_x):$signed(words[8]);
    wire signed [31:0] bounds_y0=line_kind?minimum_y(a_y,b_y):$signed(words[9]);
    wire signed [31:0] bounds_x1=line_kind?maximum_x(a_x,b_x)+11'sd1:$signed(words[10]);
    wire signed [31:0] bounds_y1=line_kind?maximum_y(a_y,b_y)+10'sd1:$signed(words[11]);
`ifndef SYNTHESIS
    // Independent original clipping equations check the registered setup
    // stages below; this full nested cone is absent from synthesized logic.
    wire signed [10:0] window_x0=maximum_x(maximum_x(clip_x(bounds_x0),clip_x($signed(words[4]))),
        $signed({1'b0,origin_x}));
    wire signed [9:0] window_y0=maximum_y(maximum_y(clip_y(bounds_y0),clip_y($signed(words[5]))),
        $signed({1'b0,origin_y}));
    wire signed [10:0] window_x1=minimum_x(minimum_x(clip_x(bounds_x1),clip_x($signed(words[6]))),
        minimum_x($signed({1'b0,origin_x})+11'sd32,11'sd640));
    wire signed [9:0] window_y1=minimum_y(minimum_y(clip_y(bounds_y1),clip_y($signed(words[7]))),
        minimum_y($signed({1'b0,origin_y})+10'sd32,10'sd480));
`endif
    wire grid_inside=x>=x0&&x<x1&&y>=y0&&y<y1;
    wire triangle_inside=(edge_current[0]>0||(edge_current[0]==0&&words[29][0]))&&
        (edge_current[1]>0||(edge_current[1]==0&&words[33][0]))&&
        (edge_current[2]>0||(edge_current[2]==0&&words[37][0]));
    wire inside_sample=(kind==0)?triangle_inside:grid_inside;
    wire [9:0] sample_addr={y[4:0],x[4:0]};
    wire [23:0] sample_depth=(kind==0)?quantize_depth(plane_current[0]):constant_depth;
    wire use_rgb_planes=(kind==0)&&color_write&&(shade==1||shade==4);
    wire [7:0] sample_r=quantize_color(plane_current[1]);
    wire [7:0] sample_g=quantize_color(plane_current[2]);
    wire [7:0] sample_b=quantize_color(plane_current[3]);
    wire [23:0] sample_color=use_rgb_planes?{sample_b,sample_g,sample_r}:constant_color;

    reg pending;
    reg [9:0] pending_addr;
    reg [23:0] pending_depth;
    reg [15:0] pending_color;
    wire fragment_ready,fragment_busy,fragment_fault,fragment_retired;
    wire fragment_color_write,fragment_depth_write;
    wire [9:0] fragment_address;
    wire [23:0] fragment_depth;
    wire [15:0] fragment_color;
    wire [15:0] ram_color,ram_depth_low,ram_depth_high;
    wire [23:0] stored_depth={ram_depth_high[7:0],ram_depth_low};
    wire fast_commit=pending&&(!depth_test||depth_pass(pending_depth,stored_depth))&&!fault&&!reset;
    wire ext_write=(state==IDLE)&&(|pix_we)&&!fault&&!reset;
    wire color_we=(ext_write&&pix_we[0])||(fast_commit&&color_write)||(fragment_color_write&&!fault&&!reset);
    wire depth_we=(ext_write&&pix_we[1])||(fast_commit&&depth_write)||(fragment_depth_write&&!fault&&!reset);
    wire [9:0] write_addr=ext_write?pix_addr:(fragment_retired?fragment_address:pending_addr);
    wire [15:0] write_color=ext_write?pix_color:(fragment_retired?fragment_color:pending_color);
    wire [23:0] write_depth=ext_write?pix_depth:(fragment_retired?fragment_depth:pending_depth);
    wire sample_re=(state==RASTER)&&inside_sample&&depth_test&&(!need_texture||fragment_ready)&&!fault&&!reset;
    wire external_re=(state==IDLE)&&pix_re&&!fault&&!reset;
    wire [9:0] read_addr=external_re?pix_raddr:sample_addr;
    nano_sdp16 color_ram(.wclk(clk),.wce(1'b1),.we(color_we),.waddr(write_addr),.wdata(write_color),
        .rclk(clk),.rce(1'b1),.re(external_re),.raddr(read_addr),.reset_output(reset),.rdata(ram_color));
    nano_sdp16 depth_low_ram(.wclk(clk),.wce(1'b1),.we(depth_we),.waddr(write_addr),.wdata(write_depth[15:0]),
        .rclk(clk),.rce(1'b1),.re(external_re||sample_re),.raddr(read_addr),.reset_output(reset),.rdata(ram_depth_low));
    nano_sdp16 depth_high_ram(.wclk(clk),.wce(1'b1),.we(depth_we),.waddr(write_addr),.wdata({8'b0,write_depth[23:16]}),
        .rclk(clk),.rce(1'b1),.re(external_re||sample_re),.raddr(read_addr),.reset_output(reset),.rdata(ram_depth_high));
    assign pix_rcolor=ram_color;
    assign pix_rdepth=stored_depth;
    assign busy=(state!=IDLE)&&(state!=FAILED)&&!fault;
    assign cmd_ready=(state==IDLE)&&!fault;

    wire [1:0] edge_index=edge_term[2:1];
    // This six-way selection is the actual setup datapath, rather than a
    // variable read port across all eighty command registers.
    reg signed [31:0] coefficient,current_edge_c;
    always @* begin
        case(edge_term)
        0:coefficient=words[26];1:coefficient=words[27];
        2:coefficient=words[30];3:coefficient=words[31];
        4:coefficient=words[34];default:coefficient=words[35];
        endcase
        case(edge_index)
        0:current_edge_c=words[28];1:current_edge_c=words[32];
        default:current_edge_c=words[36];
        endcase
    end
    wire signed [31:0] center=(edge_term[0])?(y0<<<4)+32'sd8:(x0<<<4)+32'sd8;
    wire signed [35:0] edge_product;
    wire signed [37:0] edge_sum={{2{edge_a_product[35]}},edge_a_product}+
        {{2{edge_product[35]}},edge_product}+{{6{current_edge_c[31]}},current_edge_c};
    nano_mult18 edge_multiplier(.clk(clk),.ce(1'b1),.reset(reset),
        .a((state==EDGE_LAUNCH)?coefficient[17:0]:18'sd0),
        .b((state==EDGE_LAUNCH)?center[17:0]:18'sd0),.product(edge_product));

    assign tex_cfg_valid=(state==CONFIG)&&!fault&&!reset;
    assign tex_cfg_base=words[12][22:0];
    assign tex_cfg_width=words[13][10:0];
    assign tex_cfg_height=words[14][10:0];
    assign tex_cfg_pitch=words[15][22:0];
    assign tex_cfg_repeat_u=words[1][16];
    assign tex_cfg_repeat_v=words[1][17];
    // Valid reconstructed texels are signed16; sprite sources are signed12.
    // The standalone fragment interface retains its full signed32 default.
    nano_fragment_pipeline #(.COORD_BITS(16)) fragments(.clk(clk),.reset(reset),
        .color_write(color_write),.depth_test(depth_test),.depth_write(depth_write),.depth_func(depth_func),
        .key_enable(key_enable),.key_color(key_color),
        .in_valid(state==RASTER&&inside_sample&&need_texture&&!fault&&!reset),.in_ready(fragment_ready),
        .in_address(sample_addr),.in_depth(sample_depth),
        .in_factor((kind==3||shade==2)?24'hffffff:sample_color),
        .in_u(plane_current[5]),.in_v(plane_current[6]),.in_q(plane_current[4]),
        .in_sprite(kind==3),.in_source_x({{20{source_x[11]}},source_x}),
        .in_source_y({{20{source_y[11]}},source_y}),.stored_depth(stored_depth),
        .busy(fragment_busy),.fault(fragment_fault),.retired(fragment_retired),
        .commit_color_write(fragment_color_write),.commit_depth_write(fragment_depth_write),
        .commit_address(fragment_address),.commit_depth(fragment_depth),.commit_color(fragment_color),
        .tex_req_valid(tex_req_valid),.tex_req_ready(tex_req_ready),.tex_req_x(tex_req_x),.tex_req_y(tex_req_y),
        .tex_req_tag(tex_req_tag),.tex_rsp_valid(tex_rsp_valid),.tex_rsp_ready(tex_rsp_ready),
        .tex_rsp_texel(tex_rsp_texel),.tex_rsp_tag(tex_rsp_tag));

    integer i;
    // Distance does not depend on endpoint orientation. Keep it parallel
    // with sorting, rather than sorting before a second absolute comparison.
    wire signed [10:0] line_abs_x=(b_x>=a_x)?b_x-a_x:a_x-b_x;
    wire signed [9:0] line_abs_y=(b_y>=a_y)?b_y-a_y:a_y-b_y;
    wire signed [9:0] line_negative_y=-line_abs_y;
    wire record_needs_texture=words[0][7:0]==3||
        (words[0][7:0]==0&&(|words[1][7:1])&&(words[0][16]||words[0][19]));
    wire signed [32:0] sprite_width=$signed({words[10][31],words[10]})-$signed({words[8][31],words[8]});
    wire signed [32:0] sprite_height=$signed({words[11][31],words[11]})-$signed({words[9][31],words[9]});
`ifndef SYNTHESIS
    wire signed [33:0] sprite_source_end_x=$signed({2'b0,words[17]})+$signed({sprite_width[32],sprite_width});
    wire signed [33:0] sprite_source_end_y=$signed({2'b0,words[18]})+$signed({sprite_height[32],sprite_height});
    reg record_bad;
`endif
    reg record_bad_static;
    // After full-width span classification, only an 11-bit positive span
    // can fit an accepted descriptor. Preserve source high-bit sentinels in
    // the 12-bit add and reject its carry before comparing the bounded result.
    wire [12:0] sprite_limit_x={1'b0,words[17][11:0]}+{2'b0,sprite_span_x};
    wire [12:0] sprite_limit_y={1'b0,words[18][11:0]}+{2'b0,sprite_span_y};
    wire sprite_extent_bad=sprite_nonempty&&(sprite_wide||
        (|sprite_limit_x[12:11])||sprite_limit_x[10:0]>words[13][10:0]||
        (|sprite_limit_y[12:11])||sprite_limit_y[10:0]>words[14][10:0]);
    integer check_word;
    always @* begin
        record_bad_static=words[0][15:8]!=1||(|words[0][7:2])||words[0][31:20]!=0||
            ((|words[1][7:3])||(words[1][2]&&(|words[1][1:0])))||((|words[1][15:10])||(&words[1][9:8]))||words[1][31:19]!=0||
            words[2][31:24]!=0||words[3][31:16]!=0||words[16][31:24]!=0||
            words[19]!=0||(!words[0][19]&&words[3]!=0)||
            (record_needs_texture&&!words[1][18]);
        if(words[1][18])begin
            // Reject wide packed fields before truncating the cache ports.
            record_bad_static=record_bad_static||words[12][31:23]!=0||words[15][31:23]!=0||
                words[13][31:11]!=0||words[14][31:11]!=0;
        end else record_bad_static=record_bad_static||words[12]!=0||words[13]!=0||words[14]!=0||words[15]!=0;
        case(words[0][7:0])
        0:begin
            record_bad_static=record_bad_static||words[16]!=0||words[17]!=0||words[18]!=0||
                (|words[29][31:1])||(|words[33][31:1])||(|words[37][31:1])||
                words[26][31:17]!={15{words[26][17]}}||words[27][31:17]!={15{words[27][17]}}||
                words[30][31:17]!={15{words[30][17]}}||words[31][31:17]!={15{words[31][17]}}||
                words[34][31:17]!={15{words[34][17]}}||words[35][31:17]!={15{words[35][17]}}||
                $signed(words[8])<0||$signed(words[9])<0||beyond_x(words[10])||beyond_y(words[11]);
        end
        1:begin
            record_bad_static=record_bad_static||words[17]!=0||words[18]!=0||words[1][7:0]!=0||words[0][19];
            for(check_word=20;check_word<38;check_word=check_word+1)record_bad_static=record_bad_static||words[check_word]!=0;
            record_bad_static=record_bad_static||(|coefficient_nonzero);
        end
        2:begin
            record_bad_static=record_bad_static||words[8]!=0||words[9]!=0||words[10]!=0||words[11]!=0||
                words[1][7:0]!=0||words[0][19]||outside_x(words[17])||outside_x(words[20])||outside_y(words[18])||outside_y(words[21]);
            for(check_word=22;check_word<38;check_word=check_word+1)record_bad_static=record_bad_static||words[check_word]!=0;
            record_bad_static=record_bad_static||(|coefficient_nonzero);
        end
        3:begin
            record_bad_static=record_bad_static||(words[1][7:0]!=0&&words[1][7:0]!=2)||words[17][31]||words[18][31];
            for(check_word=20;check_word<38;check_word=check_word+1)record_bad_static=record_bad_static||words[check_word]!=0;
            record_bad_static=record_bad_static||(|coefficient_nonzero);
        end
        default:begin end
        endcase
`ifndef SYNTHESIS
        record_bad=record_bad_static;
        if(words[0][7:0]==3&&sprite_width>0&&sprite_height>0)record_bad=record_bad||
            (|sprite_source_end_x[33:11])||sprite_source_end_x[10:0]>words[13][10:0]||
            (|sprite_source_end_y[33:11])||sprite_source_end_y[10:0]>words[14][10:0];
`endif
    end
    // All attachment equations advance in lockstep. OFFSET_X, OFFSET_Y,
    // horizontal raster steps and row transitions are mutually exclusive;
    // route their operands through one adder per plane instead of replicating
    // wide adders for each controller branch. Overflow comes from that same
    // retained sum, rather than a second hidden function-local addition.
    wire raster_step_x=|remaining_x;
    wire raster_step_y=|remaining_y;
    wire step_from_row=(state!=RASTER)||!raster_step_x;
    wire step_in_y=(state==OFFSET_Y)||((state==RASTER)&&!raster_step_x);
    wire signed [63:0] plane_next [0:6];
    wire [6:0] plane_overflow;
    wire signed [31:0] edge_next [0:2];
    wire [2:0] edge_step_overflow;
    generate for(genvar p=0;p<7;p=p+1) begin:plane_steppers
        wire signed [63:0] a=(state==SEED_CAPTURE)?64'sd0:
            (step_from_row?plane_row[p]:plane_current[p]);
        wire signed [63:0] b=coefficient_data[p];
        assign plane_next[p]=a+b;
        assign plane_overflow[p]=(a[63]==b[63])&&(plane_next[p][63]!=a[63]);
    end endgenerate
    generate for(genvar e=0;e<3;e=e+1) begin:edge_steppers
        wire signed [31:0] a=step_from_row?edge_row[e]:edge_current[e];
        wire signed [31:0] b=(step_in_y?$signed(words[27+4*e]):$signed(words[26+4*e]))<<<4;
        assign edge_next[e]=a+b;
        assign edge_step_overflow[e]=(a[31]==b[31])&&(edge_next[e][31]!=a[31]);
    end endgenerate

    // A native RAM read completes after its active edge, so fetch the
    // coefficient required by the *next* accepted candidate. An unaccepted
    // textured candidate keeps the current selection. Row ends prefetch dx;
    // penultimate samples prefetch dy. A one-pixel-wide span always uses dy.
    wire advance_raster=(state==RASTER)&&(!inside_sample||!need_texture||fragment_ready);
    wire first_step_y=!(|row_remaining_x);
    reg [1:0] coefficient_read_address;
    always @* begin
        coefficient_read_address=step_in_y?2:1;
        case(state)
        START,CLIP_SCISSOR,CLIP_TILE,PREPARE: coefficient_read_address=0;
        SEED_CAPTURE,CONFIG,EDGE_LAUNCH,EDGE_WAIT,EDGE_CAPTURE:
            coefficient_read_address=(offset_y!=0)?2:((offset_x!=0)?1:(first_step_y?2:1));
        OFFSET_Y: if(offset_y==1)
            coefficient_read_address=(offset_x!=0)?1:(first_step_y?2:1);
        OFFSET_X: if(offset_x==1) coefficient_read_address=first_step_y?2:1;
        RASTER: if(advance_raster)
            coefficient_read_address=(raster_step_x?(remaining_x==1):first_step_y)?2:1;
        default: begin end
        endcase
    end
    generate for(genvar c=0;c<7;c=c+1) begin:coefficient_banks
        localparam [6:0] SEED_LO=38+6*c, DX_LO=40+6*c, DY_LO=42+6*c;
        localparam [6:0] SEED_HI=39+6*c, DX_HI=41+6*c, DY_HI=43+6*c;
        wire write_ok=cmd_word_we&&(state==IDLE)&&!fault&&!reset;
        wire write_lo=write_ok&&(cmd_word_index==SEED_LO||cmd_word_index==DX_LO||cmd_word_index==DY_LO);
        wire write_hi=write_ok&&(cmd_word_index==SEED_HI||cmd_word_index==DX_HI||cmd_word_index==DY_HI);
        wire [1:0] wa=(cmd_word_index==DY_LO||cmd_word_index==DY_HI)?2:
            ((cmd_word_index==DX_LO||cmd_word_index==DX_HI)?1:0);
        wire [31:0] data_lo,data_hi;
        wire read_coefficient=(state!=IDLE)&&(state!=FAILED);
        nano_sdp32 low_bank(.wclk(clk),.wce(1'b1),.we(write_lo),.waddr({7'd0,wa}),.wdata(cmd_word_data),
            .rclk(clk),.rce(1'b1),.re(read_coefficient),.raddr({7'd0,coefficient_read_address}),
            .reset_output(reset),.rdata(data_lo));
        nano_sdp32 high_bank(.wclk(clk),.wce(1'b1),.we(write_hi),.waddr({7'd0,wa}),.wdata(cmd_word_data),
            .rclk(clk),.rce(1'b1),.re(read_coefficient),.raddr({7'd0,coefficient_read_address}),
            .reset_output(reset),.rdata(data_hi));
        assign coefficient_data[c]=$signed({data_hi,data_lo});
    end endgenerate

    // Capture overflow only when the corresponding addition is consumed.
    // In particular, the unused next sum at a final pixel cannot fault a draw.
    // Once any flag sets, fault blocks capture and all attachment writes until
    // reset; no carry chain continues through the collective OR/state mux.
    wire offset_step=(state==OFFSET_X)||(state==OFFSET_Y);
    wire grid_step=advance_raster&&(kind!=2)&&(raster_step_x||raster_step_y);
    always @(posedge clk) begin
        if(reset) begin plane_fault<=0;edge_fault<=0;end
        else if(!fault) begin
            if(offset_step||grid_step) plane_fault<=plane_overflow&plane_enable;
            if(grid_step) edge_fault<=edge_step_overflow;
        end
    end
`ifndef SYNTHESIS
    // Compare the staged results with the original raw-record equations.
    // Both references are simulation-only, so no nested clipping or wide
    // sprite-end add reaches the physical controller's state inputs.
    always @(posedge clk) if(!reset&&!fault)begin
        if(state==CLIP_TILE&&setup_bad!==
           (record_bad||origin_x>=640||origin_y>=480||origin_x[4:0]!=0||origin_y[4:0]!=0))
            $fatal(1,"tile staged raw-record validation invariant failed");
        if(state==PREPARE&&(x0!==window_x0||y0!==window_y0||x1!==window_x1||y1!==window_y1))
            $fatal(1,"tile staged clipping invariant failed");
    end
    // Independent coordinate invariants retain the original full-width
    // comparison contract while the synthesized control uses five-bit counts.
    always @(posedge clk) if(!reset&&!fault&&state==RASTER&&kind!=2) begin
        if(raster_step_x!==(x+32'sd1<x1)||raster_step_y!==(y+32'sd1<y1)||
           first_step_y!==(x0+32'sd1>=x1)||
           (raster_step_x&&((remaining_x==1)!==(x+32'sd2>=x1))))
            $fatal(1,"tile remaining-count/coordinate invariant failed");
    end
`endif

    task automatic advance_candidate;
        reg signed [16:0] twice_error;
        reg signed [16:0] next_error;
        begin
            if(kind==2) begin
                if(x==line_end_x&&y==line_end_y) state<=DRAIN;
                else begin
                    twice_error=$signed({line_error[15],line_error})<<<1;
                    next_error=$signed({line_error[15],line_error});
                    if(twice_error>=$signed({line_dy[15],line_dy})) begin
                        next_error=next_error+$signed({line_dy[15],line_dy});
                        x<=x+$signed({{16{line_sx[15]}},line_sx});
                    end
                    if(twice_error<=$signed({line_dx[15],line_dx})) begin
                        next_error=next_error+$signed({line_dx[15],line_dx});
                        y<=y+$signed({{16{line_sy[15]}},line_sy});
                    end
                    line_error<=next_error[15:0]; state<=RASTER;
                end
            end else if(raster_step_x) begin
                x<=x+11'sd1; source_x<=source_x+12'sd1;
                remaining_x<=remaining_x-5'd1;
                for(integer j=0;j<3;j=j+1)begin
                    edge_current[j]<=edge_next[j];
                end
                for(integer j=0;j<7;j=j+1) if(plane_enable[j])begin
                    plane_current[j]<=plane_next[j];
                end
                state<=RASTER;
            end else if(raster_step_y) begin
                x<=x0; y<=y+10'sd1; source_x<=source_start_x; source_y<=source_y+12'sd1;
                remaining_x<=row_remaining_x;remaining_y<=remaining_y-5'd1;
                for(integer j=0;j<3;j=j+1) begin
                    edge_row[j]<=edge_next[j];
                    edge_current[j]<=edge_next[j];
                end
                for(integer j=0;j<7;j=j+1) if(plane_enable[j]) begin
                    plane_row[j]<=plane_next[j];
                    plane_current[j]<=plane_next[j];
                end
                state<=RASTER;
            end else state<=DRAIN;
        end
    endtask

    always @(posedge clk) begin
        if(reset) begin
            state<=IDLE; done<=0; sticky_fault<=0; pending<=0; pix_rvalid<=0;
            origin_x<=0; origin_y<=0;
            x<=0;y<=0;x0<=0;y0<=0;x1<=0;y1<=0;offset_x<=0;offset_y<=0;
            remaining_x<=0;remaining_y<=0;row_remaining_x<=0;
            window_empty<=1;setup_bad<=0;sprite_nonempty<=0;sprite_wide<=0;
            sprite_span_x<=0;sprite_span_y<=0;
            source_x<=0;source_y<=0;source_start_x<=0;
            line_end_x<=0;line_end_y<=0;line_dx<=0;line_dy<=0;line_sx<=0;line_sy<=0;line_error<=0;
            edge_term<=0;wait_count<=0;edge_a_product<=0;
            pending_addr<=0;pending_depth<=0;pending_color<=0;
            for(i=0;i<3;i=i+1) begin edge_row[i]<=0;edge_current[i]<=0;end
            for(i=0;i<7;i=i+1) begin plane_row[i]<=0;plane_current[i]<=0;end
        end else begin
            done<=0; pending<=0; pix_rvalid<=external_re;
            if(cmd_word_we) begin
                if(state!=IDLE||cmd_word_index>=80) begin sticky_fault<=1;state<=FAILED;end
                else if(cmd_word_index>=38) coefficient_nonzero[cmd_word_index-38]<=|cmd_word_data;
            end
            if(fault) begin sticky_fault<=1;state<=FAILED;end
            else if((tex_flush&&busy)||tex_fault||fragment_fault) begin sticky_fault<=1;state<=FAILED;end
            else case(state)
            IDLE: if(cmd_valid&&!fault) begin
                origin_x<=cmd_tile_x;origin_y<=cmd_tile_y;state<=START;
            end
            START: begin
                // Classify the complete raw record before any draw/config
                // side effect. Internal interval registers may be computed
                // speculatively; CLIP_TILE checks the final validation flag.
                setup_bad<=record_bad_static||origin_x>=640||origin_y>=480||
                    origin_x[4:0]!=0||origin_y[4:0]!=0;
                sprite_nonempty<=sprite_width>0&&sprite_height>0;
                sprite_wide<=(|sprite_width[32:11])||(|sprite_height[32:11]);
                sprite_span_x<=sprite_width[10:0];sprite_span_y<=sprite_height[10:0];
                x0<=line_kind?minimum_x(a_x,b_x):clip_x($signed(words[8]));
                y0<=line_kind?minimum_y(a_y,b_y):clip_y($signed(words[9]));
                x1<=line_kind?(maximum_x(a_x,b_x)+11'sd1):clip_x($signed(words[10]));
                y1<=line_kind?(maximum_y(a_y,b_y)+10'sd1):clip_y($signed(words[11]));
                if(line_kind)begin
                    x<=first_x;y<=first_y;
                    line_end_x<=last_x;line_end_y<=last_y;
                    line_dx<=line_abs_x;line_dy<=line_negative_y;
                    line_sx<=first_x<last_x?16'sd1:-16'sd1;
                    line_sy<=first_y<last_y?16'sd1:-16'sd1;
                end
                state<=CLIP_SCISSOR;
            end
            CLIP_SCISSOR: begin
                // Scissor words were individually viewport-clamped on load.
                // One bounded min/max layer is allowed between registers.
                x0<=maximum_x(x0,$signed(words[4][10:0]));
                y0<=maximum_y(y0,$signed(words[5][9:0]));
                x1<=minimum_x(x1,$signed(words[6][10:0]));
                y1<=minimum_y(y1,$signed(words[7][9:0]));
                setup_bad<=setup_bad||(kind==3&&sprite_extent_bad);
                state<=CLIP_TILE;
            end
            CLIP_TILE: begin
                if(setup_bad)begin sticky_fault<=1;state<=FAILED;end
                else begin
                    x0<=maximum_x(x0,$signed({1'b0,origin_x}));
                    y0<=maximum_y(y0,$signed({1'b0,origin_y}));
                    // Validated aligned origins are at most 608/448, so the
                    // tile end is already within the 640x480 viewport.
                    x1<=minimum_x(x1,$signed({1'b0,origin_x})+11'sd32);
                    y1<=minimum_y(y1,$signed({1'b0,origin_y})+10'sd32);
                    state<=PREPARE;
                end
            end
            PREPARE: begin
                window_empty<=(x0>=x1)||(y0>=y1);
                remaining_x<=x1-x0-11'sd1;
                row_remaining_x<=x1-x0-11'sd1;
                remaining_y<=y1-y0-10'sd1;
                if(!line_kind)begin
                    x<=x0;y<=y0;
                    source_x<=$signed({1'b0,words[17][10:0]})+x0-$signed(words[8][11:0]);
                    source_start_x<=$signed({1'b0,words[17][10:0]})+x0-$signed(words[8][11:0]);
                    source_y<=$signed({1'b0,words[18][10:0]})+y0-$signed(words[9][11:0]);
                    offset_x<=x0-$signed(words[8][10:0]);
                    offset_y<=y0-$signed(words[9][9:0]);
                end
                line_error<=line_dx+line_dy;
                edge_term<=0;
                if(((x0>=x1)||(y0>=y1))&&!words[1][18])state<=DRAIN;
                else state<=SEED_CAPTURE;
            end
            SEED_CAPTURE: begin
                // PREPARE held the native seed read; its registered result
                // is available now. Seed through 0+seed so every later
                // row/current write uses the same adder result, rather than
                // retaining wide data muxes for initialization and row copies.
                // This also separates a last command write
                // coincident with cmd_valid from all coefficient RAM reads.
                for(i=0;i<7;i=i+1) begin
                    plane_row[i]<=plane_next[i];plane_current[i]<=plane_next[i];
                end
                // The cache validates every present descriptor, even for
                // empty tiles or state whose color path does not sample it.
                if(words[1][18]) state<=CONFIG;
                else if(words[0][7:0]==0) state<=EDGE_LAUNCH;
                else state<=RASTER;
            end
            CONFIG: if(tex_cfg_ready) begin
                if(window_empty)state<=DRAIN;
                else state<=kind==0?EDGE_LAUNCH:RASTER;
            end
            EDGE_LAUNCH: begin wait_count<=2;state<=EDGE_WAIT;end
            EDGE_WAIT: if(wait_count==1) state<=EDGE_CAPTURE;else wait_count<=wait_count-1;
            EDGE_CAPTURE: begin
                if(!edge_term[0]) edge_a_product<=edge_product;
                else begin
                    edge_row[edge_index]<=edge_sum[31:0];
                    edge_current[edge_index]<=edge_sum[31:0];
                end
                if(edge_term==5) begin
                    // Edges are evaluated at the clipped start by multiply.
                    // Only enabled attribute planes need the offset walk.
                    if(!(|plane_enable)) state<=RASTER;
                    else if(offset_y!=0) state<=OFFSET_Y;
                    else if(offset_x!=0) state<=OFFSET_X;
                    else state<=RASTER;
                end else begin edge_term<=edge_term+1;state<=EDGE_LAUNCH;end
                if(edge_term[0]&&edge_sum[37:31]!={7{edge_sum[31]}})begin sticky_fault<=1;state<=FAILED;end
            end
            OFFSET_Y: begin
                for(i=0;i<7;i=i+1) if(plane_enable[i]) begin
                    plane_row[i]<=plane_next[i];plane_current[i]<=plane_next[i];
                end
                offset_y<=offset_y-1;
                if(offset_y==1) begin
                    if(offset_x!=0) state<=OFFSET_X;
                    else state<=RASTER;
                end
            end
            OFFSET_X: begin
                for(i=0;i<7;i=i+1) if(plane_enable[i]) begin
                    plane_row[i]<=plane_next[i];plane_current[i]<=plane_next[i];
                end
                offset_x<=offset_x-1;
                if(offset_x==1) state<=RASTER;
            end
            RASTER: if(inside_sample) begin
                if(need_texture) begin
                    if(fragment_ready)advance_candidate();
                end else begin
                    pending<=1;pending_addr<=sample_addr;pending_depth<=sample_depth;
                    pending_color<=pack_color(sample_color);advance_candidate();
                end
            end else advance_candidate();
            DRAIN: if(!fragment_busy&&!pending)begin state<=IDLE;done<=1;end
            FAILED: begin end
            default: begin state<=FAILED;sticky_fault<=1;end
            endcase
        end
    end
endmodule
