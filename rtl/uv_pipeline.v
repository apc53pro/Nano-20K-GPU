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
            29:normalized_mantissa={normalized_q[29:0],3'b0};
            30:normalized_mantissa={normalized_q[30:0],2'b0};
            31:normalized_mantissa={normalized_q[31:0],1'b0};
            32:normalized_mantissa=normalized_q[32:0];
            33:begin normalized_mantissa=normalized_q[33:1];normalize_round=normalized_q[0]&&normalized_q[1];end
            34:begin normalized_mantissa=normalized_q[34:2];normalize_round=normalized_q[1]&&(normalized_q[0]||normalized_q[2]);end
            35:begin normalized_mantissa=normalized_q[35:3];normalize_round=normalized_q[2]&&((|normalized_q[1:0])||normalized_q[3]);end
            36:begin normalized_mantissa=normalized_q[36:4];normalize_round=normalized_q[3]&&((|normalized_q[2:0])||normalized_q[4]);end
            default:begin normalized_mantissa=normalized_q[37:5];normalize_round=normalized_q[4]&&((|normalized_q[3:0])||normalized_q[5]);end
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
                if(remainder_u[n-1]<0) begin
                    estimate_u[n]<=estimate_u[n-1]-1'b1;
                    remainder_u[n]<=remainder_u[n-1]+$signed({1'b0,q[n-1]});
                end else if(remainder_u[n-1]>=$signed({1'b0,q[n-1]})) begin
                    estimate_u[n]<=estimate_u[n-1]+1'b1;
                    remainder_u[n]<=remainder_u[n-1]-$signed({1'b0,q[n-1]});
                end else remainder_u[n]<=remainder_u[n-1];
                if(remainder_v[n-1]<0) begin
                    estimate_v[n]<=estimate_v[n-1]-1'b1;
                    remainder_v[n]<=remainder_v[n-1]+$signed({1'b0,q[n-1]});
                end else if(remainder_v[n-1]>=$signed({1'b0,q[n-1]})) begin
                    estimate_v[n]<=estimate_v[n-1]+1'b1;
                    remainder_v[n]<=remainder_v[n-1]-$signed({1'b0,q[n-1]});
                end else remainder_v[n]<=remainder_v[n-1];
            end
            result_u <= over_u ? 0 : negative_u[24] ? negative_result_u : positive_u;
            result_v <= over_v ? 0 : negative_v[24] ? negative_result_v : positive_v;
            result_overflow <= overflow[24] || over_u || over_v;
        end
    end
endmodule
