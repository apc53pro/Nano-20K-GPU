`timescale 1ns/1ps
// Compare the implemented one-hot selector with the former rotating-index
// selector over every reachable cursor, request mask and streak value. Clocks
// stay stopped: this test isolates grant/ready combinational semantics; the
// separate SDRAM pin regression checks sequential service and fairness.
module sdram_arbiter_tb;
    reg clk = 0, reset = 0;
    reg [5:0] cmd_valid = 0, cmd_write = 0;
    reg [137:0] cmd_addr = 0;
    reg [29:0] cmd_words = 0;
    wire [5:0] cmd_ready;
    nano_sdram_service dut (
        .clk(clk), .reset(reset), .cmd_valid(cmd_valid), .cmd_ready(cmd_ready),
        .cmd_write(cmd_write), .cmd_addr(cmd_addr), .cmd_words(cmd_words),
        .w_valid(6'd0), .w_data(192'd0), .w_strb(24'd0), .r_ready(6'd0),
        .sd_dq_in(32'd0)
    );

    function automatic integer legacy_winner(input [5:0] requests, input integer cursor, input integer streak);
        integer attempt, candidate;
        begin
            legacy_winner = -1;
            if (requests[0] && streak < 4) legacy_winner = 0;
            else begin
                for (attempt = 0; attempt < 5; attempt = attempt + 1) begin
                    candidate = cursor + attempt;
                    if (candidate >= 6) candidate = candidate - 5;
                    if (legacy_winner < 0 && requests[candidate]) legacy_winner = candidate;
                end
                if (legacy_winner < 0 && requests[0]) legacy_winner = 0;
            end
        end
    endfunction

    integer cursor, streak, mask, eligibility, client, metadata_kind;
    integer address_group, offset, word_count, boundary_cases = 0;
    integer expected_winner, cases = 0;
    reg [5:0] expected_grant, expected_ready;
    reg [22:0] expected_address;
    reg [4:0] expected_words;
    reg [23:0] expected_end;
    reg expected_invalid;
    initial begin
        dut.read_stream_active = 0;
        dut.read_output_valid = 0;
        dut.owner = 0;
        for (cursor = 1; cursor <= 5; cursor = cursor + 1)
            for (streak = 0; streak < 8; streak = streak + 1)
                for (mask = 0; mask < 64; mask = mask + 1)
                    for (eligibility = 0; eligibility < 6; eligibility = eligibility + 1) begin
                        cmd_valid = mask[5:0];
                        dut.round_robin = cursor[2:0];
                        dut.scanout_streak = streak[2:0];
                        dut.state = 9; // IDLE
                        dut.initialized = 1;
                        dut.refresh_running = 0;
                        dut.refresh_age = 0;
                        dut.write_tail = 0;
                        reset = 0;
                        case (eligibility)
                            1: dut.state = 10; // FILL, no command acceptance
                            2: dut.initialized = 0;
                            3: begin dut.refresh_running = 1; dut.refresh_age = 1575; end
                            4: reset = 1;
                            5: dut.write_tail = 1; // Physical WRITE tail must drain
                        endcase
                        // Include invalid metadata without changing the
                        // selected grant or silently filtering its handshake.
                        for (client = 0; client < 6; client = client + 1) begin
                            cmd_write[client] = client[0];
                            cmd_addr[client*23 +:23] = 23'h258000 + 23'(client*64);
                            cmd_words[client*5 +:5] = 5'(client+1);
                            metadata_kind = (client + mask + cursor + streak) % 5;
                            case (metadata_kind)
                                1: cmd_addr[client*23 +:23] = 23'h258001;
                                2: begin cmd_addr[client*23 +:23] = 23'h7ffffc; cmd_words[client*5 +:5] = 2; end
                                3: cmd_words[client*5 +:5] = 0;
                                4: cmd_words[client*5 +:5] = 17;
                            endcase
                        end
                        expected_winner = legacy_winner(cmd_valid,cursor,streak);
                        expected_grant = 0;
                        if (expected_winner >= 0) expected_grant[expected_winner] = 1;
                        expected_ready = eligibility == 0 ? expected_grant : 6'd0;
                        #1;
                        if (dut.winner_valid !== (expected_winner >= 0) ||
                            dut.winner !== (expected_winner >= 0 ? 3'(expected_winner) : 3'd0) ||
                            dut.arbitration_grant !== expected_grant || cmd_ready !== expected_ready)
                            $fatal(1,"arbiter mismatch cursor=%0d streak=%0d mask=%0h eligibility=%0d winner=%0d ready=%0h expected=%0h",
                                cursor,streak,mask,eligibility,dut.winner,cmd_ready,expected_ready);
                        expected_address = cmd_addr[(expected_winner >= 0 ? expected_winner : 0)*23 +:23];
                        expected_words = cmd_words[(expected_winner >= 0 ? expected_winner : 0)*5 +:5];
                        expected_end = {1'b0,expected_address} + {17'd0,expected_words,2'd0};
                        expected_invalid = expected_address[1:0] != 0 || expected_words == 0 ||
                            expected_words > 16 || expected_end > 24'h800000;
                        if (dut.winner_address !== expected_address || dut.winner_words !== expected_words ||
                            dut.winner_invalid !== expected_invalid)
                            $fatal(1,"arbiter selected-command validation mismatch");
                        cases = cases + 1;
                    end
        $display("PASS %0d exhaustive SDRAM arbiter grant/ready/metadata cases",cases);
        // Exercise both sides of the final 64-byte-line boundary, every
        // alignment and all raw five-bit counts. The reference retains the
        // original full-width byte-end equation, independently of the DUT's
        // narrowed last-line arithmetic. Repeat at low addresses to reject
        // accidental matching of only the low address bits.
        cmd_valid = 6'd1;
        dut.round_robin = 3'd1;
        dut.scanout_streak = 0;
        dut.state = 9;
        dut.initialized = 1;
        dut.refresh_running = 0;
        dut.refresh_age = 0;
        dut.write_tail = 0;
        reset = 0;
        for (address_group = 0; address_group < 2; address_group = address_group + 1)
            for (offset = 0; offset < 128; offset = offset + 1)
                for (word_count = 0; word_count < 32; word_count = word_count + 1) begin
                    expected_address = address_group == 0 ? 23'(offset) : 23'h7fff80 + 23'(offset);
                    expected_words = 5'(word_count);
                    cmd_addr[0 +:23] = expected_address;
                    cmd_words[0 +:5] = expected_words;
                    expected_end = {1'b0,expected_address} + {17'd0,expected_words,2'd0};
                    expected_invalid = expected_address[1:0] != 0 || expected_words == 0 ||
                        expected_words > 16 || expected_end > 24'h800000;
                    #1;
                    if (cmd_ready !== 6'd1 || dut.winner_invalid !== expected_invalid)
                        $fatal(1,"SDRAM extent mismatch address=%0h words=%0d invalid=%0b expected=%0b",
                            expected_address,expected_words,dut.winner_invalid,expected_invalid);
                    boundary_cases = boundary_cases + 1;
                end
        $display("PASS %0d SDRAM extent boundary/alignment/raw-count cases",boundary_cases);
        $finish;
    end
endmodule
