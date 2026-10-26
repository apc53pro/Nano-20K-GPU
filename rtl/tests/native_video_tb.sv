`timescale 1ns/1ps
module native_video_tb #(
    parameter integer CLEAN_FRAMES = 5,
    parameter integer CONTENTION = 0,
    parameter integer SWAP_ON_COMPLETION = 0
);
    reg clk = 0;
    always #5 clk = !clk;
    reg reset = 1, memory_ready = 0, front_id = 0;
    reg [1:0] phase = 0;
    wire pixel_ce = phase == 3;
    always @(posedge clk) if (reset) phase <= 0; else phase <= phase + 1'b1;
    wire [15:0] pixel;
    wire hsync,vsync,de,vblank;
    wire [9:0] x;
    wire [8:0] y;
    wire [31:0] underflows;
    wire cmd_valid,cmd_write,w_valid,r_ready;
    wire [22:0] cmd_addr;
    wire [4:0] cmd_words;
    wire [31:0] w_data;
    wire [3:0] w_strb;
    reg memory_busy = 0, response_valid = 0, response_last = 0, memory_done = 0;
    reg [31:0] response_data;
    integer word_address = 0, word_index = 0, delay_cycles = 0, cycles = 0;
    integer blank_count = 0, frames = 0, checked_pixels = 0;
    reg block_memory = 0;
    reg block_commands = 0, held_command = 0;
    reg [22:0] held_address;
    integer swap_countdown = 0;
    integer foreground_wait = 0, scanout_streak = 0;
    reg completion_swap_armed = 0;
    integer completion_swaps = 0;
    // Model the shared service's bounded scanout priority. An offered scanout
    // command wins up to four times; an empty request slot lets a queued
    // foreground WRITE hold the service while its payload is staged.
    wire command_ready = !memory_busy && !block_commands && foreground_wait == 0 &&
        (!CONTENTION || scanout_streak < 4);

    function [15:0] image_pixel;
        input integer absolute_pixel;
        integer local_pixel;
        begin
            local_pixel = absolute_pixel % 307200;
            image_pixel = ((local_pixel * 13) ^ (local_pixel >> 7)) ^
                (absolute_pixel >= 307200 ? 16'ha55a : 16'h5aa5);
        end
    endfunction
    always @* begin
        response_valid = memory_busy && delay_cycles == 0 && !block_memory;
        response_last = word_index == 15;
        response_data = {image_pixel((word_address + word_index)*2 + 1),
                         image_pixel((word_address + word_index)*2)};
    end
    always @(posedge clk) begin
        memory_done <= 0;
        if (reset) begin
            memory_busy <= 0; word_index <= 0; delay_cycles <= 0;
            foreground_wait <= 0; scanout_streak <= 0;
        end
        else begin
            if (foreground_wait != 0) foreground_wait <= foreground_wait - 1;
            if (cmd_valid && command_ready) begin
                if (cmd_write || cmd_words != 16 || cmd_addr[1:0] != 0)
                    $fatal(1,"Invalid scanout memory command");
                memory_busy <= 1; word_address <= cmd_addr >> 2;
                word_index <= 0; delay_cycles <= 5;
                scanout_streak <= scanout_streak + 1;
            end else if (CONTENTION && !memory_busy && foreground_wait == 0 &&
                         (!cmd_valid || scanout_streak >= 4)) begin
                foreground_wait <= 220;
                scanout_streak <= 0;
            end else if (delay_cycles != 0) delay_cycles <= delay_cycles - 1;
            if (response_valid && r_ready) begin
                if (response_last) begin memory_busy <= 0; memory_done <= 1; end
                else word_index <= word_index + 1;
            end
        end
    end
    always @(posedge clk) begin
        if (reset) held_command <= 0;
        else begin
            if (held_command && (!cmd_valid || cmd_addr != held_address))
                $fatal(1,"Backpressured scanout command changed during a front swap");
            held_command <= cmd_valid && !command_ready;
            held_address <= cmd_addr;
        end
    end
    nano_scanout dut (.clk(clk),.reset(reset),.pixel_ce(pixel_ce),.memory_ready(memory_ready),
        .front_id(front_id),.pixel(pixel),.hsync(hsync),.vsync(vsync),.de(de),.x(x),.y(y),
        .vblank_pulse(vblank),.underflows(underflows),
        .mem_cmd_valid(cmd_valid),.mem_cmd_ready(command_ready),.mem_cmd_write(cmd_write),
        .mem_cmd_addr(cmd_addr),.mem_cmd_words(cmd_words),.mem_w_valid(w_valid),
        .mem_w_ready(1'b0),.mem_w_data(w_data),.mem_w_strb(w_strb),
        .mem_r_valid(response_valid),.mem_r_ready(r_ready),.mem_r_data(response_data),
        .mem_r_last(response_last),.mem_done(memory_done),.mem_error(1'b0));
    initial begin
        repeat (4) @(negedge clk);
        reset = 0;
        repeat (10) @(negedge clk);
        memory_ready = 1;
    end
    always @(negedge clk) begin
        // Precisely combine completion of the old refill, acceptance of the
        // next queued refill, and a new-front generation flush. Every response
        // from that just-accepted old-front command must be discarded.
        if (completion_swap_armed && memory_done && cmd_valid && command_ready) begin
            front_id = 1;
            completion_swap_armed = 0;
            completion_swaps = completion_swaps + 1;
        end
    end
    always @(posedge clk) begin
        #1;
        if (!reset) begin
            cycles = cycles + 1;
            if (cycles > (CLEAN_FRAMES + 2) * 1680000) $fatal(1,"Scanout test timed out");
            if (swap_countdown != 0) begin
                swap_countdown = swap_countdown - 1;
                if (swap_countdown == 5) front_id = 1;
                if (swap_countdown == 0) block_commands = 0;
            end
            if (vblank) begin
                blank_count = blank_count + 1;
                if (blank_count == 2) begin
                    if (SWAP_ON_COMPLETION) completion_swap_armed = 1;
                    else begin block_commands = 1; swap_countdown = 8; end
                end
                if (blank_count == CLEAN_FRAMES + 1) block_memory = 1;
            end
            if (phase == 0 && de) begin
                if (blank_count == 0) begin
                    if (pixel != 0) $fatal(1,"Startup did not produce black");
                end else if (blank_count <= CLEAN_FRAMES) begin
                    if (pixel !== image_pixel(y*640 + x + (front_id ? 307200 : 0)))
                        $fatal(1,"FIFO pixel mismatch at (%0d,%0d) got %h expected %h",x,y,pixel,
                            image_pixel(y*640 + x + (front_id ? 307200 : 0)));
                    checked_pixels = checked_pixels + 1;
                    if (x == 639 && y == 479) frames = frames + 1;
                end else if (underflows != 0 && x == 100 && y == 0) begin
                    if (pixel != 0 || underflows != 1 || checked_pixels != CLEAN_FRAMES*307200 || frames != CLEAN_FRAMES)
                        $fatal(1,"Underflow recovery/accounting mismatch");
                    if (SWAP_ON_COMPLETION && completion_swaps != 1)
                        $fatal(1,"Concurrent completion/accept/swap was not exercised");
                    $display("PASS scanout: %0d consecutive native-FIFO frames, contention=%0d, front swap, unchanged-front repeats, startup and underflow",CLEAN_FRAMES,CONTENTION);
                    $finish;
                end
                if (!hsync || !vsync) $fatal(1,"Sync asserted during active video");
            end
        end
    end
endmodule

module scanout_contention_tb;
    native_video_tb #(.CLEAN_FRAMES(3),.CONTENTION(1),.SWAP_ON_COMPLETION(1)) scenario();
endmodule

module sdp16_async_tb;
    timeunit 1ps;
    timeprecision 1ps;
    reg wclk = 0, rclk = 0, wce = 1, we = 1, rce = 1, re = 1, reset_output = 1;
    reg [9:0] waddr = 7, raddr = 7;
    reg [15:0] wdata = 16'h1234;
    wire [15:0] rdata;
    nano_sdp16 dut(.wclk(wclk),.wce(wce),.we(we),.waddr(waddr),.wdata(wdata),
        .rclk(rclk),.rce(rce),.re(re),.raddr(raddr),.reset_output(reset_output),.rdata(rdata));
    initial begin
        // A time-zero output-reset edge is not a memory read/collision.
        wclk = 1; rclk = 1;
        #1; wclk = 0; rclk = 0; reset_output = 0; we = 0;
        #1; wclk = 1;
        #1; rclk = 1;
        #1; if (rdata !== 16'h1234) $fatal(1,"Separated asynchronous read failed");
        // re remains high, but a held rclk level causes no second read.
        wclk = 0; we = 1; wdata = 16'h5678;
        #1; wclk = 1;
        #1; if (rdata !== 16'h1234) $fatal(1,"Held read clock did not retain output");
        wclk = 0; we = 0; rclk = 0;
        #1; rclk = 1;
        #1; if (rdata !== 16'h5678) $fatal(1,"Later asynchronous read lost write");
        // Truly coincident active edges are legal at different addresses.
        wclk = 0; rclk = 0; we = 1; raddr = 8;
        #1; wclk = 1; rclk = 1;
        #1; if (rdata !== 16'h0000) $fatal(1,"Distinct-address coincident read failed");
        $display("PASS SDPB: asynchronous clocks, held clock levels and time-zero output reset");
        $finish;
    end
endmodule

module sdp16_collision_tb;
    timeunit 1ps;
    timeprecision 1ps;
    reg wclk = 0, rclk = 0;
    wire [15:0] rdata;
    nano_sdp16 dut(.wclk(wclk),.wce(1'b1),.we(1'b1),.waddr(10'd7),.wdata(16'h1234),
        .rclk(rclk),.rce(1'b1),.re(1'b1),.raddr(10'd7),.reset_output(1'b0),.rdata(rdata));
    initial begin
        #2; wclk = 1; rclk = 1;
        #1; $fatal(1,"Collision checker failed to reject undefined simultaneous access");
    end
endmodule

module tmds_encoder_tb;
    reg clk = 0;
    always #5 clk = !clk;
    reg reset = 1, ce = 1, de = 0, c0 = 0, c1 = 0;
    reg [7:0] data = 0;
    wire [9:0] symbol;
    integer n,i,ones,run_balance;
    reg [7:0] transition,decoded;
    reg [9:0] expected;
    nano_tmds_encoder dut(.clk(clk),.reset(reset),.ce(ce),.data(data),.de(de),.c0(c0),.c1(c1),.symbol(symbol));
    initial begin
        @(negedge clk); reset = 0;
        for (n = 0; n < 4; n = n + 1) begin
            {c1,c0} = n[1:0];
            case(n)
                0: expected = 10'b1101010100;
                1: expected = 10'b0010101011;
                2: expected = 10'b0101010100;
                3: expected = 10'b1010101011;
            endcase
            @(posedge clk); #1;
            if (symbol !== expected) $fatal(1,"Incorrect TMDS control symbol");
            @(negedge clk);
        end
        de = 1; run_balance = 0;
        for (n = 0; n < 4096; n = n + 1) begin
            data = ((n * 17) ^ (n >> 3));
            @(posedge clk); #1;
            transition = symbol[9] ? ~symbol[7:0] : symbol[7:0];
            decoded[0] = transition[0];
            for (i = 1; i < 8; i = i + 1)
                decoded[i] = (transition[i] ^ transition[i-1]) ^ !symbol[8];
            if (decoded !== data) $fatal(1,"TMDS symbol did not decode to input data=%h symbol=%b transition=%h decoded=%h",data,symbol,transition,decoded);
            ones = 0;
            for (i = 0; i < 10; i = i + 1) ones = ones + symbol[i];
            run_balance = run_balance + 2*ones - 10;
            if (run_balance < -8 || run_balance > 8) $fatal(1,"TMDS disparity escaped bound %0d",run_balance);
            @(negedge clk);
        end
        ce = 0; expected = symbol; data = ~data;
        repeat (3) begin @(posedge clk); #1; if (symbol !== expected) $fatal(1,"TMDS CE did not hold output"); end
        $display("PASS TMDS: all controls, 4096 data decodes, bounded disparity and CE hold");
        $finish;
    end
endmodule

module sdp16_tb;
    reg clk = 0;
    always #5 clk = !clk;
    reg reset_output = 1, we = 0, re = 0, wce = 1, rce = 1;
    reg [9:0] waddr = 0, raddr = 0;
    reg [15:0] wdata = 0;
    wire [15:0] rdata;
    nano_sdp16 dut(.wclk(clk),.wce(wce),.we(we),.waddr(waddr),.wdata(wdata),
        .rclk(clk),.rce(rce),.re(re),.raddr(raddr),.reset_output(reset_output),.rdata(rdata));
    initial begin
        @(negedge clk); reset_output = 0; we = 1; waddr = 1023; wdata = 16'haf31;
        @(negedge clk); waddr = 0; wdata = 16'h1256;
        @(negedge clk); we = 0; re = 1; raddr = 1023;
        @(posedge clk); #1; if (rdata !== 16'haf31) $fatal(1,"Native synchronous read failed");
        @(negedge clk); re = 0; raddr = 0;
        @(posedge clk); #1; if (rdata !== 16'haf31) $fatal(1,"Disabled read did not hold");
        @(negedge clk); reset_output = 1;
        @(posedge clk); #1; if (rdata !== 0) $fatal(1,"Output reset failed");
        @(negedge clk); reset_output = 0; re = 1;
        @(posedge clk); #1; if (rdata !== 16'h1256) $fatal(1,"Output reset cleared RAM contents");
        $display("PASS SDPB: synchronous output, enables, endpoints and output-only reset");
        $finish;
    end
endmodule
