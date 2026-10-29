// Exact shared-reciprocal UV prototype. Input scale and rounding match
// reference/src/hw_reciprocal.cpp.26 enabled edges, one input per enabled edge.
// Backpressure freezes ALL stages/ROM/DSPs. CE is a common clock enable and
// must also qualify consumption at the enclosing controller.
module nano_uv_pipeline (
    input clk, input reset, input ce,
    input in_valid, output in_ready,
    input signed [63:0] in_u, input signed [63:0] in_v,
    input signed [63:0] in_q, input [5:0] in_tag,
    output out_valid, input out_ready,
    output signed [31:0] out_u, output signed [31:0] out_v,
    output [5:0] out_tag, output out_overflow, output out_invalid_q);
    localparam LATENCY = 26;
    reg [LATENCY-1:0] valid;
    wire advance = ce && (!valid[LATENCY-1] || out_ready);
    assign in_ready = advance;
    assign out_valid = valid[LATENCY-1];
    reg [5:0] tag [0:25];
    reg [25:0] invalid_q, overflow, negative_u, negative_v;
    reg [37:0] q [0:24];
    reg [42:0] mag_u [0:21], mag_v [0:21];
    reg [5:0] exponent [0:17];
    reg [32:0] normalized [0:6];
    reg signed [17:0] delta [0:2];
    reg [34:0] seed_base [2:5], reciprocal [6:14];
    reg signed [17:0] newton_error;
    reg [31:0] estimate_u [18:24], estimate_v [18:24];
    // The reciprocal estimate is within two floor-quotient units. The exact
    // residual is therefore bounded far below2^40 despite the69-bit numerator.
    reg signed [40:0] remainder_u [22:24], remainder_v [22:24];
    reg signed [31:0] result_u, result_v;
    reg result_overflow;
    assign out_u = result_u;
    assign out_v = result_v;
    assign out_tag = tag[25];
    assign out_overflow = result_overflow;
    assign out_invalid_q = invalid_q[25];

    // Explicit constant slices avoid synthesizing a96-bit variable barrel
    // shifter, mask, absolute value and adder for each rounding operation.
    function [31:0] uv_round;
        input [77:0] value; input [5:0] exp;
        reg [31:0] whole; reg guard,sticky;
        begin
            case(exp)
            29:begin whole=value[68:37];guard=value[36];sticky=|value[35:0];end
            30:begin whole=value[69:38];guard=value[37];sticky=|value[36:0];end
            31:begin whole=value[70:39];guard=value[38];sticky=|value[37:0];end
            32:begin whole=value[71:40];guard=value[39];sticky=|value[38:0];end
            33:begin whole=value[72:41];guard=value[40];sticky=|value[39:0];end
            34:begin whole=value[73:42];guard=value[41];sticky=|value[40:0];end
            35:begin whole=value[74:43];guard=value[42];sticky=|value[41:0];end
            36:begin whole=value[75:44];guard=value[43];sticky=|value[42:0];end
            default:begin whole=value[76:45];guard=value[44];sticky=|value[43:0];end
            endcase
            uv_round=whole+(guard&&(sticky||whole[0]));
        end
    endfunction

    wire bad_q = in_q < 64'sd536870912 || in_q > 64'sd137438953472;
    wire [37:0] safe_q = bad_q ? 38'd536870912 : in_q[37:0];
    // The representable UV domain needs at most43 magnitude bits. Reject
    // high bits separately instead of placing64-bit negation, limit addition
    // and comparison on one input path. Range comparison uses stage0's
    // established registers on the following edge.
    wire [42:0] abs_u = in_u[63] ? -in_u[42:0] : in_u[42:0];
    wire [42:0] abs_v = in_v[63] ? -in_v[42:0] : in_v[42:0];
    wire bad_upper_u = in_u[63] ? ((~&in_u[63:43]) || (in_u[42:0] == 0)) : (|in_u[63:43]);
    wire bad_upper_v = in_v[63] ? ((~&in_v[63:43]) || (in_v[42:0] == 0)) : (|in_v[63:43]);
    wire bad_upper = bad_upper_u || bad_upper_v;
    wire [42:0] attribute_limit = {q[0],5'b0} + 43'd1024;
    wire bad_attribute = mag_u[0] > attribute_limit || mag_v[0] > attribute_limit;
    wire [37:0] normalized_q = q[0];
    reg [5:0] exponent_in;
    reg [32:0] normalized_mantissa;
    reg normalize_round;
    reg [3:0] normalize_shift;
    reg [40:0] normalized_shifted;
    reg [33:0] normalized_in;
    reg signed [23:0] delta_raw;
    reg signed [17:0] delta_in;
    integer n;
    always @* begin
        casez(normalized_q[37:29])
            9'b1????????:exponent_in=37;
            9'b01???????:exponent_in=36;
            9'b001??????:exponent_in=35;
            9'b0001?????:exponent_in=34;
            9'b00001????:exponent_in=33;
            9'b000001???:exponent_in=32;
            9'b0000001??:exponent_in=31;
            9'b00000001?:exponent_in=30;
            default:exponent_in=29;
        endcase
        normalize_round=0;
        case(exponent_in)
            29:normalize_shift=0;30:normalize_shift=1;31:normalize_shift=2;
            32:normalize_shift=3;33:normalize_shift=4;34:normalize_shift=5;
            35:normalize_shift=6;36:normalize_shift=7;default:normalize_shift=8;
        endcase
        // The bounded 0..8 shift preserves the exact constant slices while
        // sharing their overlapping bits through one small barrel network.
        normalized_shifted={normalized_q,3'b0}>>normalize_shift;
        normalized_mantissa=normalized_shifted[32:0];
        case(exponent_in)
            33:normalize_round=normalized_q[0]&&normalized_q[1];
            34:normalize_round=normalized_q[1]&&(normalized_q[0]||normalized_q[2]);
            35:normalize_round=normalized_q[2]&&((|normalized_q[1:0])||normalized_q[3]);
            36:normalize_round=normalized_q[3]&&((|normalized_q[2:0])||normalized_q[4]);
            37:normalize_round=normalized_q[4]&&((|normalized_q[3:0])||normalized_q[5]);
            default:begin end
        endcase
        normalized_in={1'b0,normalized_mantissa}+normalize_round;
        if (normalized_in == 34'h200000000) begin
            normalized_in = normalized_in >> 1; exponent_in = exponent_in + 1'b1;
        end
    end
    always @* begin
        // OLD stage1 normalization feeds the independent stage2 residual
        // rounder, in parallel with the ROM read. Both establish their values
        // before the seed DSP captures them onedge3. Subtracting the selected
        // segment center simply flips residualbit23.
        delta_raw={~normalized[1][23],normalized[1][22:0]};
        // Extend the signed residual BEFORE adding its unsigned rounding
        // predicate. Otherwise Verilog zero-extends the17-bit negative slice
        // to the18-bit destination and reverses negative segment offsets.
        delta_in=$signed({delta_raw[23],delta_raw[23:7]})+
            $signed({17'b0,((delta_raw[6:0]>7'd64)||
                ((delta_raw[6:0]==7'd64)&&delta_raw[7]))});
    end

    wire [15:0] rom0, rom1, rom2, rom3;
    nano_uv_seed_rom rom(clk, reset, advance && valid[1], normalized[1][31:24],
        rom0, rom1, rom2, rom3);
    wire signed [33:0] seed_product;
    nano_uv_mul #(.AW(16),.BW(18),.SIGNED_B(1)) seed_mul(
        clk,advance,reset,rom3,delta[2],seed_product);
    wire signed [67:0] normalized_product;
    nano_uv_mul #(.AW(33),.BW(35)) nr_mul(
        clk,advance,reset,normalized[6],reciprocal[6],normalized_product);
    wire signed [52:0] newton_product;
    nano_uv_mul #(.AW(35),.BW(18),.SIGNED_B(1)) correction_mul(
        clk,advance,reset,reciprocal[10],newton_error,newton_product);
    wire signed [77:0] u_product, v_product;
    nano_uv_mul #(.AW(43),.BW(35)) uv_mul_u(
        clk,advance,reset,mag_u[14],reciprocal[14],u_product);
    nano_uv_mul #(.AW(43),.BW(35)) uv_mul_v(
        clk,advance,reset,mag_v[14],reciprocal[14],v_product);
    wire signed [69:0] correction_u_product, correction_v_product;
    nano_uv_mul #(.AW(38),.BW(32)) exact_mul_u(
        clk,advance,reset,q[18],estimate_u[18],correction_u_product);
    nano_uv_mul #(.AW(38),.BW(32)) exact_mul_v(
        clk,advance,reset,q[18],estimate_v[18],correction_v_product);
    wire signed [26:0] rounded_seed = $signed(seed_product[33:7])+
        (seed_product[6]&&((|seed_product[5:0])||seed_product[7]));
    wire signed [18:0] rounded_newton = $signed(newton_product[52:34])+
        (newton_product[33]&&((|newton_product[32:0])||newton_product[34]));
    wire [17:0] normalized_rounded_low = normalized_product[49:32]+
        (normalized_product[31]&&((|normalized_product[30:0])||normalized_product[32]));
    // Exact correction established0<=r<q<=2^37. Precompute both rounded
    // candidates and both signs independently of the remainder comparison;
    // do not put compare -> increment -> negate -> overflow in one path.
    wire [38:0] q24 = {1'b0,q[24]};
    wire round_u = ({remainder_u[24][37:0],1'b0} > q24) ||
        (({remainder_u[24][37:0],1'b0} == q24) && estimate_u[24][0]);
    wire round_v = ({remainder_v[24][37:0],1'b0} > q24) ||
        (({remainder_v[24][37:0],1'b0} == q24) && estimate_v[24][0]);
    wire [31:0] positive_u = round_u ? estimate_u[24]+32'd1 : estimate_u[24];
    wire [31:0] positive_v = round_v ? estimate_v[24]+32'd1 : estimate_v[24];
    wire [31:0] negative_result_u = round_u ? ~estimate_u[24] : -estimate_u[24];
    wire [31:0] negative_result_v = round_v ? ~estimate_v[24] : -estimate_v[24];
    wire over_u = negative_u[24] ?
        ((estimate_u[24][31] && (|estimate_u[24][30:0])) ||
         ((estimate_u[24] == 32'h80000000) && round_u)) :
        (estimate_u[24][31] || ((estimate_u[24] == 32'h7fffffff) && round_u));
    wire over_v = negative_v[24] ?
        ((estimate_v[24][31] && (|estimate_v[24][30:0])) ||
         ((estimate_v[24] == 32'h80000000) && round_v)) :
        (estimate_v[24][31] || ((estimate_v[24] == 32'h7fffffff) && round_v));

    // Each correction stage shares -q across U and V, then uses one
    // selected signed addend and one +/-1 estimate adder per coordinate.
    wire signed [40:0] negative_q22=-$signed({3'b0,q[22]});
    wire signed [40:0] negative_q23=-$signed({3'b0,q[23]});
    wire signed [40:0] negative_q [22:23];
    assign negative_q[22]=negative_q22;
    assign negative_q[23]=negative_q23;
    always @(posedge clk) begin
        if (reset) begin
            valid <= 0; invalid_q <= 0; overflow <= 0;
            negative_u <= 0; negative_v <= 0;
            result_u <= 0; result_v <= 0; result_overflow <= 0;
        end else if (advance) begin
            valid <= {valid[24:0],in_valid};
            invalid_q <= {invalid_q[24:0],bad_q};
            overflow <= {overflow[24:0],bad_upper};
            overflow[1] <= overflow[0] || bad_attribute;
            negative_u <= {negative_u[24:0],in_u[63]};
            negative_v <= {negative_v[24:0],in_v[63]};
            tag[0] <= in_tag;
            for (n=1;n<26;n=n+1) tag[n] <= tag[n-1];
            q[0] <= safe_q;
            for (n=1;n<25;n=n+1) q[n] <= q[n-1];
            mag_u[0] <= bad_q || bad_upper ? 0 : abs_u;
            mag_v[0] <= bad_q || bad_upper ? 0 : abs_v;
            mag_u[1] <= invalid_q[0] || overflow[0] || bad_attribute ? 0 : mag_u[0];
            mag_v[1] <= invalid_q[0] || overflow[0] || bad_attribute ? 0 : mag_v[0];
            for (n=2;n<22;n=n+1) begin mag_u[n]<=mag_u[n-1]; mag_v[n]<=mag_v[n-1]; end
            exponent[1] <= exponent_in;
            for(n=2;n<18;n=n+1) exponent[n]<=exponent[n-1];
            normalized[1] <= normalized_in[32:0];
            for(n=2;n<7;n=n+1) normalized[n]<=normalized[n-1];
            delta[2]<=delta_in;
            seed_base[3] <= {rom2[2:0],rom1,rom0};
            for(n=4;n<6;n=n+1) seed_base[n]<=seed_base[n-1];
            reciprocal[6] <= $signed({1'b0,seed_base[5]})-rounded_seed;
            for(n=7;n<15;n=n+1) reciprocal[n]<=reciprocal[n-1];
            newton_error <= -normalized_rounded_low;
            reciprocal[14] <= $signed({1'b0,reciprocal[13]})+rounded_newton;
            estimate_u[18] <= uv_round(u_product,exponent[17]);
            estimate_v[18] <= uv_round(v_product,exponent[17]);
            for(n=19;n<25;n=n+1) begin
                estimate_u[n]<=estimate_u[n-1];estimate_v[n]<=estimate_v[n-1];
            end
            remainder_u[22] <= $signed({1'b0,mag_u[21],26'b0})-$signed({1'b0,correction_u_product});
            remainder_v[22] <= $signed({1'b0,mag_v[21],26'b0})-$signed({1'b0,correction_v_product});
            for(n=23;n<25;n=n+1) begin
                if(remainder_u[n-1]<0||remainder_u[n-1]>=$signed({1'b0,q[n-1]})) begin
                    estimate_u[n]<=estimate_u[n-1]+{{31{remainder_u[n-1][40]}},1'b1};
                    remainder_u[n]<=remainder_u[n-1]+
                        (remainder_u[n-1][40]?$signed({3'b0,q[n-1]}):negative_q[n-1]);
                end else remainder_u[n]<=remainder_u[n-1];
                if(remainder_v[n-1]<0||remainder_v[n-1]>=$signed({1'b0,q[n-1]})) begin
                    estimate_v[n]<=estimate_v[n-1]+{{31{remainder_v[n-1][40]}},1'b1};
                    remainder_v[n]<=remainder_v[n-1]+
                        (remainder_v[n-1][40]?$signed({3'b0,q[n-1]}):negative_q[n-1]);
                end else remainder_v[n]<=remainder_v[n-1];
            end
            result_u <= over_u ? 0 : negative_u[24] ? negative_result_u : positive_u;
            result_v <= over_v ? 0 : negative_v[24] ? negative_result_v : positive_v;
            result_overflow <= overflow[24] || over_u || over_v;
        end
    end
endmodule


// Exact nearest-texel reconstruction, 19 enabled edges and initiation interval1.
// This private rendering path returns signed integer coordinates equal to
// floor(RNE(attribute*2^26/q)/2^16), including the full Q16 overflow boundary.
// The independent full-Q16 module above retains its exact fractional result.
//
// For normalized x=q/2^e and segment center c=(513+2*i)/512, the ROM stores
// B=RNE(2^34/c), G=RNE(2^16/c^2). With D=RNE((x_hat-c)*2^25), the seed is
// B-RNE(G*D/128). The exact Taylor relative error is ((x-c)/c)^2.
// Normalization contributes at most2^-33; all coefficient/delta/product
// rounding contributes at most516 seed units. Therefore the relative error
// is bounded by (1/512+2^-33)^2 +1032/2^34. The accepted input guard gives
// y=|attribute|*1024/q <32769, so absolute estimate error is <0.127<1/4.
// Flooring the estimate differs from floor(y) by at mostone. Two exact
// residual corrections establish k=floor(y), 0<=r<q. Initially -q<r<2q,
// q<=2^37, so signed39-bit residuals suffice; only low29 magnitude bits
// contribute to the retained residual after multiplying by2^10.
//
// The high16 bits of Q16 ties-to-even rounding have exact integer tests:
// positive: k+[r>=q-floor(q/2^17)]; negative: -k-[r>floor(q/2^17)].
// Testing the signed16 range of that integer gives exactly the original
// signed-Q16 overflow flag, including equality at the negative endpoint.
// No approximate texel addresses or relaxed numeric inputs are introduced.
module nano_uv_texture_pipeline (
    input clk, input reset, input ce,
    input in_valid, output in_ready,
    input signed [63:0] in_u, input signed [63:0] in_v,
    input signed [63:0] in_q, input [5:0] in_tag,
    output out_valid, input out_ready,
    output signed [31:0] out_u, output signed [31:0] out_v,
    output [5:0] out_tag, output out_overflow, output out_invalid_q);
    localparam LATENCY = 19;
    reg [LATENCY-1:0] valid;
    wire advance = ce && (!valid[LATENCY-1] || out_ready);
    assign in_ready = advance;
    assign out_valid = valid[LATENCY-1];
    reg [5:0] tag [0:18];
    reg [18:0] invalid_q, overflow, negative_u, negative_v;
    reg [37:0] q [0:16];
    reg [3:0] delay_pointer;
    wire [47:0] delayed_context11;
    wire [91:0] delayed_magnitude7;
    wire [57:0] delayed_magnitude14;
    wire [33:0] delayed_estimate15;
    reg [42:0] mag_u [0:1], mag_v [0:1];
    reg [5:0] exponent [0:10];
    reg [32:0] normalized [0:2];
    reg signed [17:0] delta [0:3];
    reg [34:0] seed_base [3:6], reciprocal;
    reg [16:0] estimate_u [11:17], estimate_v [11:17];
    // A bounded seed estimate followed by exact residual corrections.
    reg signed [38:0] remainder_u [15:17], remainder_v [15:17];
    reg signed [31:0] result_u, result_v;
    reg result_overflow;
    reg [5:0] rounded_exponent;
    assign out_u = result_u;
    assign out_v = result_v;
    assign out_tag = tag[18];
    assign out_overflow = result_overflow;
    assign out_invalid_q = invalid_q[18];

    function [16:0] integer_estimate;
        input [77:0] value; input [5:0] exp;
        begin
            case(exp)
            29:integer_estimate=value[69:53];
            30:integer_estimate=value[70:54];
            31:integer_estimate=value[71:55];
            32:integer_estimate=value[72:56];
            33:integer_estimate=value[73:57];
            34:integer_estimate=value[74:58];
            35:integer_estimate=value[75:59];
            36:integer_estimate=value[76:60];
            default:integer_estimate=value[77:61];
            endcase
        end
    endfunction

    wire bad_q = in_q < 64'sd536870912 || in_q > 64'sd137438953472;
    wire [37:0] safe_q = bad_q ? 38'd536870912 : in_q[37:0];
    // The representable UV domain needs at most43 magnitude bits. Reject
    // high bits separately instead of placing64-bit negation, limit addition
    // and comparison on one input path. Range comparison uses stage0's
    // established registers on the following edge.
    wire [42:0] abs_u = in_u[63] ? -in_u[42:0] : in_u[42:0];
    wire [42:0] abs_v = in_v[63] ? -in_v[42:0] : in_v[42:0];
    wire bad_upper_u = in_u[63] ? ((~&in_u[63:43]) || (in_u[42:0] == 0)) : (|in_u[63:43]);
    wire bad_upper_v = in_v[63] ? ((~&in_v[63:43]) || (in_v[42:0] == 0)) : (|in_v[63:43]);
    wire bad_upper = bad_upper_u || bad_upper_v;
    // q*32+1024 is (q+32)<<5. Capture that independent addition with
    // the stage0 token so stage1 only compares its retained magnitude/limit.
    reg [37:0] attribute_limit_hi;
    wire [42:0] attribute_limit = {attribute_limit_hi,5'b0};
    wire bad_attribute = mag_u[0] > attribute_limit || mag_v[0] > attribute_limit;
    // Four independent native-RAM schedules, eight physical BSRAM blocks.
    // Bad-attribute validation occurs at edge1, so its combinational predicate
    // accompanies stage0 metadata written at that same edge.
    wire [47:0] context_input={overflow[0]||bad_attribute,invalid_q[0],
        negative_v[0],negative_u[0],tag[0],q[0]};
    nano_uv_bsram_delay #(.WIDTH(48),.DELAY(11)) context_delay(
        clk,reset,advance,delay_pointer,context_input,delayed_context11);
    nano_uv_bsram_delay #(.WIDTH(92),.DELAY(6)) magnitude_first(
        clk,reset,advance,delay_pointer,{rounded_exponent,mag_v[1],mag_u[1]},delayed_magnitude7);
    nano_uv_bsram_delay #(.WIDTH(58),.DELAY(7)) magnitude_second(
        clk,reset,advance,delay_pointer,{delayed_magnitude7[71:43],delayed_magnitude7[28:0]},delayed_magnitude14);
    nano_uv_bsram_delay #(.WIDTH(34),.DELAY(4),.BANK_WIDTH(36)) estimate_delay(
        clk,reset,advance,delay_pointer,{estimate_v[11],estimate_u[11]},delayed_estimate15);
    wire [16:0] previous_estimate_u [15:16],previous_estimate_v [15:16];
    assign previous_estimate_u[15]=delayed_estimate15[16:0];
    assign previous_estimate_v[15]=delayed_estimate15[33:17];
    assign previous_estimate_u[16]=estimate_u[16];
    assign previous_estimate_v[16]=estimate_v[16];
    wire [37:0] normalized_q = q[0];
    reg [5:0] exponent_in;
    reg [32:0] normalized_mantissa;
    reg normalize_round;
    reg [3:0] normalize_shift;
    reg [40:0] normalized_shifted;
    reg [33:0] normalized_in;
    reg signed [23:0] delta_raw;
    reg signed [17:0] delta_in;
    integer n;
    always @* begin
        casez(normalized_q[37:29])
            9'b1????????:exponent_in=37;
            9'b01???????:exponent_in=36;
            9'b001??????:exponent_in=35;
            9'b0001?????:exponent_in=34;
            9'b00001????:exponent_in=33;
            9'b000001???:exponent_in=32;
            9'b0000001??:exponent_in=31;
            9'b00000001?:exponent_in=30;
            default:exponent_in=29;
        endcase
        normalize_round=0;
        case(exponent_in)
            29:normalize_shift=0;30:normalize_shift=1;31:normalize_shift=2;
            32:normalize_shift=3;33:normalize_shift=4;34:normalize_shift=5;
            35:normalize_shift=6;36:normalize_shift=7;default:normalize_shift=8;
        endcase
        // The bounded 0..8 shift preserves the exact constant slices while
        // sharing their overlapping bits through one small barrel network.
        normalized_shifted={normalized_q,3'b0}>>normalize_shift;
        normalized_mantissa=normalized_shifted[32:0];
        case(exponent_in)
            33:normalize_round=normalized_q[0]&&normalized_q[1];
            34:normalize_round=normalized_q[1]&&(normalized_q[0]||normalized_q[2]);
            35:normalize_round=normalized_q[2]&&((|normalized_q[1:0])||normalized_q[3]);
            36:normalize_round=normalized_q[3]&&((|normalized_q[2:0])||normalized_q[4]);
            37:normalize_round=normalized_q[4]&&((|normalized_q[3:0])||normalized_q[5]);
            default:begin end
        endcase
    end
    reg [32:0] normalize_mantissa1;
    reg normalize_round1;
    always @* begin
        normalized_in={1'b0,normalize_mantissa1}+normalize_round1;
        rounded_exponent=exponent[1];
        if ((&normalize_mantissa1)&&normalize_round1) begin
            normalized_in=34'h100000000;
            rounded_exponent=exponent[1]+1'b1;
        end
    end
    always @* begin
        // OLD stage2 normalization feeds the independent stage3 residual
        // rounder, in parallel with the ROM read. Both establish their values
        // before the seed DSP captures them onedge4. Subtracting the selected
        // segment center simply flips residualbit23.
        delta_raw={~normalized[2][23],normalized[2][22:0]};
        // Extend the signed residual BEFORE adding its unsigned rounding
        // predicate. Otherwise Verilog zero-extends the17-bit negative slice
        // to the18-bit destination and reverses negative segment offsets.
        delta_in=$signed({delta_raw[23],delta_raw[23:7]})+
            $signed({17'b0,((delta_raw[6:0]>7'd64)||
                ((delta_raw[6:0]==7'd64)&&delta_raw[7]))});
    end

    wire [15:0] rom0, rom1, rom2, rom3;
    nano_uv_seed_rom rom(clk, reset, advance && valid[2], normalized[2][31:24],
        rom0, rom1, rom2, rom3);
    wire signed [33:0] seed_product;
    nano_uv_mul #(.AW(16),.BW(18),.SIGNED_B(1)) seed_mul(
        clk,advance,reset,rom3,delta[3],seed_product);
    wire signed [77:0] u_product,v_product;
    nano_uv_mul #(.AW(43),.BW(35)) uv_mul_u(
        clk,advance,reset,delayed_magnitude7[42:0],reciprocal,u_product);
    nano_uv_mul #(.AW(43),.BW(35)) uv_mul_v(
        clk,advance,reset,delayed_magnitude7[85:43],reciprocal,v_product);
    wire signed [54:0] correction_u_product,correction_v_product;
    nano_uv_mul #(.AW(38),.BW(17)) exact_mul_u(
        clk,advance,reset,delayed_context11[37:0],estimate_u[11],correction_u_product);
    nano_uv_mul #(.AW(38),.BW(17)) exact_mul_v(
        clk,advance,reset,delayed_context11[37:0],estimate_v[11],correction_v_product);
    wire signed [26:0] rounded_seed = $signed(seed_product[33:7])+
        (seed_product[6]&&((|seed_product[5:0])||seed_product[7]));
    reg [20:0] epsilon;
    // Replace the final full-q delay with its two rounding thresholds.
    // They are captured from stage16 alongside residual stage17, preserving
    // the enabled-edge latency while keeping subtraction out of the round path.
    reg [37:0] positive_threshold;
    wire [37:0] next_positive_threshold=q[16]-(q[16]>>17);
    wire round_u=negative_u[17]?(remainder_u[17]>$signed({1'b0,epsilon})):
        (remainder_u[17]>=$signed({1'b0,positive_threshold}));
    wire round_v=negative_v[17]?(remainder_v[17]>$signed({1'b0,epsilon})):
        (remainder_v[17]>=$signed({1'b0,positive_threshold}));
    // Exact signed16 range predicates and both round candidates are formed
    // independently of the remainder comparison. Round selects only the final
    // candidate; it does not feed an increment/negate/comparison carry chain.
    // Negative candidates are -k and ~(k), exactly -(k+1). The range tests
    // account separately for the allowed negative endpoint -32768.
    wire [16:0] positive_u=round_u?estimate_u[17]+17'd1:estimate_u[17];
    wire [16:0] positive_v=round_v?estimate_v[17]+17'd1:estimate_v[17];
    wire [16:0] negative_result_u=round_u?~estimate_u[17]:-estimate_u[17];
    wire [16:0] negative_result_v=round_v?~estimate_v[17]:-estimate_v[17];
    wire over_u=negative_u[17]?
        (estimate_u[17][16]||(estimate_u[17][15]&&((|estimate_u[17][14:0])||round_u))):
        ((|estimate_u[17][16:15])||((&estimate_u[17][14:0])&&round_u));
    wire over_v=negative_v[17]?
        (estimate_v[17][16]||(estimate_v[17][15]&&((|estimate_v[17][14:0])||round_v))):
        ((|estimate_v[17][16:15])||((&estimate_v[17][14:0])&&round_v));
    wire signed [16:0] selected_u=negative_u[17]?$signed(negative_result_u):$signed(positive_u);
    wire signed [16:0] selected_v=negative_v[17]?$signed(negative_result_v):$signed(positive_v);
    wire signed [31:0] signed_u={{15{selected_u[16]}},selected_u};
    wire signed [31:0] signed_v={{15{selected_v[16]}},selected_v};
    wire signed [38:0] negative_q [15:16];
    assign negative_q[15]=-$signed({1'b0,q[15]});
    assign negative_q[16]=-$signed({1'b0,q[16]});
    always @(posedge clk) begin
        if (reset) begin
            valid <= 0; invalid_q <= 0; overflow <= 0; delay_pointer <= 0;
            negative_u <= 0; negative_v <= 0;
            result_u <= 0; result_v <= 0; result_overflow <= 0;
        end else if (advance) begin
            valid <= {valid[17:0],in_valid};
            delay_pointer <= delay_pointer+1'b1;
            invalid_q[0] <= bad_q; overflow[0] <= bad_upper;
            negative_u[0] <= in_u[63]; negative_v[0] <= in_v[63];
            tag[0] <= in_tag; q[0] <= safe_q;
            attribute_limit_hi <= safe_q+38'd32;
            q[12] <= delayed_context11[37:0];
            positive_threshold <= next_positive_threshold;
            epsilon <= q[16][37:17];
            tag[12] <= delayed_context11[43:38];
            negative_u[12] <= delayed_context11[44];
            negative_v[12] <= delayed_context11[45];
            invalid_q[12] <= delayed_context11[46];
            overflow[12] <= delayed_context11[47];
            for(n=13;n<17;n=n+1)q[n] <= q[n-1];
            for(n=13;n<19;n=n+1)begin
                tag[n] <= tag[n-1];
                negative_u[n] <= negative_u[n-1];negative_v[n] <= negative_v[n-1];
                invalid_q[n] <= invalid_q[n-1];overflow[n] <= overflow[n-1];
            end
            mag_u[0] <= bad_q || bad_upper ? 0 : abs_u;
            mag_v[0] <= bad_q || bad_upper ? 0 : abs_v;
            mag_u[1] <= invalid_q[0] || overflow[0] || bad_attribute ? 0 : mag_u[0];
            mag_v[1] <= invalid_q[0] || overflow[0] || bad_attribute ? 0 : mag_v[0];
            exponent[1] <= exponent_in;
            exponent[8]<=delayed_magnitude7[91:86];
            for(n=9;n<11;n=n+1) exponent[n]<=exponent[n-1];
            normalized[2] <= normalized_in[32:0];
            normalize_mantissa1 <= normalized_mantissa; normalize_round1 <= normalize_round;
            delta[3]<=delta_in;
            seed_base[4] <= {rom2[2:0],rom1,rom0};
            for(n=5;n<7;n=n+1) seed_base[n]<=seed_base[n-1];
            reciprocal <= $signed({1'b0,seed_base[6]})-rounded_seed;
            estimate_u[11] <= integer_estimate(u_product,exponent[10]);
            estimate_v[11] <= integer_estimate(v_product,exponent[10]);
            remainder_u[15] <= $signed({1'b0,delayed_magnitude14[28:0],10'b0})-$signed({1'b0,correction_u_product});
            remainder_v[15] <= $signed({1'b0,delayed_magnitude14[57:29],10'b0})-$signed({1'b0,correction_v_product});
            for(n=16;n<18;n=n+1) begin
                estimate_u[n]<=previous_estimate_u[n-1];
                estimate_v[n]<=previous_estimate_v[n-1];
                if(remainder_u[n-1]<0||remainder_u[n-1]>=$signed({1'b0,q[n-1]})) begin
                    estimate_u[n]<=previous_estimate_u[n-1]+{{16{remainder_u[n-1][38]}},1'b1};
                    remainder_u[n]<=remainder_u[n-1]+
                        (remainder_u[n-1][38]?$signed({1'b0,q[n-1]}):negative_q[n-1]);
                end else remainder_u[n]<=remainder_u[n-1];
                if(remainder_v[n-1]<0||remainder_v[n-1]>=$signed({1'b0,q[n-1]})) begin
                    estimate_v[n]<=previous_estimate_v[n-1]+{{16{remainder_v[n-1][38]}},1'b1};
                    remainder_v[n]<=remainder_v[n-1]+
                        (remainder_v[n-1][38]?$signed({1'b0,q[n-1]}):negative_q[n-1]);
                end else remainder_v[n]<=remainder_v[n-1];
            end
            result_u <= over_u ? 0 : signed_u;
            result_v <= over_v ? 0 : signed_v;
            result_overflow <= overflow[17] || over_u || over_v;
        end
    end
endmodule


// Native synchronous RAM output supplies the final retained delay edge.
// The shared ring pointer advances with the entire UV pipeline. Distinct
// addresses for DELAY2..16 avoid the device's undefined read/write collision.
module nano_uv_bsram_delay #(parameter WIDTH=32,parameter DELAY=16,parameter BANK_WIDTH=32)(
    input clk,input reset,input advance,input [3:0] pointer,
    input [WIDTH-1:0] data,output wire [WIDTH-1:0] delayed);
    localparam BANKS=(WIDTH+BANK_WIDTH-1)/BANK_WIDTH;
    wire [3:0] read_address=pointer-4'(DELAY-1);
    wire [BANKS*BANK_WIDTH-1:0] padded={{(BANKS*BANK_WIDTH-WIDTH){1'b0}},data};
    wire [BANKS*BANK_WIDTH-1:0] read_data;
    genvar bank;
    generate for(bank=0;bank<BANKS;bank=bank+1)begin:banks
        if(BANK_WIDTH==36)begin:wide
            nano_sdp36 memory(.wclk(clk),.wce(1'b1),.we(advance&&!reset),
                .waddr({5'd0,pointer}),.wdata(padded[BANK_WIDTH*bank+:BANK_WIDTH]),
                .rclk(clk),.rce(1'b1),.re(advance),.raddr({5'd0,read_address}),
                .reset_output(reset),.rdata(read_data[BANK_WIDTH*bank+:BANK_WIDTH]));
        end else begin:regular
            nano_sdp32 memory(.wclk(clk),.wce(1'b1),.we(advance&&!reset),
                .waddr({5'd0,pointer}),.wdata(padded[BANK_WIDTH*bank+:BANK_WIDTH]),
                .rclk(clk),.rce(1'b1),.re(advance),.raddr({5'd0,read_address}),
                .reset_output(reset),.rdata(read_data[BANK_WIDTH*bank+:BANK_WIDTH]));
        end
    end endgenerate
    assign delayed=read_data[WIDTH-1:0];
`ifndef SYNTHESIS
    initial if(DELAY<2||DELAY>16||!(BANK_WIDTH==32||BANK_WIDTH==36))
        $fatal(1,"unsupported native UV delay configuration");
`endif
endmodule
