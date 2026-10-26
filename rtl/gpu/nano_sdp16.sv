// One selected Gowin 18-Kbit BSRAM, SDPB 1024x16, bypassed output register.
// The native synchronous read output changes after the rclk edge and is held
// when disabled. Reset clears that output only, never the RAM contents.
// Read and write ports may use independent clocks. Callers own CDC and must
// not read and write the same address on coincident edges (undefined in silicon).
module nano_sdp16 #(
    parameter INIT_FILE = "",
    parameter [16383:0] INIT_BITS = 16384'b0
) (
    input wire wclk, input wire wce, input wire we,
    input wire [9:0] waddr, input wire [15:0] wdata,
    input wire rclk, input wire rce, input wire re,
    input wire [9:0] raddr, input wire reset_output,
    output wire [15:0] rdata
);
`ifndef SYNTHESIS
    // Compare absolute edge times at simulator precision, not rounded ns.
    // This is an assertion-only checker and adds no implementation storage.
    timeunit 1ps;
    timeprecision 1ps;
`endif
`ifdef SYNTHESIS
    // File-based ROM initialization must be converted to INIT_BITS by the
    // generator; accepting a simulation-only image here would describe a
    // different circuit from the one synthesized.
    initial begin
        if (INIT_FILE != "") $error("Use explicit INIT_BITS for synthesized nano_sdp16 initialization");
    end
    wire [31:0] native_data;
    SDPB #(
        .READ_MODE(1'b0), .BIT_WIDTH_0(16), .BIT_WIDTH_1(16),
        .BLK_SEL_0(3'b0), .BLK_SEL_1(3'b0), .RESET_MODE("SYNC"),
        .INIT_RAM_00(INIT_BITS[255:0]),
        .INIT_RAM_01(INIT_BITS[511:256]),
        .INIT_RAM_02(INIT_BITS[767:512]),
        .INIT_RAM_03(INIT_BITS[1023:768]),
        .INIT_RAM_04(INIT_BITS[1279:1024]),
        .INIT_RAM_05(INIT_BITS[1535:1280]),
        .INIT_RAM_06(INIT_BITS[1791:1536]),
        .INIT_RAM_07(INIT_BITS[2047:1792]),
        .INIT_RAM_08(INIT_BITS[2303:2048]),
        .INIT_RAM_09(INIT_BITS[2559:2304]),
        .INIT_RAM_0A(INIT_BITS[2815:2560]),
        .INIT_RAM_0B(INIT_BITS[3071:2816]),
        .INIT_RAM_0C(INIT_BITS[3327:3072]),
        .INIT_RAM_0D(INIT_BITS[3583:3328]),
        .INIT_RAM_0E(INIT_BITS[3839:3584]),
        .INIT_RAM_0F(INIT_BITS[4095:3840]),
        .INIT_RAM_10(INIT_BITS[4351:4096]),
        .INIT_RAM_11(INIT_BITS[4607:4352]),
        .INIT_RAM_12(INIT_BITS[4863:4608]),
        .INIT_RAM_13(INIT_BITS[5119:4864]),
        .INIT_RAM_14(INIT_BITS[5375:5120]),
        .INIT_RAM_15(INIT_BITS[5631:5376]),
        .INIT_RAM_16(INIT_BITS[5887:5632]),
        .INIT_RAM_17(INIT_BITS[6143:5888]),
        .INIT_RAM_18(INIT_BITS[6399:6144]),
        .INIT_RAM_19(INIT_BITS[6655:6400]),
        .INIT_RAM_1A(INIT_BITS[6911:6656]),
        .INIT_RAM_1B(INIT_BITS[7167:6912]),
        .INIT_RAM_1C(INIT_BITS[7423:7168]),
        .INIT_RAM_1D(INIT_BITS[7679:7424]),
        .INIT_RAM_1E(INIT_BITS[7935:7680]),
        .INIT_RAM_1F(INIT_BITS[8191:7936]),
        .INIT_RAM_20(INIT_BITS[8447:8192]),
        .INIT_RAM_21(INIT_BITS[8703:8448]),
        .INIT_RAM_22(INIT_BITS[8959:8704]),
        .INIT_RAM_23(INIT_BITS[9215:8960]),
        .INIT_RAM_24(INIT_BITS[9471:9216]),
        .INIT_RAM_25(INIT_BITS[9727:9472]),
        .INIT_RAM_26(INIT_BITS[9983:9728]),
        .INIT_RAM_27(INIT_BITS[10239:9984]),
        .INIT_RAM_28(INIT_BITS[10495:10240]),
        .INIT_RAM_29(INIT_BITS[10751:10496]),
        .INIT_RAM_2A(INIT_BITS[11007:10752]),
        .INIT_RAM_2B(INIT_BITS[11263:11008]),
        .INIT_RAM_2C(INIT_BITS[11519:11264]),
        .INIT_RAM_2D(INIT_BITS[11775:11520]),
        .INIT_RAM_2E(INIT_BITS[12031:11776]),
        .INIT_RAM_2F(INIT_BITS[12287:12032]),
        .INIT_RAM_30(INIT_BITS[12543:12288]),
        .INIT_RAM_31(INIT_BITS[12799:12544]),
        .INIT_RAM_32(INIT_BITS[13055:12800]),
        .INIT_RAM_33(INIT_BITS[13311:13056]),
        .INIT_RAM_34(INIT_BITS[13567:13312]),
        .INIT_RAM_35(INIT_BITS[13823:13568]),
        .INIT_RAM_36(INIT_BITS[14079:13824]),
        .INIT_RAM_37(INIT_BITS[14335:14080]),
        .INIT_RAM_38(INIT_BITS[14591:14336]),
        .INIT_RAM_39(INIT_BITS[14847:14592]),
        .INIT_RAM_3A(INIT_BITS[15103:14848]),
        .INIT_RAM_3B(INIT_BITS[15359:15104]),
        .INIT_RAM_3C(INIT_BITS[15615:15360]),
        .INIT_RAM_3D(INIT_BITS[15871:15616]),
        .INIT_RAM_3E(INIT_BITS[16127:15872]),
        .INIT_RAM_3F(INIT_BITS[16383:16128])
    ) bank (
        .CLKA(wclk), .CLKB(rclk), .CEA(wce && we),
        .CEB(rce && (re || reset_output)), .RESETA(1'b0),
        .RESETB(reset_output), .OCE(1'b0),
        .ADA({waddr,4'b0}), .ADB({raddr,4'b0}), .DI({16'b0,wdata}),
        .BLKSELA(3'b0), .BLKSELB(3'b0), .DO(native_data)
    );
    assign rdata = native_data[15:0];
`else
    reg [15:0] memory [0:1023];
    reg [15:0] output_word;
    integer word_index;
    time last_write_edge, last_read_edge;
    reg write_edge_seen, read_edge_seen;
    reg [9:0] last_write_address, last_read_address;
    initial begin
        output_word = 0;
        write_edge_seen = 0; read_edge_seen = 0;
        last_write_edge = 0; last_read_edge = 0;
        last_write_address = 0; last_read_address = 0;
        for (word_index = 0; word_index < 1024; word_index = word_index + 1)
            memory[word_index] = INIT_BITS[16*word_index +: 16];
        if (INIT_FILE != "") $readmemh(INIT_FILE, memory);
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
                $fatal(1,"nano_sdp16 undefined same-address read/write collision at address %0d",waddr);
            last_write_edge = $time;
            last_write_address = waddr;
            write_edge_seen = 1;
        end
    end
    always @(posedge rclk) begin
        if (rce && re && !reset_output) begin
            if (write_edge_seen && last_write_edge == $time && last_write_address == raddr)
                $fatal(1,"nano_sdp16 undefined same-address read/write collision at address %0d",raddr);
            last_read_edge = $time;
            last_read_address = raddr;
            read_edge_seen = 1;
        end
    end
    assign rdata = output_word;
`endif
endmodule
