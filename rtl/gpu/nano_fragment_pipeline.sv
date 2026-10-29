// Ordered 64-context textured path. Every retained payload is stored through
// three native RAM ports. Ready/rejection bits and small handoff
// registers are explicit RTL state; there is no parallel array readback path.
module nano_fragment_pipeline #(parameter COORD_BITS = 32) (
    input wire clk, input wire reset,
    input wire color_write, input wire depth_test, input wire depth_write,
    input wire [1:0] depth_func, input wire key_enable, input wire [15:0] key_color,
    input wire in_valid, output wire in_ready,
    input wire [9:0] in_address, input wire [23:0] in_depth,
    input wire [23:0] in_factor,
    input wire signed [63:0] in_u,in_v,in_q,
    input wire in_sprite, input wire signed [31:0] in_source_x,in_source_y,
    input wire [23:0] stored_depth,
    output wire busy, output reg fault,
    output wire retired, output wire commit_color_write, output wire commit_depth_write,
    output wire [9:0] commit_address, output wire [23:0] commit_depth,
    output wire [15:0] commit_color,
    output wire tex_req_valid,input wire tex_req_ready,
    output wire signed [31:0] tex_req_x,tex_req_y,output wire [5:0] tex_req_tag,
    input wire tex_rsp_valid,output wire tex_rsp_ready,
    input wire [15:0] tex_rsp_texel,input wire [5:0] tex_rsp_tag
);
    reg [5:0] head,tail;
    reg [6:0] count;
    reg [63:0] ready;
    // Retirement only distinguishes accepted versus rejected fragments.
    // Depth and key rejection have identical attachment-write behavior.
    reg [63:0] rejected;
    reg head_read;
    reg [5:0] head_read_slot;
    reg head_read_rejected;
    reg depth_valid,depth_sprite;
    reg [5:0] depth_slot;
    reg [23:0] depth_value;
    reg signed [63:0] depth_u,depth_v,depth_q;
    reg signed [COORD_BITS-1:0] depth_x,depth_y;
    reg coordinate_read,coordinate_valid;
    reg [5:0] coordinate_read_slot,coordinate_slot;
    reg signed [COORD_BITS-1:0] coordinate_read_x,coordinate_read_y,coordinate_x,coordinate_y;
    reg [23:0] coordinate_factor;
    // A second retained coordinate absorbs the first blocked request. Ready
    // depends only on this registered occupancy, not the cache's tag/miss
    // decision. The main entry still feeds the cache directly on warm runs.
    reg coordinate_skid_valid;
    reg [5:0] coordinate_skid_slot;
    reg signed [COORD_BITS-1:0] coordinate_skid_x,coordinate_skid_y;
    reg [23:0] coordinate_skid_factor;
    reg [23:0] texture_factor [0:7];
    reg [5:0] texture_slot [0:7];
    reg [2:0] texture_head,texture_tail;
    reg [3:0] texture_count;
    reg [2:0] shade_valid;
    reg [5:0] shade_slot [0:2];

    wire retire_now=head_read&&!fault&&!reset;
    wire [5:0] next_head=head+{5'b0,retire_now};
    wire launch_head=(count>{6'b0,retire_now})&&ready[next_head]&&!fault&&!reset;
    wire texture_send=coordinate_valid&&(texture_count<8)&&!fault&&!reset;
    wire texture_accept=texture_send&&tex_req_ready;
    wire texture_response=tex_rsp_valid&&tex_rsp_ready;
    wire coordinate_space=!coordinate_skid_valid;
    wire capture_coordinate=coordinate_read&&coordinate_space;
    wire read_space=!coordinate_read||capture_coordinate;
    wire uv_valid,uv_ready,uv_in_ready,uv_overflow,uv_invalid;
    wire signed [31:0] uv_u,uv_v;
    wire [5:0] uv_tag;
    wire depth_passing=!depth_test||depth_func==2||
        (depth_func==0?depth_value<stored_depth:depth_value<=stored_depth);
    wire depth_reject=depth_valid&&!depth_passing&&!fault&&!reset;
    wire sprite_coordinate=depth_valid&&depth_passing&&depth_sprite&&read_space&&!uv_valid&&!fault&&!reset;
    wire uv_input=depth_valid&&depth_passing&&!depth_sprite&&!fault&&!reset;
    wire depth_consumed=depth_reject||sprite_coordinate||(uv_input&&uv_in_ready);
    // Accept replacement work whenever the retained depth stage could pass
    // downstream, without placing the depth-RAM comparison on upstream ready.
    // A rejected sample still clears depth_valid independently below. If its
    // downstream path is blocked, this conservative ready may insert one
    // bubble; it never overwrites an unconsumed sample and retains warm II1.
    wire depth_downstream_space=depth_sprite?(read_space&&!uv_valid):uv_in_ready;
    assign in_ready=(!depth_valid||depth_downstream_space)&&((count<64)||retire_now)&&!fault&&!reset;
    wire accept=in_valid&&in_ready;
    assign uv_ready=read_space&&!fault&&!reset;
    wire uv_coordinate=uv_valid&&uv_ready;
    wire factor_read=uv_coordinate||sprite_coordinate;
    wire [5:0] factor_address=uv_coordinate?uv_tag:depth_slot;
    nano_uv_texture_pipeline perspective(.clk(clk),.reset(reset),.ce(1'b1),
        .in_valid(uv_input),.in_ready(uv_in_ready),.in_u(depth_u),.in_v(depth_v),.in_q(depth_q),.in_tag(depth_slot),
        .out_valid(uv_valid),.out_ready(uv_ready),.out_u(uv_u),.out_v(uv_v),.out_tag(uv_tag),
        .out_overflow(uv_overflow),.out_invalid_q(uv_invalid));
    assign tex_req_valid=texture_send;
    assign tex_req_x=coordinate_x;assign tex_req_y=coordinate_y;assign tex_req_tag=coordinate_slot;
    assign tex_rsp_ready=texture_count!=0&&!fault&&!reset;
    wire keyed=key_enable&&tex_rsp_texel==key_color;
    wire shade_launch=texture_response&&!keyed;
    wire [5:0] response_slot=texture_slot[texture_head];
    wire [23:0] response_factor=texture_factor[texture_head];
    wire [7:0] tex_r={tex_rsp_texel[15:11],tex_rsp_texel[15:13]};
    wire [7:0] tex_g={tex_rsp_texel[10:5],tex_rsp_texel[10:9]};
    wire [7:0] tex_b={tex_rsp_texel[4:0],tex_rsp_texel[4:2]};
    wire signed [35:0] product_r,product_g,product_b;
    nano_mult18 shade_r(.clk(clk),.ce(1'b1),.reset(reset),
        .a(shade_launch?{10'b0,tex_r}:18'b0),.b(shade_launch?{10'b0,response_factor[7:0]}:18'b0),.product(product_r));
    nano_mult18 shade_g(.clk(clk),.ce(1'b1),.reset(reset),
        .a(shade_launch?{10'b0,tex_g}:18'b0),.b(shade_launch?{10'b0,response_factor[15:8]}:18'b0),.product(product_g));
    nano_mult18 shade_b(.clk(clk),.ce(1'b1),.reset(reset),
        .a(shade_launch?{10'b0,tex_b}:18'b0),.b(shade_launch?{10'b0,response_factor[23:16]}:18'b0),.product(product_b));
    wire [16:0] mod_r={1'b0,product_r[15:0]}+17'd128;
    wire [16:0] mod_g={1'b0,product_g[15:0]}+17'd128;
    wire [16:0] mod_b={1'b0,product_b[15:0]}+17'd128;
    wire [16:0] rounded_r=mod_r+(mod_r>>8);
    wire [16:0] rounded_g=mod_g+(mod_g>>8);
    wire [16:0] rounded_b=mod_b+(mod_b>>8);
    wire [7:0] out_r=rounded_r[15:8];
    wire [7:0] out_g=rounded_g[15:8];
    wire [7:0] out_b=rounded_b[15:8];
    wire [15:0] shaded={out_r[7:3],out_g[7:2],out_b[7:3]};
    wire result_write=shade_valid[2]&&!fault&&!reset;
    wire [35:0] metadata_data;
    wire [31:0] factor_data;
    wire [15:0] data0={6'b0,metadata_data[9:0]},data1=metadata_data[25:10],
        data2={8'b0,metadata_data[33:26]},data3=factor_data[15:0],
        data4={8'b0,factor_data[23:16]},data5;
    // Fields read together share the native word. The two different read
    // schedules (retirement and factor lookup) still own separate ports.
    nano_sdp36 context_metadata(.wclk(clk),.wce(1'b1),.we(accept),.waddr({3'b0,tail}),
        .wdata({2'b0,in_depth,in_address}),.rclk(clk),.rce(1'b1),.re(launch_head),
        .raddr({3'b0,next_head}),.reset_output(reset),.rdata(metadata_data));
    nano_sdp32 context_factor(.wclk(clk),.wce(1'b1),.we(accept),.waddr({3'b0,tail}),
        .wdata({8'b0,in_factor}),.rclk(clk),.rce(1'b1),.re(factor_read),
        .raddr({3'b0,factor_address}),.reset_output(reset),.rdata(factor_data));
    nano_sdp16 context5(.wclk(clk),.wce(1'b1),.we(result_write),.waddr({4'b0,shade_slot[2]}),.wdata(shaded),
        .rclk(clk),.rce(1'b1),.re(launch_head),.raddr({4'b0,next_head}),.reset_output(reset),.rdata(data5));
    assign busy=count!=0;
    assign retired=retire_now;
    assign commit_address=data0[9:0];assign commit_depth={data2[7:0],data1};assign commit_color=data5;
    assign commit_color_write=retire_now&&!head_read_rejected&&color_write;
    assign commit_depth_write=retire_now&&!head_read_rejected&&depth_write;
    genvar slot_index;
    generate for(slot_index=0;slot_index<64;slot_index=slot_index+1) begin: metadata
        always @(posedge clk) begin
            if(reset) begin ready[slot_index]<=0;rejected[slot_index]<=0;end
            else if(!fault) begin
                if(retire_now&&head==slot_index)ready[slot_index]<=0;
                if(result_write&&shade_slot[2]==slot_index)begin
                    ready[slot_index]<=1;rejected[slot_index]<=0;
                end
                if(texture_response&&keyed&&response_slot==slot_index)begin
                    ready[slot_index]<=1;rejected[slot_index]<=1;
                end
                if(depth_reject&&depth_slot==slot_index)begin
                    ready[slot_index]<=1;rejected[slot_index]<=1;
                end
            end
        end
    end endgenerate
    integer i;
    always @(posedge clk) begin
        if(reset) begin
            head<=0;tail<=0;count<=0;head_read<=0;head_read_slot<=0;head_read_rejected<=0;
            depth_valid<=0;depth_sprite<=0;depth_slot<=0;depth_value<=0;
            depth_u<=0;depth_v<=0;depth_q<=0;depth_x<=0;depth_y<=0;
            coordinate_read<=0;coordinate_valid<=0;coordinate_read_slot<=0;coordinate_slot<=0;
            coordinate_read_x<=0;coordinate_read_y<=0;coordinate_x<=0;coordinate_y<=0;coordinate_factor<=0;
            coordinate_skid_valid<=0;coordinate_skid_slot<=0;
            coordinate_skid_x<=0;coordinate_skid_y<=0;coordinate_skid_factor<=0;
            texture_head<=0;texture_tail<=0;texture_count<=0;shade_valid<=0;fault<=0;
            for(i=0;i<3;i=i+1)shade_slot[i]<=0;
        end else if(!fault) begin
            case({accept,retire_now})
            2'b10:count<=count+1;2'b01:count<=count-1;default:begin end
            endcase
            if(retire_now)begin
                if(head_read_slot!=head||!ready[head])fault<=1;
                head<=head+1;
            end
            head_read<=launch_head;
            if(launch_head)begin head_read_slot<=next_head;head_read_rejected<=rejected[next_head];end
            if(result_write)begin
                if(ready[shade_slot[2]])fault<=1;

            end
            shade_valid<={shade_valid[1:0],shade_launch};
            shade_slot[2]<=shade_slot[1];shade_slot[1]<=shade_slot[0];
            if(shade_launch)shade_slot[0]<=response_slot;
            if(tex_rsp_valid&&texture_count==0)fault<=1;
            case({texture_accept,texture_response})
            2'b10:texture_count<=texture_count+1;2'b01:texture_count<=texture_count-1;default:begin end
            endcase
            if(texture_response)begin
                if(tex_rsp_tag!=response_slot)fault<=1;
                texture_head<=texture_head+1;
                if(keyed)begin
                    if(ready[response_slot])fault<=1;

                end
            end
            if(texture_accept)begin
                texture_factor[texture_tail]<=coordinate_factor;texture_slot[texture_tail]<=coordinate_slot;
                texture_tail<=texture_tail+1;
            end
            if(texture_accept)begin
                if(coordinate_skid_valid)begin
                    coordinate_valid<=1;coordinate_skid_valid<=0;
                    coordinate_slot<=coordinate_skid_slot;
                    coordinate_x<=coordinate_skid_x;coordinate_y<=coordinate_skid_y;
                    coordinate_factor<=coordinate_skid_factor;
                end else coordinate_valid<=0;
            end
            if(capture_coordinate)begin
                coordinate_read<=0;
                if(!coordinate_valid||texture_accept)begin
                    coordinate_valid<=1;coordinate_slot<=coordinate_read_slot;
                    coordinate_x<=coordinate_read_x;coordinate_y<=coordinate_read_y;
                    coordinate_factor<={data4[7:0],data3};
                end else begin
                    coordinate_skid_valid<=1;coordinate_skid_slot<=coordinate_read_slot;
                    coordinate_skid_x<=coordinate_read_x;coordinate_skid_y<=coordinate_read_y;
                    coordinate_skid_factor<={data4[7:0],data3};
                end
            end
            if(uv_coordinate)begin
                if(uv_invalid||uv_overflow)fault<=1;
                coordinate_read<=1;coordinate_read_slot<=uv_tag;
                coordinate_read_x<=uv_u;coordinate_read_y<=uv_v;
            end else if(sprite_coordinate)begin
                coordinate_read<=1;coordinate_read_slot<=depth_slot;
                coordinate_read_x<=depth_x;coordinate_read_y<=depth_y;
            end
            if(depth_consumed)depth_valid<=0;
            if(depth_reject)begin
                if(ready[depth_slot])fault<=1;

            end
            if(accept)begin
                depth_valid<=1;depth_sprite<=in_sprite;depth_slot<=tail;depth_value<=in_depth;
                depth_u<=in_u;depth_v<=in_v;depth_q<=in_q;depth_x<=in_source_x;depth_y<=in_source_y;
                tail<=tail+1;
            end
        end
    end
endmodule
