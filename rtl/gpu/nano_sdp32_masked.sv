// One native SDPB 512x32 bank with four active-high byte-write lanes.
// Reads are synchronous and hold their output when disabled. Reset clears
// only the output register. Same-word cross-port read/write is forbidden,
// even when a write enables only some of that word's bytes.
module nano_sdp32_masked (
    input wire wclk, input wire wce, input wire we,
    input wire [3:0] wstrb, input wire [8:0] waddr,
    input wire [31:0] wdata,
    input wire rclk, input wire rce, input wire re,
    input wire [8:0] raddr, input wire reset_output,
    output wire [31:0] rdata
);
`ifndef SYNTHESIS
    timeunit 1ps;
    timeprecision 1ps;
`endif
`ifdef SYNTHESIS
    SDPB #(
        .READ_MODE(1'b0), .BIT_WIDTH_0(32), .BIT_WIDTH_1(32),
        .BLK_SEL_0(3'b0), .BLK_SEL_1(3'b0), .RESET_MODE("SYNC")
    ) bank (
        .CLKA(wclk), .CLKB(rclk), .CEA(wce && we && (|wstrb)),
        .CEB(rce && (re || reset_output)), .RESETA(1'b0),
        .RESETB(reset_output), .OCE(1'b0),
        // In the vendor's SDPB x32 mode ADA[3:0] enables the four
        // eight-bit lanes; ADA[4] is unused and ADA[13:5] is the word.
        .ADA({waddr,1'b0,wstrb}), .ADB({raddr,5'b0}), .DI(wdata),
        .BLKSELA(3'b0), .BLKSELB(3'b0), .DO(rdata)
    );
`else
    reg [31:0] memory [0:511];
    reg [31:0] output_word;
    integer word_index, lane;
    time last_write_edge, last_read_edge;
    reg write_edge_seen, read_edge_seen;
    reg [8:0] last_write_address, last_read_address;
    initial begin
        output_word=0;write_edge_seen=0;read_edge_seen=0;
        last_write_edge=0;last_read_edge=0;
        last_write_address=0;last_read_address=0;
        for(word_index=0;word_index<512;word_index=word_index+1)memory[word_index]=0;
    end
    always @(posedge wclk)if(wce&&we)begin
        for(lane=0;lane<4;lane=lane+1)
            if(wstrb[lane])memory[waddr][8*lane +:8]<=wdata[8*lane +:8];
    end
    always @(posedge rclk)begin
        if(rce&&reset_output)output_word<=0;
        else if(rce&&re)output_word<=memory[raddr];
    end
    // Blocking assignments are assertion bookkeeping so either process can
    // detect simultaneous active edges, including independent clocks.
    always @(posedge wclk)if(wce&&we&&(|wstrb))begin
        if(read_edge_seen&&last_read_edge==$time&&last_read_address==waddr)
            $fatal(1,"nano_sdp32_masked undefined same-word read/write collision at address %0d",waddr);
        last_write_edge=$time;last_write_address=waddr;write_edge_seen=1;
    end
    always @(posedge rclk)if(rce&&re&&!reset_output)begin
        if(write_edge_seen&&last_write_edge==$time&&last_write_address==raddr)
            $fatal(1,"nano_sdp32_masked undefined same-word read/write collision at address %0d",raddr);
        last_read_edge=$time;last_read_address=raddr;read_edge_seen=1;
    end
    assign rdata=output_word;
`endif
endmodule
