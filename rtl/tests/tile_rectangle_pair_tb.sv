`timescale 1ns/1ps

// Check rectangle issue rate and exact attachments using half-open coordinate
// intersections and an independent per-pixel depth test.
module nano_tile_rectangle_pair_tb;
    localparam integer ORIGIN_X = 608, ORIGIN_Y = 448;
    localparam integer VIEW_WIDTH = 640, VIEW_HEIGHT = 480;
    localparam integer RASTER = 8;
    localparam [2:0] COLOR = 3'b001, TEST = 3'b010, DEPTH = 3'b100;
    localparam [1:0] LESS = 0, LEQUAL = 1, ALWAYS = 2;

    reg clk = 0, reset = 0, cmd_word_we = 0, cmd_valid = 0, pix_re = 0;
    reg [1:0] pix_we = 0;
    wire pix_write_pair = 0;
    reg [6:0] cmd_word_index = 0;
    reg [31:0] cmd_word_data = 0;
    wire [9:0] cmd_tile_x = ORIGIN_X;
    wire [8:0] cmd_tile_y = ORIGIN_Y;
    reg [9:0] pix_addr = 0, pix_raddr = 0;
    reg [31:0] pix_color = 0;
    reg [23:0] pix_depth = 0;
    wire cmd_ready, busy, done, fault, pix_rvalid;
    wire [31:0] pix_rcolor;
    wire [23:0] pix_rdepth;
    wire tex_flush = 0, tex_cfg_ready = 1, tex_req_ready = 1;
    wire tex_rsp_valid = 0, tex_fault = 0;
    wire [15:0] tex_rsp_texel = 0;
    wire [5:0] tex_rsp_tag = 0;
    wire tex_cfg_valid, tex_cfg_repeat_u, tex_cfg_repeat_v;
    wire [22:0] tex_cfg_base, tex_cfg_pitch;
    wire [10:0] tex_cfg_width, tex_cfg_height;
    wire tex_req_valid, tex_rsp_ready;
    wire signed [31:0] tex_req_x, tex_req_y;
    wire [5:0] tex_req_tag;
    nano_tile_renderer dut(.*);

    reg [31:0] record [0:19];
    reg [15:0] expected_color [0:1023];
    reg [23:0] expected_depth [0:1023];
    integer case_count = 0, checked_pixels = 0, paired_issues = 0;

    task automatic tick;
        begin
            #5; clk = 1;
            #5; clk = 0;
        end
    endtask

    task automatic initialize_pixel(
        input integer address,
        input [15:0] color,
        input [23:0] depth);
        begin
            expected_color[address] = color;
            expected_depth[address] = depth;
            pix_we = 3;
            pix_addr = address;
            pix_color = {16'habcd, color};
            pix_depth = depth;
            tick();
            pix_we = 0;
        end
    endtask

    task automatic inspect_tile;
        integer address, even_address;
        begin
            if (!cmd_ready || busy || fault)
                $fatal(1, "attachment inspection before rectangle retirement");
            pix_re = 1;
            for (address = 0; address < 1024; address = address + 1) begin
                even_address = address & 1022;
                pix_raddr = address;
                tick();
                if (!pix_rvalid ||
                    pix_rcolor !== {expected_color[even_address + 1], expected_color[even_address]} ||
                    pix_rdepth !== expected_depth[address])
                    $fatal(1, "rectangle case %0d address %0d color %h expected %h%h, D24 %h expected %h",
                        case_count, address, pix_rcolor, expected_color[even_address + 1],
                        expected_color[even_address], pix_rdepth, expected_depth[address]);
                checked_pixels = checked_pixels + 1;
            end
            pix_re = 0;
            tick();
        end
    endtask

    task automatic record_rectangle(
        input integer x0, y0, x1, y1,
        input integer sx0, sy0, sx1, sy1,
        input [2:0] flags,
        input [1:0] depth_func,
        input [23:0] rgb, depth);
        integer word_index;
        begin
            for (word_index = 0; word_index < 20; word_index = word_index + 1)
                record[word_index] = 0;
            record[0] = {13'b0, flags, 8'd2, 8'd1};
            record[1] = {22'b0, depth_func, 8'b0};
            record[2] = {8'b0, rgb};
            record[4] = sx0;
            record[5] = sy0;
            record[6] = sx1;
            record[7] = sy1;
            record[8] = x0;
            record[9] = y0;
            record[10] = x1;
            record[11] = y1;
            record[16] = {8'b0, depth};
        end
    endtask

    task automatic launch;
        integer word_index;
        begin
            if (!cmd_ready)
                $fatal(1, "rectangle command launched while renderer occupied");
            cmd_word_we = 1;
            for (word_index = 0; word_index < 20; word_index = word_index + 1) begin
                cmd_word_index = word_index;
                cmd_word_data = record[word_index];
                tick();
            end
            cmd_word_we = 0;
            cmd_valid = 1;
            tick();
            cmd_valid = 0;
        end
    endtask

    task automatic submit_rectangle(
        input integer x0, y0, x1, y1,
        input integer sx0, sy0, sx1, sy1,
        input [2:0] flags,
        input [1:0] depth_func,
        input [23:0] rgb, depth);
        integer x, y, address, row_coverage, expected_raster, expected_samples;
        integer raster_edges, elapsed;
        reg depth_accept;
        reg [15:0] packed_color;
        begin
            case_count = case_count + 1;
            expected_raster = 0;
            expected_samples = 0;
            packed_color = (rgb[7:0] / 8) * 2048 + (rgb[15:8] / 4) * 32 + rgb[23:16] / 8;
            for (y = ORIGIN_Y; y < ORIGIN_Y + 32; y = y + 1) begin
                row_coverage = 0;
                for (x = ORIGIN_X; x < ORIGIN_X + 32; x = x + 1) begin
                    if (x < VIEW_WIDTH && y < VIEW_HEIGHT && x >= x0 && x < x1 &&
                        y >= y0 && y < y1 && x >= sx0 && x < sx1 && y >= sy0 && y < sy1) begin
                        address = 32 * (y - ORIGIN_Y) + (x - ORIGIN_X);
                        row_coverage = row_coverage + 1;
                        expected_samples = expected_samples + 1;
                        // Count one transaction per occupied aligned pair in this row.
                        if ((x % 2) == 0 || row_coverage == 1)
                            expected_raster = expected_raster + 1;
                        depth_accept = !(flags & TEST) || depth_func == ALWAYS ||
                            depth < expected_depth[address] ||
                            (depth_func == LEQUAL && depth == expected_depth[address]);
                        if (depth_accept && (flags & COLOR))
                            expected_color[address] = packed_color;
                        if (depth_accept && (flags & DEPTH))
                            expected_depth[address] = depth;
                    end
                end
            end

            record_rectangle(x0, y0, x1, y1, sx0, sy0, sx1, sy1, flags, depth_func, rgb, depth);
            launch();
            elapsed = 0;
            raster_edges = 0;
            while (!done && !fault && elapsed < 1100) begin
                if (dut.state == RASTER)
                    raster_edges = raster_edges + 1;
                if (tex_req_valid || tex_cfg_valid)
                    $fatal(1, "untextured rectangle touched the texture interface");
                tick();
                elapsed = elapsed + 1;
            end
            if (!done || fault || busy)
                $fatal(1, "rectangle case %0d failed to retire", case_count);
            if (raster_edges != expected_raster)
                $fatal(1, "rectangle case %0d used %0d raster clocks, expected %0d for %0d samples",
                    case_count, raster_edges, expected_raster, expected_samples);
            paired_issues = paired_issues + expected_samples - expected_raster;
            inspect_tile();
        end
    endtask

    task automatic reset_tile;
        begin
            reset = 1;
            tick();
            reset = 0;
            tick();
        end
    endtask

    task automatic initialize_depth_pattern;
        integer column, row;
        reg [23:0] depth;
        begin
            for (row = 4; row <= 5; row = row + 1)
                for (column = 0; column < 12; column = column + 1) begin
                    case ((column + 3 * (row - 4)) % 12)
                        0, 3: depth = 24'h800001;
                        1, 2: depth = 24'h7fffff;
                        4, 7: depth = 24'h800000;
                        5, 6, 9, 10: depth = 24'h000000;
                        default: depth = 24'hffffff;
                    endcase
                    initialize_pixel(32 * row + column, 16'ha736 ^ (32 * row + column), depth);
                end
        end
    endtask

    integer index, start_parity, span, wait_edges;
    initial begin
        reset_tile();
        for (index = 0; index < 1024; index = index + 1)
            initialize_pixel(index, 16'h315a ^ (index * 13), 24'h800000 ^ (index * 7919));

        // Clip oversized geometry to all widths at the right and bottom of
        // the viewport, including the final pair in the last tile row.
        for (start_parity = 0; start_parity < 2; start_parity = start_parity + 1)
            for (span = 1; span <= 32 - start_parity; span = span + 1)
                submit_rectangle(-18, -6, 650, 505,
                    ORIGIN_X + start_parity, 478, ORIGIN_X + start_parity + span, 500,
                    COLOR | DEPTH, ALWAYS, 24'hc72d08 + span, 24'hf10203 + span);

        initialize_depth_pattern();
        submit_rectangle(608, 452, 620, 454, 0, 0, 640, 480,
            COLOR | TEST | DEPTH, LESS, 24'h0cab88, 24'h800000);
        // Submit immediately after retirement. Including equality in LEQUAL
        // changes the color of lanes that just passed the preceding LESS.
        submit_rectangle(608, 452, 620, 454, 0, 0, 640, 480,
            COLOR | TEST, LEQUAL, 24'hb21c67, 24'h800000);
        // Force write-depth-only through mixed pass/fail and equal lanes.
        initialize_depth_pattern();
        submit_rectangle(608, 452, 620, 454, 0, 0, 640, 480,
            TEST | DEPTH, LEQUAL, 24'hff71a7, 24'h800000);
        submit_rectangle(609, 452, 619, 454, 0, 0, 640, 480,
            COLOR | TEST | DEPTH, ALWAYS, 24'h61bf24, 24'hffffff);
        // Disabling the test permits a farther sample even for LESS.
        submit_rectangle(608, 452, 620, 454, 0, 0, 640, 480,
            COLOR, LESS, 24'h35f080, 24'hffffff);
        submit_rectangle(608, 452, 620, 454, 0, 0, 640, 480,
            DEPTH, LESS, 24'hffff00, 24'h000000);
        submit_rectangle(608, 452, 620, 454, 0, 0, 640, 480,
            TEST, LESS, 24'hadde55, 24'h000000);
        submit_rectangle(608, 452, 620, 454, 610, 452, 610, 480,
            COLOR | DEPTH, ALWAYS, 24'hbeefdf, 24'h123456);

        // Hold reset across the retirement edge of the first pair. The
        // attachment RAM retains existing contents and both writes cancel.
        record_rectangle(610, 450, 612, 451, 0, 0, 640, 480,
            COLOR | DEPTH, ALWAYS, 24'hdedac7, 24'h012456);
        launch();
        wait_edges = 0;
        while (!dut.pending && !fault && !done && wait_edges < 32) begin
            tick();
            wait_edges = wait_edges + 1;
        end
        if (!dut.pending || fault || done)
            $fatal(1, "did not reach the active rectangle retirement stage");
        reset_tile();
        inspect_tile();

        // Invalid present fields must fault before either lane can write.
        record_rectangle(610, 450, 612, 451, 0, 0, 640, 480,
            COLOR | DEPTH, ALWAYS, 24'hd1e2f3, 24'h789456);
        record[19] = 1;
        launch();
        wait_edges = 0;
        while (!fault && !done && wait_edges < 32) begin
            tick();
            wait_edges = wait_edges + 1;
        end
        if (!fault || done)
            $fatal(1, "invalid rectangle prefix was accepted");
        reset_tile();
        inspect_tile();

        // Draw again directly over both preserved lanes after recovery.
        submit_rectangle(610, 450, 612, 451, 0, 0, 640, 480,
            COLOR | DEPTH, ALWAYS, 24'h699bee, 24'h123abc);
        $display("PASS rectangle pairs: cases%0d pixels%0d paired%0d reset1 fault1",
            case_count, checked_pixels, paired_issues);
        $finish;
    end
endmodule
