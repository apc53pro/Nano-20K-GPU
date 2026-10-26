`timescale 1ns/1ps
module nano_sdp32_tb;
    reg wclk=0,wce=1,we=0,rclk=0,rce=1,re=0,reset_output=0;
    reg [8:0] waddr=0,raddr=0;
    reg [31:0] wdata=0;
    wire [31:0] rdata;
    nano_sdp32 dut(.*);
    task write_edge;
        begin #3;wclk=1;#2;wclk=0;end
    endtask
    task read_edge;
        begin #4;rclk=1;#3;rclk=0;end
    endtask
    task expect_word;
        input [31:0] expected;
        begin if(rdata!==expected)$fatal(1,"SDPB32 data=%h expected=%h",rdata,expected);end
    endtask
    initial begin
        expect_word(0);
        we=1;waddr=0;wdata=32'h81234567;write_edge();
        waddr=511;wdata=32'hfedcba98;write_edge();we=0;
        // Read data changes only after an enabled read clock edge.
        raddr=511;#2;expect_word(0);re=1;read_edge();expect_word(32'hfedcba98);
        re=0;raddr=0;read_edge();expect_word(32'hfedcba98);
        re=1;rce=0;read_edge();expect_word(32'hfedcba98);
        rce=1;read_edge();expect_word(32'h81234567);
        // Both write gates are honored, including the highest address.
        we=1;wce=0;waddr=511;wdata=0;write_edge();
        we=0;wce=1;waddr=0;write_edge();
        raddr=511;read_edge();expect_word(32'hfedcba98);
        raddr=0;read_edge();expect_word(32'h81234567);
        // Reset is gated by read clock enable and clears only the output.
        reset_output=1;rce=0;read_edge();expect_word(32'h81234567);
        rce=1;re=0;read_edge();expect_word(0);
        reset_output=0;re=1;read_edge();expect_word(32'h81234567);
        raddr=511;read_edge();expect_word(32'hfedcba98);
        // Coincident independent edges at DIFFERENT addresses are supported.
        waddr=9;wdata=32'h55aa00ff;we=1;raddr=0;
        #3;wclk=1;rclk=1;#2;wclk=0;rclk=0;we=0;expect_word(32'h81234567);
        raddr=9;read_edge();expect_word(32'h55aa00ff);
        $display("PASS native SDPB512x32 timing, enables, endpoints and reset retention");
        $finish;
    end
endmodule
