// GPU triangle setup for kind-4 vertex triangles.
//
// The core streams a record's first 16 words into this block and, for kind 4,
// starts it once per tile reference. It writes the tile renderer's ordinary
// flat-triangle command words (kind 0) for that tile, so the renderer itself
// is unchanged:
//   words 0..19   header, full-viewport scissor, bounds = the record's bounds
//                 intersected with the tile, zero texture/constant fields;
//   words 26..37  the three edges {A, B, C, inclusive};
//   words 38..43  the depth plane {seed, step_x, step_y}, seeded at the
//                 window corner so the renderer never walks to the tile start.
// The arithmetic is reference/src/setup.cpp vertex_triangle_setup():
//   edge e runs from vertex e to e+1: A = Ye - Ye+1, B = Xe+1 - Xe,
//   C = -(A*Xe + B*Ye); the area is edge 0's value at vertex 2. Zero area
//   draws nothing; a negative area is culled when requested. Otherwise its
//   edges are negated: the edge set of the triangle with vertices 1 and 2
//   exchanged.
//   NX = Z0*A1 + Z1*A2 + Z2*A0, NY = Z0*B1 + Z1*B2 + Z2*B0 (Z = D24 * 256);
//   G = sign(N*area) * min(floor(|N|*4096/|area| + 1/2), 2^42-1) (Q16/pixel);
//   seed(x,y) = Z0*256 + floor((SX*GX + SY*GY + 8) / 16),
//   SX = 16x+8-X0, SY = 16y+8-Y0.
// A tile whose window cannot contain a covered pixel center (some edge's
// largest value over the window is negative) is rejected without a draw.
//
// One signed 44x16 multiplier (two-clock latency), one 64-bit accumulator and
// one restoring divider compute everything. Each issued step carries its
// accumulator operation down the multiplier pipeline, and a follow-up action
// reads the settled result, so operations apply in issue order:
//   issue at t -> operands t+1 -> product and accumulate t+2 -> action t+3.
// Operand A is a coordinate, a depth, the quotient or 1; operand B is an
// edge coefficient, a sample offset or a constant. Edge words and gradients
// reach the accumulator as products.
module nano_triangle_setup (
    input wire clk, input wire reset,
    // Record words as fetched; only indices 0..15 are captured.
    input wire capture_we, input wire [6:0] capture_index, input wire [31:0] capture_data,
    output wire vertex_kind,                 // captured word 0 selects kind 4
    input wire start,                        // run for the current tile
    input wire [9:0] tile_x, input wire [8:0] tile_y,
    output reg word_we, output reg [6:0] word_index, output reg [31:0] word_data,
    output reg done, output reg draw, output reg bad
);
    // ---------------------------------------------------------------- capture
    reg [2:0] flags;            // color write, depth test, depth write
    reg cull;
    reg [1:0] depth_func;
    reg [23:0] color;
    reg [13:0] vx [0:2];
    reg [12:0] vy [0:2];
    reg [31:0] vz [0:2];
    reg [10:0] bound_x0, bound_x1, bound_y0, bound_y1;
    reg [15:0] word_bad;        // per-word malformed flags, recomputed on rewrite
    reg kind4;
    assign vertex_kind=kind4;
    wire [3:0] vertex_offset=capture_index[3:0]-4'd3, depth_offset=capture_index[3:0]-4'd6;
    wire [1:0] vertex_slot=vertex_offset[1:0], depth_slot=depth_offset[1:0];
    always @(posedge clk) begin
        if(reset) begin word_bad<=0; kind4<=0; end
        else if(capture_we&&capture_index<16) begin
            case(capture_index[3:0])
            0: begin
                kind4<=capture_data[7:0]==4;
                word_bad[0]<=capture_data[7:0]!=4||capture_data[15:8]!=1||capture_data[19]||(|capture_data[31:21]);
                flags<=capture_data[18:16]; cull<=capture_data[20];
            end
            1: begin
                word_bad[1]<=(|capture_data[31:10])||(|capture_data[7:0])||(&capture_data[9:8]);
                depth_func<=capture_data[9:8];
            end
            2: begin word_bad[2]<=|capture_data[31:24]; color<=capture_data[23:0]; end
            3,4,5: begin
                word_bad[capture_index[3:0]]<=(|capture_data[15:14])||(|capture_data[31:29]);
                vx[vertex_slot]<=capture_data[13:0];
                vy[vertex_slot]<=capture_data[28:16];
            end
            6,7,8: begin
                word_bad[capture_index[3:0]]<=0;
                vz[depth_slot]<=capture_data;
            end
            9: begin
                word_bad[9]<=(|capture_data[31:27])||(|capture_data[15:11]);
                bound_x0<=capture_data[10:0]; bound_x1<=capture_data[26:16];
            end
            10: begin
                word_bad[10]<=(|capture_data[31:27])||(|capture_data[15:11]);
                bound_y0<=capture_data[10:0]; bound_y1<=capture_data[26:16];
            end
            default: word_bad[capture_index[3:0]]<=|capture_data;
            endcase
        end
    end

    // ------------------------------------------------- tile window
    // The record's bounds act as a scissor; tiles lie inside the viewport, so
    // the window does too. *_last is the inclusive last pixel.
    wire [10:0] tile_x1={1'b0,tile_x}+11'd32;
    wire [10:0] tile_y1={2'b0,tile_y}+11'd32;
    reg [9:0] window_x0, window_x_last;
    reg [8:0] window_y0, window_y_last;
    wire [10:0] window_x0_next=(bound_x0>{1'b0,tile_x})?bound_x0:{1'b0,tile_x};
    wire [10:0] window_x1_next=(bound_x1<tile_x1)?bound_x1:tile_x1;
    wire [10:0] window_y0_next=(bound_y0>{2'b0,tile_y})?bound_y0:{2'b0,tile_y};
    wire [10:0] window_y1_next=(bound_y1<tile_y1)?bound_y1:tile_y1;
    wire window_empty=window_x0_next>=window_x1_next||window_y0_next>=window_y1_next;

    // ------------------------------------------------- edges
    wire signed [14:0] edge_a [0:2], edge_b [0:2];
    assign edge_a[0]=$signed({2'b0,vy[0]})-$signed({2'b0,vy[1]});
    assign edge_a[1]=$signed({2'b0,vy[1]})-$signed({2'b0,vy[2]});
    assign edge_a[2]=$signed({2'b0,vy[2]})-$signed({2'b0,vy[0]});
    assign edge_b[0]=$signed({1'b0,vx[1]})-$signed({1'b0,vx[0]});
    assign edge_b[1]=$signed({1'b0,vx[2]})-$signed({1'b0,vx[1]});
    assign edge_b[2]=$signed({1'b0,vx[0]})-$signed({1'b0,vx[2]});
    reg negative;               // negative area: every edge is negated
    // Signs of the emitted (possibly negated) edges.
    wire [2:0] a_positive, b_positive, edge_tie;
    generate for(genvar e=0;e<3;e=e+1) begin:ties
        wire a_zero=edge_a[e]==0, b_zero=edge_b[e]==0;
        assign a_positive[e]=!a_zero&&(edge_a[e][14]==negative);
        assign b_positive[e]=!b_zero&&(edge_b[e][14]==negative);
        // Top-left: A>0, or A=0 with B>0 (dy<0, or dy=0 with dx>0).
        assign edge_tie[e]=a_positive[e]||(a_zero&&b_positive[e]);
    end endgenerate
    wire use_depth=flags[1]||flags[2];

    // ------------------------------------------------- sequencer
    localparam [1:0] IDLE=0, RUN=1;
    reg [1:0] state;
    reg [5:0] step;
    reg orient_done;

    localparam [2:0] OP_NONE=0, OP_SET=1, OP_SETNEG=2, OP_ADD=3, OP_SUB=4,
        OP_ABS=5, OP_SHIFT_ADD=6;
    localparam [3:0] AC_NONE=0, AC_WORD_LO=1, AC_WORD_HI=2, AC_TEST=3, AC_AREA=4,
        AC_AREA_LOAD=5, AC_SIGN_X=6, AC_SIGN_Y=7, AC_DIV_X=8, AC_HOLD_NY=9;
    // Operand sources.
    localparam [1:0] A_COORD=0, A_DEPTH=1, A_QUOTIENT=2, A_ONE=3;
    localparam [2:0] B_EDGE_A=0, B_EDGE_B=1, B_SAMPLE_X=2, B_SAMPLE_Y=3,
        B_PLUS1=4, B_MINUS1=5, B_EIGHT=6, B_256=7;
    localparam [2:0] C_X=0, C_Y=1, C_CORNER_X=2, C_CORNER_Y=3;
    reg [2:0] issue_op, op_q1, op_q2;
    reg [3:0] issue_action, act_q1, act_q2, act_q3;
    reg [6:0] issue_word, word_q1, word_q2, word_q3;
    reg [1:0] a_src, e_sel, v_sel, z_sel;
    reg [2:0] b_src, c_src;
    reg signed [43:0] mul_a_q;
    reg signed [15:0] mul_b_q;
    reg signed [59:0] product;
    reg signed [63:0] acc;
    reg sign_x, sign_y;
    reg [47:0] hold_ny;              // |NY| while the divider works on X
    wire signed [15:0] sample_x=$signed({2'b0,window_x0,4'b1000})-$signed({2'b0,vx[0]});
    wire signed [15:0] sample_y=$signed({3'b0,window_y0,4'b1000})-$signed({3'b0,vy[0]});

    // Divider: |N|*4096 (< 2^59) / |area| (< 2^28). One check clock for
    // saturation, 43 quotient bits (the last rounds), then the rounded,
    // saturated magnitude replaces the quotient.
    reg div_busy;
    reg [5:0] div_count;
    reg [28:0] area;
    reg [41:0] numerator;
    reg [42:0] quotient;
    reg [28:0] remainder;
    reg saturate;

    // Pre-selected operands.
    wire signed [14:0] e_a=edge_a[e_sel], e_b=edge_b[e_sel];
    reg [14:0] coordinate;
    always @* begin
        case(c_src)
        C_X: coordinate={1'b0,vx[v_sel]};
        C_Y: coordinate={2'b0,vy[v_sel]};
        // Window corner (Q4 16x+8) where the emitted edge's value is largest.
        C_CORNER_X: coordinate={1'b0,a_positive[e_sel]?window_x_last:window_x0,4'b1000};
        default: coordinate={2'b0,b_positive[e_sel]?window_y_last:window_y0,4'b1000};
        endcase
    end
    wire [43:0] mul_a=(a_src==A_COORD)?{29'd0,coordinate}:
                      (a_src==A_DEPTH)?{12'd0,vz[z_sel]}:
                      (a_src==A_QUOTIENT)?{1'b0,quotient}:44'd1;
    reg [15:0] mul_b;
    always @* begin
        case(b_src)
        B_EDGE_A: mul_b={e_a[14],e_a};
        B_EDGE_B: mul_b={e_b[14],e_b};
        B_SAMPLE_X: mul_b=sample_x;
        B_SAMPLE_Y: mul_b=sample_y;
        B_PLUS1: mul_b=16'd1;
        B_MINUS1: mul_b=16'hffff;
        B_EIGHT: mul_b=16'd8;
        default: mul_b=16'd256;
        endcase
    end
    // Negated edges flip the sign of each edge term.
    wire [2:0] op_c_first=negative?OP_SET:OP_SETNEG;
    wire [2:0] op_c_next=negative?OP_ADD:OP_SUB;
    wire [2:0] op_e_next=negative?OP_SUB:OP_ADD;

    // Issue program:
    //  0..3 area (edge 0 at vertex 2), 4 |area|, 5 wait for orientation;
    //  6..17 edges: C (written), then the window-corner coverage test;
    //  18..23 A and B words; 24..31 NX, NY (X divides meanwhile);
    //  32 wait, 33..35 X gradient words and seed term, 36 divide Y, 37 wait;
    //  38..43 seed and Y gradient; 60 wait for the last words.
    wire [3:0] in_edge=step[3:0]-4'd6;           // steps 6..17: 4*edge + k
    wire hold=(step==5)||(step==32)||(step==34)||(step==36)||(step==37)||(step==41)||(step>=44);
    always @* begin
        issue_op=OP_NONE; issue_action=AC_NONE; issue_word=0;
        a_src=A_ONE; b_src=B_PLUS1; c_src=C_X; e_sel=0; v_sel=0; z_sel=0;
        if(state==RUN) begin
            // Operand selections are combinational on the step; they are
            // sampled only where the step issues.
            if(step<4) begin
                // -A0*X0 - B0*Y0 + A0*X2 + B0*Y2.
                a_src=A_COORD; e_sel=0; c_src=step[0]?C_Y:C_X; v_sel=step[1]?2'd2:2'd0;
                b_src=step[0]?B_EDGE_B:B_EDGE_A;
            end else if(step>=6&&step<18) begin
                e_sel=(step<10)?2'd0:((step<14)?2'd1:2'd2); v_sel=e_sel;
                // k = in_edge[1:0]: X, Y (C), then the corner X, Y (test).
                a_src=A_COORD; b_src=in_edge[0]?B_EDGE_B:B_EDGE_A;
                c_src=(in_edge[1]==0)?(in_edge[0]?C_Y:C_X):(in_edge[0]?C_CORNER_Y:C_CORNER_X);
            end else if(step>=18&&step<24) begin
                e_sel=(step<20)?2'd0:((step<22)?2'd1:2'd2);
                a_src=A_ONE; b_src=step[0]?B_EDGE_B:B_EDGE_A;
            end else if(step>=24&&step<32) begin
                // N = Z0*E1 + Z1*E2 + Z2*E0 with E = A (24..26) or B (28..30).
                a_src=A_DEPTH; z_sel=step[1:0]; e_sel=(step[1:0]==0)?2'd1:((step[1:0]==1)?2'd2:2'd0);
                b_src=step[2]?B_EDGE_B:B_EDGE_A;
            end else case(step)
            33: begin a_src=A_QUOTIENT; b_src=sign_x?B_MINUS1:B_PLUS1; end
            35: begin a_src=A_QUOTIENT; b_src=B_SAMPLE_X; end
            38: begin a_src=A_QUOTIENT; b_src=B_SAMPLE_Y; end
            39: begin a_src=A_ONE; b_src=B_EIGHT; end
            40: begin a_src=A_DEPTH; z_sel=0; b_src=B_256; end
            42: begin a_src=A_QUOTIENT; b_src=sign_y?B_MINUS1:B_PLUS1; end
            default: begin end
            endcase
            if(!hold) case(step)
            0: issue_op=OP_SETNEG;
            1: issue_op=OP_SUB;
            2: issue_op=OP_ADD;
            3: begin issue_op=OP_ADD; issue_action=AC_AREA; end
            4: begin issue_op=OP_ABS; issue_action=AC_AREA_LOAD; end
            6,7,8,9,10,11,12,13,14,15,16,17: case(in_edge[1:0])
                0: issue_op=op_c_first;
                1: begin issue_op=op_c_next; issue_action=AC_WORD_LO; issue_word=7'd28+{3'd0,e_sel,2'd0}; end
                2: issue_op=op_e_next;
                default: begin issue_op=op_e_next; issue_action=AC_TEST; end
                endcase
            18,19,20,21,22,23: begin
                issue_op=negative?OP_SETNEG:OP_SET;
                issue_action=AC_WORD_LO; issue_word=7'd26+{3'd0,e_sel,2'd0}+{6'd0,step[0]};
            end
            24,28: issue_op=OP_SET;
            25,29: issue_op=OP_ADD;
            26: begin issue_op=OP_ADD; issue_action=AC_SIGN_X; end
            30: begin issue_op=OP_ADD; issue_action=AC_SIGN_Y; end
            27: begin issue_op=OP_ABS; issue_action=AC_DIV_X; end
            31: begin issue_op=OP_ABS; issue_action=AC_HOLD_NY; end
            33: begin issue_op=OP_SET; issue_action=AC_WORD_LO; issue_word=7'd40; end
            35: issue_op=sign_x?OP_SETNEG:OP_SET;
            38: issue_op=sign_y?OP_SUB:OP_ADD;
            39: issue_op=OP_ADD;
            40: begin issue_op=OP_SHIFT_ADD; issue_action=AC_WORD_LO; issue_word=7'd38; end
            42: begin issue_op=OP_SET; issue_action=AC_WORD_LO; issue_word=7'd42; end
            43: begin issue_action=AC_WORD_HI; issue_word=7'd43; end
            default: begin end
            endcase
            // Upper words follow their low words on the next clock.
            if(step==34) begin issue_action=AC_WORD_HI; issue_word=7'd41; end
            if(step==41) begin issue_action=AC_WORD_HI; issue_word=7'd39; end
        end
    end

    // ------------------------------------------------- accumulator
    reg [63:0] add_x, add_y;
    reg add_carry;
    wire [63:0] product64={{4{product[59]}},product};
    always @* begin
        add_x=acc; add_y=product64; add_carry=0;
        case(op_q2)
        OP_SET: add_x=0;
        OP_SETNEG: begin add_x=0; add_y=~product64; add_carry=1; end
        OP_SUB: begin add_y=~product64; add_carry=1; end
        OP_ABS: begin add_x=0; add_y=acc^{64{acc[63]}}; add_carry=acc[63]; end
        OP_SHIFT_ADD: add_x={{4{acc[63]}},acc[63:4]};
        default: begin end
        endcase
    end
    wire [63:0] add_sum=add_x+add_y+{63'd0,add_carry};

    // ------------------------------------------------- streamed words
    // Words 0..19 and the three inclusive words; A, B, C, seed and gradient
    // words come from the accumulator.
    localparam [4:0] STREAM_WORDS=23;
    reg [4:0] out_count;
    reg [6:0] out_index;
    reg [31:0] out_data;
    always @* begin
        out_index={2'd0,out_count};
        out_data=0;
        case(out_count)
        0: out_data={13'd0,flags,8'h01,8'h00};
        1: out_data={22'd0,depth_func,8'd0};
        2: out_data={8'd0,color};
        6: out_data=32'd640;
        7: out_data=32'd480;
        8: out_data={22'd0,window_x0};
        9: out_data={23'd0,window_y0};
        10: out_data={21'd0,{1'b0,window_x_last}+11'd1};
        11: out_data={22'd0,{1'b0,window_y_last}+10'd1};
        20: begin out_index=7'd29; out_data={31'd0,edge_tie[0]}; end
        21: begin out_index=7'd33; out_data={31'd0,edge_tie[1]}; end
        22: begin out_index=7'd37; out_data={31'd0,edge_tie[2]}; end
        default: out_data=0;
        endcase
    end

    wire divide_y=state==RUN&&step==36;
    wire [47:0] divide_source=divide_y?hold_ny:acc[47:0];
    wire [43:0] rounded={1'b0,quotient}+44'd1;
    wire take={remainder,numerator[41]}>={1'b0,area};
    always @(posedge clk) begin
        mul_a_q<=mul_a; mul_b_q<=mul_b;
        product<=mul_a_q*mul_b_q;
        if(reset) begin
            state<=IDLE; done<=0; draw<=0; bad<=0; word_we<=0; step<=0; orient_done<=0;
            op_q1<=0; op_q2<=0; act_q1<=0; act_q2<=0; act_q3<=0; out_count<=0;
            div_busy<=0; div_count<=0; negative<=0;
        end else begin
            done<=0; word_we<=0;
            op_q1<=issue_op; op_q2<=op_q1;
            act_q1<=issue_action; act_q2<=act_q1; act_q3<=act_q2;
            word_q1<=issue_word; word_q2<=word_q1; word_q3<=word_q2;
            if(op_q2!=OP_NONE) acc<=add_sum;
            // Divider (|NX| from an action, |NY| from hold_ny at step 36).
            if(divide_y||(state!=IDLE&&act_q3==AC_DIV_X)) begin
                remainder<={11'd0,divide_source[47:30]}; numerator<={divide_source[29:0],12'd0};
                div_count<=0; div_busy<=1;
            end else if(div_busy) begin
                if(div_count==0) begin
                    saturate<={1'b0,remainder}>={1'b0,area};
                    div_count<=1;
                end else if(div_count<44) begin
                    remainder<=take?({remainder[27:0],numerator[41]}-area):{remainder[27:0],numerator[41]};
                    quotient<={quotient[41:0],take};
                    numerator<={numerator[40:0],1'b0};
                    div_count<=div_count+1;
                end else begin
                    // (q+1)/2 rounds the 43-bit quotient; saturate or overflow clamps.
                    quotient<=(saturate||rounded[43])?43'h3ffffffffff:rounded[43:1];
                    div_busy<=0;
                end
            end
            // Actions after a rejection are dropped (state is IDLE again).
            if(state!=IDLE) case(act_q3)
            AC_WORD_LO: begin word_we<=1; word_index<=word_q3; word_data<=acc[31:0]; end
            AC_WORD_HI: begin word_we<=1; word_index<=word_q3; word_data<=acc[63:32]; end
            AC_TEST: if(acc[63]) begin done<=1; draw<=0; bad<=0; state<=IDLE; end
            AC_AREA: begin
                negative<=acc[63];
                if(acc==0||(acc[63]&&cull)) begin done<=1; draw<=0; bad<=0; state<=IDLE; end
            end
            AC_AREA_LOAD: begin area<=acc[28:0]; orient_done<=1; end
            AC_SIGN_X: sign_x<=acc[63]^negative;
            AC_SIGN_Y: sign_y<=acc[63]^negative;
            AC_HOLD_NY: hold_ny<=acc[47:0];
            default: begin end
            endcase
            case(state)
            IDLE: if(start) begin
                window_x0<=window_x0_next[9:0]; window_x_last<=window_x1_next[9:0]-10'd1;
                window_y0<=window_y0_next[8:0]; window_y_last<=window_y1_next[8:0]-9'd1;
                step<=0; orient_done<=0; out_count<=0;
                if(|word_bad) begin done<=1; draw<=0; bad<=1; end
                else if(window_empty) begin done<=1; draw<=0; bad<=0; end
                else state<=RUN;
            end
            RUN: begin
                case(step)
                5: if(orient_done) step<=6;
                23: step<=use_depth?6'd24:6'd60;
                32,37: if(!div_busy) step<=step+1;
                43: step<=60;
                60: if(out_count==STREAM_WORDS&&act_q1==AC_NONE&&act_q2==AC_NONE&&act_q3==AC_NONE&&!word_we) begin
                    done<=1; draw<=1; bad<=0; state<=IDLE;
                end
                default: step<=step+1;
                endcase
            end
            default: state<=IDLE;
            endcase
            // Header, window and inclusive words stream to the idle renderer
            // once the orientation is final, between result words.
            if(state==RUN&&step>=6&&out_count<STREAM_WORDS&&act_q3!=AC_WORD_LO&&act_q3!=AC_WORD_HI) begin
                word_we<=1; word_index<=out_index; word_data<=out_data;
                out_count<=out_count+1;
            end
        end
    end
endmodule
