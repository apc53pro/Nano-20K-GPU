// Standard DVI-compatible TMDS video encoding and Gowin 10:1 DDR output.
// Clock generation and exact board differential-pin constraints belong to the
// production board wrapper. serial_clk must be five times pixel_clk.
module nano_tmds_encoder (
    input wire clk, input wire reset, input wire ce,
    input wire [7:0] data, input wire de, input wire c0, input wire c1,
    output reg [9:0] symbol
);
    reg signed [5:0] disparity;
    function [3:0] count_ones;
        input [7:0] value;
        integer bit_index;
        begin
            count_ones = 0;
            for (bit_index = 0; bit_index < 8; bit_index = bit_index + 1)
                count_ones = count_ones + {3'b0,value[bit_index]};
        end
    endfunction
    wire [3:0] data_ones = count_ones(data);
    wire use_xnor = data_ones > 4 || (data_ones == 4 && !data[0]);
    wire [8:0] transition;
    assign transition[0] = data[0];
    genvar bit_index;
    generate for (bit_index = 1; bit_index < 8; bit_index = bit_index + 1) begin: transitions
        assign transition[bit_index] = transition[bit_index-1] ^ data[bit_index] ^ use_xnor;
    end endgenerate
    assign transition[8] = !use_xnor;
    wire [3:0] transition_ones = count_ones(transition[7:0]);
    wire signed [5:0] balance = $signed({1'b0,transition_ones,1'b0}) - 6'sd8;
    always @(posedge clk) begin
        if (reset) begin disparity <= 0; symbol <= 10'b1101010100; end
        else if (ce) begin
            if (!de) begin
                disparity <= 0;
                case ({c1,c0})
                    2'b00: symbol <= 10'b1101010100;
                    2'b01: symbol <= 10'b0010101011;
                    2'b10: symbol <= 10'b0101010100;
                    2'b11: symbol <= 10'b1010101011;
                endcase
            end else if (disparity == 0 || transition_ones == 4) begin
                symbol <= {~transition[8],transition[8],
                    transition[8] ? transition[7:0] : ~transition[7:0]};
                disparity <= disparity + (transition[8] ? balance : -balance);
            end else if ((disparity > 0 && balance > 0) || (disparity < 0 && balance < 0)) begin
                symbol <= {1'b1,transition[8],~transition[7:0]};
                disparity <= disparity + (transition[8] ? 6'sd2 : 6'sd0) - balance;
            end else begin
                symbol <= {1'b0,transition[8],transition[7:0]};
                disparity <= disparity - (transition[8] ? 6'sd0 : 6'sd2) + balance;
            end
        end
    end
endmodule

module nano_oser10 (
    input wire pixel_clk, input wire serial_clk, input wire reset,
    input wire [9:0] parallel_data, output wire serial_data
);
`ifdef SYNTHESIS
    OSER10 #(.GSREN("false"),.LSREN("true")) serializer (
        .D0(parallel_data[0]),.D1(parallel_data[1]),.D2(parallel_data[2]),
        .D3(parallel_data[3]),.D4(parallel_data[4]),.D5(parallel_data[5]),
        .D6(parallel_data[6]),.D7(parallel_data[7]),.D8(parallel_data[8]),
        .D9(parallel_data[9]),.PCLK(pixel_clk),.FCLK(serial_clk),.RESET(reset),.Q(serial_data)
    );
`else
    // Functional primitive model: load on PCLK and emit D0 first on DDR
    // serial edges. This does not model analog setup/hold or clock skew.
    reg [9:0] loaded, shifted;
    reg load_toggle, seen_toggle, output_bit;
    reg [3:0] position;
    initial begin loaded = 0; shifted = 0; load_toggle = 0; seen_toggle = 0; output_bit = 0; position = 0; end
    always @(posedge pixel_clk or posedge reset) begin
        if (reset) begin loaded <= 0; load_toggle <= 0; end
        else begin loaded <= parallel_data; load_toggle <= !load_toggle; end
    end
    always @(posedge serial_clk or negedge serial_clk or posedge reset) begin
        if (reset) begin shifted <= 0; seen_toggle <= 0; output_bit <= 0; position <= 0; end
        else if (seen_toggle != load_toggle) begin
            shifted <= loaded;
            seen_toggle <= load_toggle;
            output_bit <= loaded[0];
            position <= 1;
        end else begin
            output_bit <= shifted[position];
            position <= position == 9 ? 0 : position + 1'b1;
        end
    end
    assign serial_data = output_bit;
`endif
endmodule

module nano_tmds (
    input wire pixel_clk, input wire serial_clk, input wire reset,
    input wire [15:0] rgb565, input wire hsync, input wire vsync, input wire de,
    output wire [3:0] tmds_p, output wire [3:0] tmds_n
);
    wire [7:0] red = {rgb565[15:11],rgb565[15:13]};
    wire [7:0] green = {rgb565[10:5],rgb565[10:9]};
    wire [7:0] blue = {rgb565[4:0],rgb565[4:2]};
    wire [9:0] symbols [0:3];
    wire [3:0] serial_bits;
    nano_tmds_encoder blue_encoder (.clk(pixel_clk),.reset(reset),.ce(1'b1),
        .data(blue),.de(de),.c0(hsync),.c1(vsync),.symbol(symbols[0]));
    nano_tmds_encoder green_encoder (.clk(pixel_clk),.reset(reset),.ce(1'b1),
        .data(green),.de(de),.c0(1'b0),.c1(1'b0),.symbol(symbols[1]));
    nano_tmds_encoder red_encoder (.clk(pixel_clk),.reset(reset),.ce(1'b1),
        .data(red),.de(de),.c0(1'b0),.c1(1'b0),.symbol(symbols[2]));
    assign symbols[3] = 10'b0000011111;
    genvar lane;
    generate for (lane = 0; lane < 4; lane = lane + 1) begin: lanes
        nano_oser10 serializer (.pixel_clk(pixel_clk),.serial_clk(serial_clk),.reset(reset),
            .parallel_data(symbols[lane]),.serial_data(serial_bits[lane]));
`ifdef SYNTHESIS
        TLVDS_OBUF output_buffer (.I(serial_bits[lane]),.O(tmds_p[lane]),.OB(tmds_n[lane]));
`else
        assign tmds_p[lane] = serial_bits[lane];
        assign tmds_n[lane] = !serial_bits[lane];
`endif
    end endgenerate
endmodule
