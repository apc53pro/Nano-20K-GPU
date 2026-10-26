// Registered GW2AR multiplier. The default signed configuration matches
// Mult18x18 in the C++ model; static sign parameters also select the native
// unsigned and mixed modes for full-width limbs in the RTL UV datapath.
module nano_mult18 #(parameter SIGNED_A = 1, parameter SIGNED_B = 1)(
    input clk, input ce, input reset,
    input signed [17:0] a, input signed [17:0] b,
    output signed [35:0] product);
`ifdef SYNTHESIS
    MULT18X18 #(.AREG(1'b1), .BREG(1'b1), .PIPE_REG(1'b1),
        .OUT_REG(1'b1), .ASIGN_REG(1'b0), .BSIGN_REG(1'b0),
        .SOA_REG(1'b0), .MULT_RESET_MODE("SYNC")) primitive_multiplier (
        .A(a), .B(b), .SIA(18'b0), .SIB(18'b0), .ASIGN(SIGNED_A != 0),
        .BSIGN(SIGNED_B != 0), .ASEL(1'b0), .BSEL(1'b0), .CE(ce),
        .CLK(clk), .RESET(reset), .DOUT(product), .SOA(), .SOB());
`else
    reg signed [17:0] ar, br;
    reg signed [35:0] pipeline_product, output_product;
    wire signed [18:0] extended_a = {SIGNED_A ? ar[17] : 1'b0,ar};
    wire signed [18:0] extended_b = {SIGNED_B ? br[17] : 1'b0,br};
    always @(posedge clk) begin
        if (reset) begin ar <= 0; br <= 0; pipeline_product <= 0; output_product <= 0; end
        else if (ce) begin
            ar <= a; br <= b;
            pipeline_product <= extended_a * extended_b;
            output_product <= pipeline_product;
        end
    end
    assign product = output_product;
`endif
endmodule

// Positive operands use all 18 bits of an unsigned native input. Signed-B
// mode is used for the 18-bit seed/Newton residual and uses one signed limb.
// The balanced shifted sum is combinational; its physical timing is measured
// by synthesis/place-and-route rather than assigned a fictitious wide DSP.
module nano_uv_mul #(parameter AW = 33, parameter BW = 35,
    parameter SIGNED_B = 0, parameter PW = AW + BW)(
    input clk, input ce, input reset,
    input [AW-1:0] a, input [BW-1:0] b,
    output signed [PW-1:0] product);
    localparam NA = (AW + 17) / 18;
    localparam NB = SIGNED_B ? 1 : (BW + 17) / 18;
    localparam NP = NA * NB;
    localparam NT = (NP <= 1) ? 1 : (NP <= 2) ? 2 :
        (NP <= 4) ? 4 : (NP <= 8) ? 8 : 16;
    wire [18*NA-1:0] padded_a = {{(18*NA-AW){1'b0}}, a};
    wire [18*NB-1:0] padded_b;
    generate if (!SIGNED_B) begin
        assign padded_b = {{(18*NB-BW){1'b0}}, b};
    end else begin
        assign padded_b = 0;
    end endgenerate
    wire signed [PW-1:0] tree [1:2*NT-1];
    genvar i, j, k;
    generate for (i = 0; i < NA; i = i + 1) begin: apart
        for (j = 0; j < NB; j = j + 1) begin: bpart
            localparam A_BITS = (AW - 18*i < 18) ? AW - 18*i : 18;
            localparam B_BITS = SIGNED_B ? 18 : ((BW - 18*j < 18) ? BW - 18*j : 18);
            localparam PRODUCT_BITS = A_BITS + B_BITS;
            wire [17:0] ai = padded_a[18*i +: 18];
            wire signed [17:0] bj;
            if (SIGNED_B) assign bj = b;
            else assign bj = padded_b[18*j +: 18];
            wire signed [35:0] p;
            nano_mult18 #(.SIGNED_A(0),.SIGNED_B(SIGNED_B)) m(clk, ce, reset, ai, bj, p);
            // Native DOUT always has 36 bits. For these bounded limbs its
            // upper bits are redundant sign/zero extension. The primitive is
            // a synthesis black box: expose the proven product width here
            // rather than building wide adders for fictitious upper bits.
            wire signed [PW-1:0] expanded;
            if (SIGNED_B) assign expanded = $signed(p[PRODUCT_BITS-1:0]);
            else assign expanded = {{(PW-PRODUCT_BITS){1'b0}},p[PRODUCT_BITS-1:0]};
            assign tree[NT+i*NB+j] = expanded <<< (18*(i+j));
        end
    end
    for (k = NP; k < NT; k = k + 1) begin: padding
        assign tree[NT+k] = 0;
    end
    for (k = 1; k < NT; k = k + 1) begin: sum
        assign tree[k] = tree[2*k] + tree[2*k+1];
    end endgenerate
    assign product = tree[1];
endmodule
