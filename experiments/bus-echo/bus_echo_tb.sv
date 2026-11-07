`timescale 1ns/1ps
// Replays the MPC-2350 gpu_display.cpp bus sequences against the echo top.
module bus_echo_tb;
    reg hclk = 0, reset_n = 0, abort = 0, ale_n = 1, wr_n = 1, rd_n = 1;
    reg drive = 0; reg [7:0] dval = 0;
    wire [7:0] gpu_d = drive ? dval : 8'bz;
    wire wready, rvalid;
    tang20k_bus_echo dut(.gpu_hclk(hclk), .gpu_reset_n(reset_n), .gpu_abort(abort),
        .gpu_ale_n(ale_n), .gpu_wr_n(wr_n), .gpu_rd_n(rd_n), .gpu_d(gpu_d),
        .gpu_wready(wready), .gpu_rvalid(rvalid));
    localparam P = 50000; // 50 us HCLK phases, as in the firmware
    integer selected = -1, edges, failures = 0; reg pre; reg [7:0] value; reg ok;
    task low; begin hclk = 0; #P; end endtask
    task high; begin hclk = 1; #P; end endtask
    task write_cycle(input [7:0] b, input is_addr, output reg accepted_out);
        reg accepted; integer tries; begin
        rd_n = 1; wr_n = 1; low();
        dval = b; drive = 1; ale_n = is_addr ? 0 : 1; wr_n = 0; #P;
        accepted_out = 0;
        for (tries = 0; tries < 20 && !accepted_out; tries = tries + 1) begin
            accepted = wready === 1'b1;
            high(); low();
            if (accepted) begin wr_n = 1; ale_n = 1; drive = 0; accepted_out = 1; end
        end
        if (!accepted_out) begin wr_n = 1; ale_n = 1; drive = 0; end
    end endtask
    task read_at(input [7:0] a, output reg [7:0] out, output reg good);
        reg acc; begin
        good = 0;
        if (a != selected) begin
            write_cycle(a, 1, acc);
            if (!acc) begin selected = -1; disable read_at; end
            selected = a;
        end
        wr_n = 1; rd_n = 1; drive = 0; low();
        ale_n = 1; rd_n = 0; #P;
        pre = rvalid === 1'b1; edges = 0;
        while (!good && edges < 20) begin
            high(); edges = edges + 1;
            if (rvalid === 1'b1) begin out = gpu_d; rd_n = 1; good = 1; end
            low();
        end
        if (!good) begin rd_n = 1; selected = -1; end
    end endtask
    task pulse_reset; integer i; begin
        reset_n = 0;
        for (i = 0; i < 32; i = i + 1) begin high(); low(); end
        reset_n = 1;
        for (i = 0; i < 64; i = i + 1) begin high(); low(); end
        selected = -1;
    end endtask
    task expect_echo(input [7:0] a); begin
        read_at(a, value, ok);
        if (!ok || value !== a || edges != 1 || pre) begin
            failures = failures + 1;
            $display("FAIL reg %02h -> %02h ok=%0d edges=%0d pre=%0d", a, value, ok, edges, pre);
        end
    end endtask
    integer i;
    initial begin
        #100000;
        pulse_reset();
        // RVALID and the data pins stay released while RD_N is HIGH.
        if (rvalid !== 1'b0 || gpu_d !== 8'bz) begin failures = failures + 1; $display("FAIL idle bus"); end
        for (i = 64; i < 80; i = i + 1) expect_echo(i);          // GPU_PROBE
        selected = -1;
        expect_echo(8'h01); expect_echo(8'h02); expect_echo(8'h21); expect_echo(8'h23); // GPU_SWAPTEST
        pulse_reset();
        expect_echo(8'h41); expect_echo(8'h42); expect_echo(8'h43); expect_echo(8'h41); // GPU_LATCHTEST
        for (i = 0; i < 128; i = i + 1) begin selected = -1; expect_echo(i); end
        // An address with D7 HIGH is never accepted (WREADY = !D7).
        selected = -1; read_at(8'hC1, value, ok);
        if (ok) begin failures = failures + 1; $display("FAIL D7 address accepted"); end
        // GPU_HOLD-style held read keeps RVALID and the byte while HCLK stays HIGH.
        selected = -1; write_cycle(8'h41, 1, ok);
        wr_n = 1; rd_n = 1; low(); ale_n = 1; rd_n = 0; #P; high(); #(10 * P);
        if (rvalid !== 1'b1 || gpu_d !== 8'h41) begin failures = failures + 1; $display("FAIL held read"); end
        rd_n = 1; low();
        if (failures) $fatal(1, "bus echo: %0d failures", failures);
        $display("PASS bus echo: probe, swap test, latch test, all 128 addresses, D7 refusal and held read");
        $finish;
    end
endmodule
