// One selected Gowin 18-Kbit BSRAM, SDPX9B 512x36, bypassed output register.
// The native synchronous read output changes after the rclk edge and is held
// when disabled. Reset clears that output only, never the RAM contents.
// Read and write ports may use independent clocks. Callers own CDC and must
// not read and write the same address on coincident edges (undefined in silicon).
module nano_sdp36 (
    input wire wclk, input wire wce, input wire we,
    input wire [8:0] waddr, input wire [35:0] wdata,
    input wire rclk, input wire rce, input wire re,
    input wire [8:0] raddr, input wire reset_output,
    output wire [35:0] rdata
);
`ifndef SYNTHESIS
    // Compare absolute edge times at simulator precision, not rounded ns.
    // This is an assertion-only checker and adds no implementation storage.
    timeunit 1ps;
    timeprecision 1ps;
`endif
`ifdef SYNTHESIS
    wire [35:0] native_data;
    SDPX9B #(
        .READ_MODE(1'b0), .BIT_WIDTH_0(36), .BIT_WIDTH_1(36),
        .BLK_SEL_0(3'b0), .BLK_SEL_1(3'b0), .RESET_MODE("SYNC")
    ) bank (
        .CLKA(wclk), .CLKB(rclk), .CEA(wce && we),
        .CEB(rce && (re || reset_output)), .RESETA(1'b0),
        .RESETB(reset_output), .OCE(1'b0),
        .ADA({waddr,5'b0}), .ADB({raddr,5'b0}), .DI(wdata),
        .BLKSELA(3'b0), .BLKSELB(3'b0), .DO(native_data)
    );
    assign rdata = native_data;
`else
    reg [35:0] memory [0:511];
    reg [35:0] output_word;
    integer word_index;
    time last_write_edge, last_read_edge;
    reg write_edge_seen, read_edge_seen;
    reg [8:0] last_write_address, last_read_address;
    initial begin
        output_word = 0;
        write_edge_seen = 0; read_edge_seen = 0;
        last_write_edge = 0; last_read_edge = 0;
        last_write_address = 0; last_read_address = 0;
        for (word_index = 0; word_index < 512; word_index = word_index + 1)
            memory[word_index] = 0;
    end
    always @(posedge wclk) if (wce && we) memory[waddr] <= wdata;
    always @(posedge rclk) begin
        if (rce && reset_output) output_word <= 0;
        else if (rce && re) output_word <= memory[raddr];
    end
    // Blocking updates are intentional assertion bookkeeping: whichever
    // coincident process executes second sees the other port's active edge.
    // Checking a clock's level would falsely flag held or asynchronous clocks.
    always @(posedge wclk) begin
        if (wce && we) begin
            if (read_edge_seen && last_read_edge == $time && last_read_address == waddr)
                $fatal(1,"nano_sdp36 undefined same-address read/write collision at address %0d",waddr);
            last_write_edge = $time;
            last_write_address = waddr;
            write_edge_seen = 1;
        end
    end
    always @(posedge rclk) begin
        if (rce && re && !reset_output) begin
            if (write_edge_seen && last_write_edge == $time && last_write_address == raddr)
                $fatal(1,"nano_sdp36 undefined same-address read/write collision at address %0d",raddr);
            last_read_edge = $time;
            last_read_address = raddr;
            read_edge_seen = 1;
        end
    end
    assign rdata = output_word;
`endif
endmodule
