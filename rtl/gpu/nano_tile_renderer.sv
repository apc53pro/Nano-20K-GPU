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
    output wire busy, output reg done, output reg fault,
    input wire pix_we, input wire [9:0] pix_addr,
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
        DRAIN=17, FAILED=18;
    reg [4:0] state;
    reg [31:0] words [0:79];
    reg [9:0] origin_x;
    reg [8:0] origin_y;
    reg [1:0] kind;
    reg [2:0] shade;
    reg [1:0] depth_func;
    reg color_write, depth_test, depth_write, key_enable, need_texture;
    reg [15:0] key_color;
    reg [23:0] constant_color, constant_depth;
    reg signed [31:0] x, y, x0, y0, x1, y1;
    reg signed [31:0] offset_x, offset_y;
    reg signed [31:0] source_x, source_y, source_start_x;
    reg signed [31:0] line_end_x, line_end_y;
    reg signed [15:0] line_dx, line_dy, line_sx, line_sy, line_error;
    reg signed [31:0] edge_row [0:2], edge_current [0:2];
    reg signed [63:0] plane_row [0:6], plane_current [0:6];
    reg [6:0] plane_enable;
    reg [2:0] edge_term;
    reg [1:0] wait_count;
    reg signed [35:0] edge_a_product;

    function automatic signed [31:0] maximum;
        input signed [31:0] a,b;
        begin maximum=(a>b)?a:b; end
    endfunction
    function automatic signed [31:0] minimum;
        input signed [31:0] a,b;
        begin minimum=(a<b)?a:b; end
    endfunction
    function automatic [23:0] quantize;
        input signed [63:0] value;
        input [23:0] limit;
        reg [48:0] rounded;
        begin
            rounded={1'b0,value[63:16]} +
                ((value[15:0]>16'h8000)||((value[15:0]==16'h8000)&&value[16]));
            if(value<=0) quantize=0;
            else if(rounded>{25'b0,limit}) quantize=limit;
            else quantize=rounded[23:0];
        end
    endfunction
    function automatic [15:0] pack_color;
        input [23:0] rgb;
        begin pack_color={rgb[7:3],rgb[15:10],rgb[23:19]}; end
    endfunction
    function automatic [7:0] quantize_color;
        input signed [63:0] value;
        reg [23:0] full_value;
        begin full_value=quantize(value,24'd255);quantize_color=full_value[7:0];end
    endfunction
    function automatic add_overflow;
        input signed [63:0] a,b;
        reg signed [63:0] sum;
        begin sum=a+b;add_overflow=(a[63]==b[63])&&(sum[63]!=a[63]);end
    endfunction
    function automatic edge_overflow;
        input signed [31:0] a,b;
        reg signed [31:0] sum;
        begin sum=a+b;edge_overflow=(a[31]==b[31])&&(sum[31]!=a[31]);end
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
    function automatic signed [63:0] step_x;
        input integer index;
        begin step_x=$signed({words[41+6*index],words[40+6*index]}); end
    endfunction
    function automatic signed [63:0] step_y;
        input integer index;
        begin step_y=$signed({words[43+6*index],words[42+6*index]}); end
    endfunction
    function automatic signed [63:0] seed;
        input integer index;
        begin seed=$signed({words[39+6*index],words[38+6*index]}); end
    endfunction
    function automatic signed [31:0] edge_a;
        input integer index;
        begin edge_a=$signed(words[26+4*index]); end
    endfunction
    function automatic signed [31:0] edge_b;
        input integer index;
        begin edge_b=$signed(words[27+4*index]); end
    endfunction

    wire signed [31:0] a_x=$signed(words[17]), a_y=$signed(words[18]);
    wire signed [31:0] b_x=$signed(words[20]), b_y=$signed(words[21]);
    wire swap_line=(b_x<a_x)||((b_x==a_x)&&(b_y<a_y));
    wire signed [31:0] first_x=swap_line?b_x:a_x, first_y=swap_line?b_y:a_y;
    wire signed [31:0] last_x=swap_line?a_x:b_x, last_y=swap_line?a_y:b_y;
    wire line_kind=words[0][7:0]==2;
    wire signed [31:0] bounds_x0=line_kind?minimum(a_x,b_x):$signed(words[8]);
    wire signed [31:0] bounds_y0=line_kind?minimum(a_y,b_y):$signed(words[9]);
    wire signed [31:0] bounds_x1=line_kind?maximum(a_x,b_x)+32'sd1:$signed(words[10]);
    wire signed [31:0] bounds_y1=line_kind?maximum(a_y,b_y)+32'sd1:$signed(words[11]);
    wire signed [31:0] window_x0=maximum(maximum(maximum(bounds_x0,$signed(words[4])),
        $signed({22'b0,origin_x})),32'sd0);
    wire signed [31:0] window_y0=maximum(maximum(maximum(bounds_y0,$signed(words[5])),
        $signed({23'b0,origin_y})),32'sd0);
    wire signed [31:0] window_x1=minimum(minimum(minimum(bounds_x1,$signed(words[6])),
        $signed({22'b0,origin_x})+32'sd32),32'sd640);
    wire signed [31:0] window_y1=minimum(minimum(minimum(bounds_y1,$signed(words[7])),
        $signed({23'b0,origin_y})+32'sd32),32'sd480);
    wire grid_inside=x>=x0&&x<x1&&y>=y0&&y<y1;
    wire triangle_inside=(edge_current[0]>0||(edge_current[0]==0&&words[29][0]))&&
        (edge_current[1]>0||(edge_current[1]==0&&words[33][0]))&&
        (edge_current[2]>0||(edge_current[2]==0&&words[37][0]));
    wire inside_sample=(kind==0)?triangle_inside:grid_inside;
    wire [9:0] sample_addr={y[4:0],x[4:0]};
    wire [23:0] sample_depth=(kind==0)?quantize(plane_current[0],24'hffffff):constant_depth;
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
    wire ext_write=(state==IDLE)&&pix_we&&!fault&&!reset;
    wire color_we=ext_write||(fast_commit&&color_write)||(fragment_color_write&&!fault&&!reset);
    wire depth_we=ext_write||(fast_commit&&depth_write)||(fragment_depth_write&&!fault&&!reset);
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
    assign busy=(state!=IDLE)&&(state!=FAILED);
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
    nano_fragment_pipeline fragments(.clk(clk),.reset(reset),
        .color_write(color_write),.depth_test(depth_test),.depth_write(depth_write),.depth_func(depth_func),
        .key_enable(key_enable),.key_color(key_color),
        .in_valid(state==RASTER&&inside_sample&&need_texture&&!fault&&!reset),.in_ready(fragment_ready),
        .in_address(sample_addr),.in_depth(sample_depth),
        .in_factor((kind==3||shade==2)?24'hffffff:sample_color),
        .in_u(plane_current[5]),.in_v(plane_current[6]),.in_q(plane_current[4]),
        .in_sprite(kind==3),.in_source_x(source_x),.in_source_y(source_y),.stored_depth(stored_depth),
        .busy(fragment_busy),.fault(fragment_fault),.retired(fragment_retired),
        .commit_color_write(fragment_color_write),.commit_depth_write(fragment_depth_write),
        .commit_address(fragment_address),.commit_depth(fragment_depth),.commit_color(fragment_color),
        .tex_req_valid(tex_req_valid),.tex_req_ready(tex_req_ready),.tex_req_x(tex_req_x),.tex_req_y(tex_req_y),
        .tex_req_tag(tex_req_tag),.tex_rsp_valid(tex_rsp_valid),.tex_rsp_ready(tex_rsp_ready),
        .tex_rsp_texel(tex_rsp_texel),.tex_rsp_tag(tex_rsp_tag));

    integer i;
    wire signed [31:0] line_abs_x=(last_x>=first_x)?last_x-first_x:first_x-last_x;
    wire signed [31:0] line_abs_y=(last_y>=first_y)?last_y-first_y:first_y-last_y;
    wire signed [31:0] line_negative_y=-line_abs_y;
    wire signed [31:0] line_initial_error=line_abs_x-line_abs_y;
    wire record_needs_texture=words[0][7:0]==3||
        (words[0][7:0]==0&&words[1][7:0]>=2&&(words[0][16]||words[0][19]));
    wire signed [32:0] sprite_width=$signed({words[10][31],words[10]})-$signed({words[8][31],words[8]});
    wire signed [32:0] sprite_height=$signed({words[11][31],words[11]})-$signed({words[9][31],words[9]});
    wire signed [33:0] sprite_source_end_x=$signed({2'b0,words[17]})+$signed({sprite_width[32],sprite_width});
    wire signed [33:0] sprite_source_end_y=$signed({2'b0,words[18]})+$signed({sprite_height[32],sprite_height});
    reg record_bad;
    integer check_word;
    always @* begin
        record_bad=words[0][15:8]!=1||words[0][7:0]>3||words[0][31:20]!=0||
            words[1][7:0]>4||words[1][15:8]>2||words[1][31:19]!=0||
            words[2][31:24]!=0||words[3][31:16]!=0||words[16][31:24]!=0||
            words[19]!=0||(!words[0][19]&&words[3]!=0)||
            (record_needs_texture&&!words[1][18]);
        if(words[1][18])begin
            // Reject wide packed fields before truncating the cache ports.
            record_bad=record_bad||words[12][31:23]!=0||words[15][31:23]!=0||
                words[13][31:11]!=0||words[14][31:11]!=0;
        end else record_bad=record_bad||words[12]!=0||words[13]!=0||words[14]!=0||words[15]!=0;
        case(words[0][7:0])
        0:begin
            record_bad=record_bad||words[16]!=0||words[17]!=0||words[18]!=0||
                words[29]>1||words[33]>1||words[37]>1||
                words[26][31:17]!={15{words[26][17]}}||words[27][31:17]!={15{words[27][17]}}||
                words[30][31:17]!={15{words[30][17]}}||words[31][31:17]!={15{words[31][17]}}||
                words[34][31:17]!={15{words[34][17]}}||words[35][31:17]!={15{words[35][17]}}||
                $signed(words[8])<0||$signed(words[9])<0||words[10]>640||words[11]>480;
        end
        1:begin
            record_bad=record_bad||words[17]!=0||words[18]!=0||words[1][7:0]!=0||words[0][19];
            for(check_word=20;check_word<80;check_word=check_word+1)record_bad=record_bad||words[check_word]!=0;
        end
        2:begin
            record_bad=record_bad||words[8]!=0||words[9]!=0||words[10]!=0||words[11]!=0||
                words[1][7:0]!=0||words[0][19]||words[17]>=640||words[20]>=640||words[18]>=480||words[21]>=480;
            for(check_word=22;check_word<80;check_word=check_word+1)record_bad=record_bad||words[check_word]!=0;
        end
        3:begin
            record_bad=record_bad||(words[1][7:0]!=0&&words[1][7:0]!=2)||words[17][31]||words[18][31];
            if(sprite_width>0&&sprite_height>0)record_bad=record_bad||
                sprite_source_end_x>$signed({2'b0,words[13]})||sprite_source_end_y>$signed({2'b0,words[14]});
            for(check_word=20;check_word<80;check_word=check_word+1)record_bad=record_bad||words[check_word]!=0;
        end
        default:begin end
        endcase
    end
    task automatic advance_candidate;
        reg signed [16:0] twice_error;
        reg signed [16:0] next_error;
        reg overflowed;
        begin
            overflowed=0;
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
            end else if(x+1<x1) begin
                x<=x+1; source_x<=source_x+1;
                for(integer j=0;j<3;j=j+1)begin
                    overflowed=overflowed||edge_overflow(edge_current[j],edge_a(j)<<<4);
                    edge_current[j]<=edge_current[j]+(edge_a(j)<<<4);
                end
                for(integer j=0;j<7;j=j+1) if(plane_enable[j])begin
                    overflowed=overflowed||add_overflow(plane_current[j],step_x(j));
                    plane_current[j]<=plane_current[j]+step_x(j);
                end
                state<=RASTER;
            end else if(y+1<y1) begin
                x<=x0; y<=y+1; source_x<=source_start_x; source_y<=source_y+1;
                for(integer j=0;j<3;j=j+1) begin
                    overflowed=overflowed||edge_overflow(edge_row[j],edge_b(j)<<<4);
                    edge_row[j]<=edge_row[j]+(edge_b(j)<<<4);
                    edge_current[j]<=edge_row[j]+(edge_b(j)<<<4);
                end
                for(integer j=0;j<7;j=j+1) if(plane_enable[j]) begin
                    overflowed=overflowed||add_overflow(plane_row[j],step_y(j));
                    plane_row[j]<=plane_row[j]+step_y(j);
                    plane_current[j]<=plane_row[j]+step_y(j);
                end
                state<=RASTER;
            end else state<=DRAIN;
            if(overflowed)begin fault<=1;state<=FAILED;end
        end
    endtask

    always @(posedge clk) begin
        if(reset) begin
            state<=IDLE; done<=0; fault<=0; pending<=0; pix_rvalid<=0;
            origin_x<=0; origin_y<=0;
            kind<=0; shade<=0; depth_func<=0;
            color_write<=0;depth_test<=0;depth_write<=0;key_enable<=0;need_texture<=0;
            key_color<=0;constant_color<=0;constant_depth<=0;
            x<=0;y<=0;x0<=0;y0<=0;x1<=0;y1<=0;offset_x<=0;offset_y<=0;
            source_x<=0;source_y<=0;source_start_x<=0;
            line_end_x<=0;line_end_y<=0;line_dx<=0;line_dy<=0;line_sx<=0;line_sy<=0;line_error<=0;
            edge_term<=0;wait_count<=0;edge_a_product<=0;plane_enable<=0;
            pending_addr<=0;pending_depth<=0;pending_color<=0;
            for(i=0;i<3;i=i+1) begin edge_row[i]<=0;edge_current[i]<=0;end
            for(i=0;i<7;i=i+1) begin plane_row[i]<=0;plane_current[i]<=0;end
        end else begin
            done<=0; pending<=0; pix_rvalid<=external_re;
            if(cmd_word_we) begin
                if(state!=IDLE||cmd_word_index>=80) begin fault<=1;state<=FAILED;end
                else words[cmd_word_index]<=cmd_word_data;
            end
            if((tex_flush&&busy)||tex_fault||fragment_fault) begin fault<=1;state<=FAILED;end
            else case(state)
            IDLE: if(cmd_valid&&!fault) begin
                origin_x<=cmd_tile_x;origin_y<=cmd_tile_y;state<=START;
            end
            START: begin
                if(record_bad||
                    origin_x>=640||origin_y>=480||origin_x[4:0]!=0||origin_y[4:0]!=0) begin
                    fault<=1;state<=FAILED;
                end else if((window_x0>=window_x1||window_y0>=window_y1)&&!words[1][18]) state<=DRAIN;
                else begin
                    kind<=words[0][1:0];shade<=words[1][2:0];depth_func<=words[1][9:8];
                    color_write<=words[0][16];depth_test<=words[0][17];depth_write<=words[0][18];
                    key_enable<=words[0][19];key_color<=words[3][15:0];
                    constant_color<=words[2][23:0];constant_depth<=words[16][23:0];
                    need_texture<=(words[0][7:0]==3||(words[0][7:0]==0&&words[1][7:0]>=2))&&
                        (words[0][16]||words[0][19]);
                    x0<=window_x0;y0<=window_y0;x1<=window_x1;y1<=window_y1;
                    x<=line_kind?first_x:window_x0;y<=line_kind?first_y:window_y0;
                    line_end_x<=last_x;line_end_y<=last_y;
                    line_dx<=line_abs_x[15:0];
                    line_dy<=line_negative_y[15:0];
                    line_sx<=first_x<last_x?16'sd1:-16'sd1;
                    line_sy<=first_y<last_y?16'sd1:-16'sd1;
                    line_error<=line_initial_error[15:0];
                    source_x<=$signed(words[17])+window_x0-bounds_x0;
                    source_start_x<=$signed(words[17])+window_x0-bounds_x0;
                    source_y<=$signed(words[18])+window_y0-bounds_y0;
                    offset_x<=window_x0-bounds_x0;offset_y<=window_y0-bounds_y0;
                    for(i=0;i<7;i=i+1) plane_row[i]<=seed(i);
                    plane_enable<={{3{(words[0][7:0]==0&&words[1][7:0]>=2&&(words[0][16]||words[0][19]))}},
                        {3{(words[0][16]&&(words[1][7:0]==1||words[1][7:0]==4))}},
                        (words[0][17]||words[0][18])};
                    edge_term<=0;
                    // The cache validates every present descriptor, even for
                    // empty tiles or state whose color path does not sample it.
                    if(words[1][18]) state<=CONFIG;
                    else if(words[0][7:0]==0) state<=EDGE_LAUNCH;
                    else state<=RASTER;
                end
            end
            CONFIG: if(tex_cfg_ready) begin
                if(window_x0>=window_x1||window_y0>=window_y1)state<=DRAIN;
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
                    if(offset_y!=0) state<=OFFSET_Y;
                    else if(offset_x!=0) state<=OFFSET_X;
                    else begin for(i=0;i<7;i=i+1) plane_current[i]<=plane_row[i];state<=RASTER;end
                end else begin edge_term<=edge_term+1;state<=EDGE_LAUNCH;end
                if(edge_term[0]&&edge_sum[37:31]!={7{edge_sum[31]}})begin fault<=1;state<=FAILED;end
            end
            OFFSET_Y: begin
                for(i=0;i<7;i=i+1) if(plane_enable[i]) plane_row[i]<=plane_row[i]+step_y(i);
                offset_y<=offset_y-1;
                if(offset_y==1) begin
                    if(offset_x!=0) state<=OFFSET_X;
                    else begin
                        for(i=0;i<7;i=i+1) plane_current[i]<=plane_enable[i]?plane_row[i]+step_y(i):plane_row[i];
                        state<=RASTER;
                    end
                end
                for(i=0;i<7;i=i+1)if(plane_enable[i]&&add_overflow(plane_row[i],step_y(i)))begin fault<=1;state<=FAILED;end
            end
            OFFSET_X: begin
                for(i=0;i<7;i=i+1) if(plane_enable[i]) plane_row[i]<=plane_row[i]+step_x(i);
                offset_x<=offset_x-1;
                if(offset_x==1) begin
                    for(i=0;i<7;i=i+1) plane_current[i]<=plane_enable[i]?plane_row[i]+step_x(i):plane_row[i];
                    state<=RASTER;
                end
                for(i=0;i<7;i=i+1)if(plane_enable[i]&&add_overflow(plane_row[i],step_x(i)))begin fault<=1;state<=FAILED;end
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
            default: begin state<=FAILED;fault<=1;end
            endcase
        end
    end
endmodule
