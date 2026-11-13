// Shared 8 MiB, x32 SDR SDRAM service for the GW2AR QN88 SiP device.
// Every external access is a physical sequential BL8 command. A single native
// 16x32 logical payload slot (two SDPB banks) stages write data or retains a
// read response. Client zero has bounded scanout priority; the other clients
// rotate fairly. No RAM content lives in a host simulation back door.
//
// A transaction of 1..16 words is split into BL8 segments (32-byte aligned
// column blocks). Column commands are pipelined: the next segment's READ or
// WRITE issues eight edges after the previous one when its row is open, so
// read data and write beats stream back to back. A WRITE segment issues as
// soon as all of its words are staged, while the client keeps filling later
// segments. The next segment's PRECHARGE/ACTIVE may issue during a burst.
//
// Commands/DQ are sampled on rising clk edges. WRITE carries beat zero on its
// command edge, then seven data edges. READ beat zero is captured exactly
// CAS_LATENCY rising edges after the READ command the chip establishes its
// output. With sd_clk in phase with clk, the controller captures that OLD DQ
// output on the following rising edge (CAS_LATENCY+1). SDRAM DQM is active high;
// client w_strb is active high byte-enable. Reset does not initialize SDRAM.
// A logic reset masks/drives the remainder of an already accepted WRITE burst
// before restarting the full initialization wait; it does not power-cycle the
// external chip or drop its required data driver in the middle of BL8.
module nano_sdram_service #(
    parameter integer CLIENTS = 6,
    parameter integer POWERUP_CYCLES = 10080,
    parameter integer REFRESH_INTERVAL = 1575,
    parameter integer CAS_LATENCY = 3,
    parameter integer T_RP = 3,
    parameter integer T_RCD = 3,
    parameter integer T_RFC = 9,
    parameter integer T_MRD = 3,
    parameter integer T_WR = 3,
    parameter integer T_RAS = 6,
    parameter integer T_RC = 9,
    parameter integer READ_TO_WRITE = 2,
    parameter integer WRITE_TO_READ = 2,
    parameter integer SCANOUT_PRIORITY_LIMIT = 4
) (
    input wire clk, input wire reset,
    input wire [CLIENTS-1:0] cmd_valid,
    output reg [CLIENTS-1:0] cmd_ready,
    input wire [CLIENTS-1:0] cmd_write,
    input wire [CLIENTS*23-1:0] cmd_addr,
    input wire [CLIENTS*5-1:0] cmd_words,
    input wire [CLIENTS-1:0] w_valid,
    output reg [CLIENTS-1:0] w_ready,
    input wire [CLIENTS*32-1:0] w_data,
    input wire [CLIENTS*4-1:0] w_strb,
    output reg [CLIENTS-1:0] r_valid,
    input wire [CLIENTS-1:0] r_ready,
    output reg [CLIENTS*32-1:0] r_data,
    output reg [CLIENTS-1:0] r_last,
    output reg [CLIENTS-1:0] done,
    output reg [CLIENTS-1:0] error,
    output reg initialized,
    output wire sd_clk,
    output reg sd_cke, output reg sd_cs_n,
    output reg sd_ras_n, output reg sd_cas_n, output reg sd_we_n,
    output reg [1:0] sd_ba, output reg [10:0] sd_addr,
    input wire [31:0] sd_dq_in,
    output reg [31:0] sd_dq_out, output reg sd_dq_oe,
    output reg [3:0] sd_dqm
);
    function integer maximum(input integer a, input integer b);
        maximum = a > b ? a : b;
    endfunction
    function integer count_bits(input integer count);
        count_bits = count <= 1 ? 1 : $clog2(count);
    endfunction
    localparam integer RP_BITS = count_bits(T_RP), RCD_BITS = count_bits(T_RCD),
        RAS_BITS = count_bits(T_RAS), RC_BITS = count_bits(T_RC), WR_BITS = count_bits(T_WR);
    localparam integer WAIT_MAX = maximum(CAS_LATENCY, maximum(T_RP - 1,
        maximum(T_RFC - 1, maximum(T_MRD - 1, T_WR - 1))));
    localparam integer WAIT_BITS = count_bits(WAIT_MAX + 1);
    localparam integer POWER_BITS = count_bits(POWERUP_CYCLES);
    localparam integer TURN_BITS = count_bits(maximum(READ_TO_WRITE, WRITE_TO_READ));
    localparam integer STREAK_BITS = count_bits(SCANOUT_PRIORITY_LIMIT + 1);
    localparam integer OWNER_BITS = (CLIENTS <= 2) ? 1 : $clog2(CLIENTS);
    localparam integer AGE_BITS = $clog2(REFRESH_INTERVAL + 2);
    localparam integer REFRESH_GUARD = 3 * (T_RP + T_RCD + T_RAS + T_RC +
        CAS_LATENCY + 8 + T_WR + READ_TO_WRITE + WRITE_TO_READ + 4) + T_RP + 2;
    localparam [4:0] POWER_WAIT = 0, INIT_PRE = 1, INIT_PRE_WAIT = 2,
        INIT_REF_ONE = 3, INIT_REF_ONE_WAIT = 4, INIT_REF_TWO = 5,
        INIT_REF_TWO_WAIT = 6, INIT_MODE = 7, INIT_MODE_WAIT = 8,
        IDLE = 9, ACTIVE = 10, REFRESH_WAIT = 18,
        REFRESH_PRE_WAIT = 19, REFRESH_COMMAND = 20, REFRESH_RECOVERY = 21,
        CHECK_REQUEST = 22;
    reg [4:0] state /*verilator public_flat_rd*/, refresh_resume;
    reg [POWER_BITS-1:0] power_count;
    reg [WAIT_BITS-1:0] wait_count;
    reg [AGE_BITS-1:0] refresh_age;
    reg refresh_running;
    wire refresh_due = refresh_running &&
        refresh_age >= AGE_BITS'(REFRESH_INTERVAL - REFRESH_GUARD);

    reg [OWNER_BITS-1:0] owner, round_robin;
    reg [STREAK_BITS-1:0] scanout_streak;
    reg request_write;
    reg [22:0] request_address;
    reg [4:0] request_words, fill_index;
    // A validated transaction owns the slot from CHECK until completion.
    // issued counts request words whose column command has been sent; the
    // seg_* registers describe the next segment so no address adder sits on
    // the command path.
    reg txn;
    // Early release. A transaction gives up the command slot once its last
    // column command has issued; its remaining write beats or read response
    // finish in the background (w_* and stream_* below) while the next request
    // is granted, checked, activates its row and stages its data. The payload
    // RAM holds two 16-word slots, one per transaction; a slot stays busy until
    // its transaction's completion event, and a grant waits for a free slot.
    reg front_slot, alloc_slot;
    reg [1:0] slot_busy;
    // The read response stream belongs to one read transaction at a time,
    // from its first READ command to its final handshake; the front may still
    // be that transaction (stream_front) or a later one may already be granted.
    reg read_stream_active, read_output_valid, stream_front;
    reg [OWNER_BITS-1:0] stream_owner;
    reg stream_slot;
    reg [4:0] stream_words, read_index, read_fill;
    reg [4:0] issued;
    reg [22:0] seg_base;
    reg [2:0] seg_prefix;
    reg [3:0] seg_useful;
    wire seg_pending = txn && issued != request_words;
    wire [4:0] remaining_after = request_words - issued - {1'b0,seg_useful};
    wire [3:0] next_useful = remaining_after > 5'd8 ? 4'd8 : remaining_after[3:0];
    wire [3:0] first_room = 4'd8 - {1'b0,request_address[4:2]};

    reg [3:0] bank_open;
    reg [10:0] bank_row [0:3];
    reg [RP_BITS-1:0] rp_wait [0:3];
    reg [RCD_BITS-1:0] rcd_wait [0:3];
    reg [RAS_BITS-1:0] ras_wait [0:3];
    reg [RC_BITS-1:0] rc_wait [0:3];
    reg [WR_BITS-1:0] wr_wait [0:3];
    reg [TURN_BITS-1:0] turnaround;
    // Direction of the last burst: turnaround only separates opposite
    // directions; same-direction bursts may follow each other directly.
    reg last_write_dir;
    // Eight edges between column commands: one BL8 data window each.
    reg [2:0] column_gap;

    // Read capture: a READ issued on edge t returns beat zero at t+CL+1.
    // Column commands are at least eight edges apart, so at most one segment
    // waits (pend_*) while another is captured (cap_*).
    reg [CAS_LATENCY:0] read_pipe;
    reg [2:0] pend_prefix, cap_prefix, cap_beat;
    reg [3:0] pend_useful, cap_useful;
    reg [4:0] pend_index, cap_index;
    reg pend_slot, cap_slot;
    reg cap_active;
    wire cap_start = read_pipe[CAS_LATENCY];
    wire read_capture = cap_start || cap_active;
    wire read_in_flight = (|read_pipe) || cap_active;
    wire [2:0] capture_beat = cap_start ? 3'd0 : cap_beat;
    wire [2:0] capture_prefix = cap_start ? pend_prefix : cap_prefix;
    wire [3:0] capture_useful = cap_start ? pend_useful : cap_useful;
    wire [4:0] capture_index = cap_start ? pend_index : cap_index;
    wire useful_read = read_capture && capture_beat >= capture_prefix &&
        {1'b0, capture_beat} < {1'b0, capture_prefix} + capture_useful;
    wire [4:0] read_buffer_index = capture_index + {2'b0, capture_beat} - {2'b0, capture_prefix};
    wire capture_slot = cap_start ? pend_slot : cap_slot;

    wire [1:0] burst_bank = seg_base[11:10];
    wire [10:0] burst_row = seg_base[22:12];
    wire row_matches = bank_open[burst_bank] && bank_row[burst_bank] == burst_row;
    // A PRECHARGE never truncates an in-flight read burst.
    wire may_precharge = ras_wait[burst_bank] == 0 && wr_wait[burst_bank] == 0 && !read_in_flight;
    wire may_activate = !bank_open[burst_bank] && rp_wait[burst_bank] == 0 && rc_wait[burst_bank] == 0;

    // Write beats. The RAM output register holds the segment's first word
    // (head_loaded) before its WRITE; each useful beat prefetches the next.
    // Configuration-initialized like write_tail, which depends on it. A beat
    // run keeps its own owner and slot, and w_last marks the transaction's
    // final segment, whose last beat is that transaction's completion.
    reg wbeat_active = 1'b0;
    reg head_loaded;
    reg [2:0] wbeat, w_prefix;
    reg [3:0] w_useful;
    reg [4:0] w_index;
    reg [1:0] w_bank;
    reg [OWNER_BITS-1:0] w_owner;
    reg w_slot, w_last;
    wire turn_ok = turnaround == 0 || last_write_dir == request_write;
    // A released transaction may still own the data bus or the payload RAM
    // read port. A WRITE waits for every released read (captures and response
    // stream); a READ waits for released write beats and for an older read's
    // stream, but not for its own transaction's earlier segment.
    wire direction_ok = request_write ? !read_stream_active :
        (!wbeat_active && (!read_stream_active || stream_front));
    wire may_column = row_matches && rcd_wait[burst_bank] == 0 && turn_ok && column_gap == 0 && direction_ok;
    wire seg_filled = {1'b0, fill_index} >= {1'b0, issued} + {2'b0, seg_useful};

    wire command_slot = state == ACTIVE && seg_pending && !refresh_due;
    wire want_precharge = bank_open[burst_bank] && !row_matches && may_precharge;
    wire do_precharge = command_slot && want_precharge;
    wire do_activate = command_slot && !want_precharge && may_activate;
    wire do_column = command_slot && !want_precharge && !may_activate && may_column &&
        (!request_write || (head_loaded && seg_filled));
    wire write_command = do_column && request_write;
    wire read_command = do_column && !request_write;

    wire write_beat_active = write_command || wbeat_active;
    wire [2:0] physical_beat = write_command ? 3'd0 : wbeat;
    wire [2:0] beat_prefix = write_command ? seg_prefix : w_prefix;
    wire [3:0] beat_useful = write_command ? seg_useful : w_useful;
    wire [4:0] beat_index = write_command ? issued : w_index;
    wire beat_slot = write_command ? front_slot : w_slot;
    wire beat_last = write_command ? issued + {1'b0, seg_useful} == request_words : w_last;
    wire useful_write = write_beat_active && physical_beat >= beat_prefix &&
        {1'b0, physical_beat} < {1'b0, beat_prefix} + beat_useful;
    wire last_useful_write = useful_write &&
        {1'b0, physical_beat} + 4'd1 == {1'b0, beat_prefix} + beat_useful;
    wire [4:0] write_buffer_index = beat_index + {2'b0, physical_beat} - {2'b0, beat_prefix};
    wire [4:0] next_head = beat_index + {1'b0, beat_useful};
    // A segment's last useful beat prefetches the same transaction's next head.
    // After a transaction's final segment the beat run may belong to a released
    // transaction: its last useful beat may prefetch the front's first head.
    wire next_head_ready = !beat_last && next_head != request_words && fill_index > next_head;
    wire head_prefetch = txn && request_write && !head_loaded && !read_stream_active &&
        (!write_beat_active || (last_useful_write && beat_last)) &&
        issued != request_words && fill_index > issued;
    wire write_done_event = wbeat_active && wbeat == 7 && w_last;
    // The payload RAM read for write beats, evaluated for both values of
    // write_command from retained registers. write_command (open-row compare
    // and column decision) then only selects between the two, instead of
    // feeding the beat multiplexers, comparisons and index adders. The result
    // equals the beat_* expressions above.
    // write_command: physical beat 0 of the segment just issued.
    wire cmd_useful = seg_prefix == 0 && seg_useful != 0;
    wire cmd_last_useful = seg_prefix == 0 && seg_useful == 1;
    wire cmd_last = issued + {1'b0, seg_useful} == request_words;
    wire [4:0] cmd_head = issued + {1'b0, seg_useful};
    wire cmd_head_ready = !cmd_last && cmd_head != request_words && fill_index > cmd_head;
    wire cmd_prefetch = txn && request_write && !head_loaded && !read_stream_active &&
        cmd_last_useful && cmd_last && issued != request_words && fill_index > issued;
    // Otherwise: the retained beat run, if any.
    wire run_useful = wbeat_active && wbeat >= w_prefix &&
        {1'b0, wbeat} < {1'b0, w_prefix} + w_useful;
    wire run_last_useful = run_useful && {1'b0, wbeat} + 4'd1 == {1'b0, w_prefix} + w_useful;
    wire [4:0] run_head = w_index + {1'b0, w_useful};
    wire run_head_ready = !w_last && run_head != request_words && fill_index > run_head;
    wire run_prefetch = txn && request_write && !head_loaded && !read_stream_active &&
        (!wbeat_active || (run_last_useful && w_last)) && issued != request_words && fill_index > issued;
    reg cmd_re, run_re;
    reg [9:0] cmd_raddr, run_raddr;
    always @* begin
        cmd_re = 0; cmd_raddr = 0; run_re = 0; run_raddr = 0;
        if (cmd_useful && !cmd_last_useful) begin
            cmd_re = 1; cmd_raddr = {4'b0, front_slot, issued - {2'b0, seg_prefix} + 5'd1};
        end else if (cmd_last_useful && cmd_head_ready) begin
            cmd_re = 1; cmd_raddr = {4'b0, front_slot, cmd_head};
        end else if (cmd_prefetch) begin
            cmd_re = 1; cmd_raddr = {4'b0, front_slot, issued};
        end
        if (run_useful && !run_last_useful) begin
            run_re = 1; run_raddr = {4'b0, w_slot, w_index + {2'b0, wbeat} - {2'b0, w_prefix} + 5'd1};
        end else if (run_last_useful && run_head_ready) begin
            run_re = 1; run_raddr = {4'b0, w_slot, run_head};
        end else if (run_prefetch) begin
            run_re = 1; run_raddr = {4'b0, front_slot, issued};
        end
    end
`ifndef SYNTHESIS
    // The split must equal the original beat_* selection on every edge.
    wire split_re = write_command ? cmd_re : run_re;
    wire [9:0] split_raddr = write_command ? cmd_raddr : run_raddr;
    wire expect_re = (useful_write && !last_useful_write) || (last_useful_write && next_head_ready) ||
        head_prefetch;
    wire [9:0] expect_raddr = (useful_write && !last_useful_write) ?
        {4'b0, beat_slot, write_buffer_index + 5'd1} :
        (last_useful_write && next_head_ready) ? {4'b0, beat_slot, next_head} : {4'b0, front_slot, issued};
    always @(posedge clk)
        if (!reset && (split_re != expect_re || (split_re && split_raddr != expect_raddr)))
            $fatal(1, "SDRAM write-beat read address split differs");
`endif
    // This small physically initialized tracker deliberately survives runtime
    // logic reset until a physical WRITE finishes. FPGA configuration sets it
    // to zero; runtime reset must not erase the remaining-beat obligation.
    reg [2:0] write_tail = 3'd0;
    wire draining_reset_tail = write_tail != 0 && !wbeat_active;

    // The native RAM output itself is the elastic read-response register.
    // Holding its read enable low holds valid/data under response backpressure.
    // The stream can continue during periodic refresh, avoiding retention
    // dependence on the consumer's readiness.
    wire read_handshake = read_stream_active && read_output_valid && r_ready[stream_owner];
    wire [4:0] next_read_index = read_index + {4'b0,read_handshake};
    wire read_stream_done = read_handshake && read_index + 1 == stream_words;
    // Read captures and write staging share the payload RAM write port; a
    // capture of a released read holds the front's staging for that edge.
    wire fill_accept = txn && request_write && fill_index != request_words && w_valid[owner] && !useful_read;
    reg ram_we, ram_re;
    reg [9:0] ram_waddr, ram_raddr;
    reg [31:0] ram_wdata;
    wire [31:0] ram_rdata;
    wire [15:0] mask_rdata;
    nano_sdp16 buffer_low (
        .wclk(clk), .wce(1'b1), .we(ram_we), .waddr(ram_waddr), .wdata(ram_wdata[15:0]),
        .rclk(clk), .rce(1'b1), .re(ram_re), .raddr(ram_raddr), .reset_output(reset), .rdata(ram_rdata[15:0]));
    nano_sdp16 buffer_high (
        .wclk(clk), .wce(1'b1), .we(ram_we), .waddr(ram_waddr), .wdata(ram_wdata[31:16]),
        .rclk(clk), .rce(1'b1), .re(ram_re), .raddr(ram_raddr), .reset_output(reset), .rdata(ram_rdata[31:16]));
    // Byte masks follow the same synchronous address/enable as write data.
    // Every staged word is written before its segment can be read for a
    // WRITE; no reset or combinational read of a mask register array is needed.
    nano_sdp16 buffer_masks (
        .wclk(clk), .wce(1'b1),
        .we(fill_accept && !reset && !draining_reset_tail),
        .waddr({4'b0,front_slot,fill_index}), .wdata({12'b0,w_strb[owner*4 +: 4]}),
        // A released write's beats may run while the front is a read.
        .rclk(clk), .rce(1'b1), .re(ram_re),
        .raddr(ram_raddr), .reset_output(reset), .rdata(mask_rdata));

    // The round-robin cursor is initialized to one and advances only after a
    // foreground grant, wrapping CLIENTS-1 back to one. Decode that bounded
    // cursor before selecting requests. Masked and wraparound priorities are
    // computed in parallel; no integer candidate arithmetic or dynamic request
    // indexing is on the ready path. Keep the grant one-hot through cmd_ready,
    // encoding it only for the retained owner and selected command payload.
    wire winner_valid = |cmd_valid;
    wire [OWNER_BITS-1:0] winner;
    wire [CLIENTS-1:0] foreground_requested, cursor_decode, foreground_eligible;
    wire [CLIENTS-1:0] arbitration_grant;
    wire scanout_first = cmd_valid[0] &&
        scanout_streak < STREAK_BITS'(SCANOUT_PRIORITY_LIMIT);
    wire foreground_any = |foreground_requested;
    wire eligible_any = |foreground_eligible;
    assign foreground_requested[0] = 1'b0;
    assign cursor_decode[0] = 1'b0;
    assign foreground_eligible[0] = 1'b0;
    assign arbitration_grant[0] = cmd_valid[0] && (scanout_first || !foreground_any);
    generate for (genvar client = 1; client < CLIENTS; client = client + 1) begin: arbiter_clients
        assign foreground_requested[client] = cmd_valid[client];
        assign cursor_decode[client] = round_robin == OWNER_BITS'(client);
        assign foreground_eligible[client] = cmd_valid[client] && (|cursor_decode[client:1]);
        assign arbitration_grant[client] = !scanout_first &&
            ((foreground_eligible[client] && !(|foreground_eligible[client-1:0])) ||
             (!eligible_any && foreground_requested[client] && !(|foreground_requested[client-1:0])));
    end endgenerate
    generate for (genvar bit_index = 0; bit_index < OWNER_BITS; bit_index = bit_index + 1) begin: arbiter_owner
        wire [CLIENTS-1:0] encoded_sources;
        for (genvar client = 0; client < CLIENTS; client = client + 1) begin: clients
            assign encoded_sources[client] = arbitration_grant[client] && ((client & (1 << bit_index)) != 0);
        end
        assign winner[bit_index] = |encoded_sources;
    end endgenerate

    reg refresh_safe;
    integer bank;
    always @* begin
        refresh_safe = turnaround == 0 && !read_in_flight && !write_beat_active;
        for (integer k = 0; k < 4; k = k + 1)
            if (ras_wait[k] != 0 || wr_wait[k] != 0 || rc_wait[k] != 0 || rp_wait[k] != 0)
                refresh_safe = 0;
    end

    wire [22:0] winner_address = cmd_addr[winner*23 +: 23];
    wire [4:0] winner_words = cmd_words[winner*5 +: 5];
    // Unregistered reference for the arbiter testbench; CHECK_REQUEST makes
    // the actual decision from retained fields, so this logic is unused.
    wire [5:0] winner_last_line_end = {2'b0, winner_address[5:2]} + {1'b0, winner_words};
    wire winner_past_end = (&winner_address[22:6]) && winner_last_line_end > 6'd16;
    wire winner_invalid = winner_address[1:0] != 0 || winner_words == 0 ||
        winner_words > 16 || winner_past_end;

    // Capture command fields on the handshake before evaluating their range.
    // This stage consumes only the retained request, never live client inputs.
    // With an aligned address and 1..16 words, only the final 64-byte line
    // can cross the 8 MiB boundary. Keep malformed alignment/count rejection,
    // but check that last line with a six-bit sum instead of carrying through
    // the complete byte address. Ending exactly at 8 MiB remains valid.
    wire [5:0] request_last_line_end = {2'b0, request_address[5:2]} + {1'b0, request_words};
    wire request_past_end = (&request_address[22:6]) && request_last_line_end > 6'd16;
    wire request_invalid = request_address[1:0] != 0 || request_words == 0 ||
        request_words > 16 || request_past_end;

    assign sd_clk = clk;
    always @* begin
        cmd_ready = 0; w_ready = 0; r_valid = 0; r_data = 0; r_last = 0;
        if (state == IDLE && initialized && !refresh_due && !slot_busy[alloc_slot]) cmd_ready = arbitration_grant;
        if (txn && request_write && fill_index != request_words && !useful_read) w_ready[owner] = 1;
        if (read_stream_active && read_output_valid) begin
            r_valid[stream_owner] = 1;
            r_data[stream_owner*32 +: 32] = ram_rdata;
            r_last[stream_owner] = read_index + 1 == stream_words;
        end
        ram_we = 0; ram_re = 0; ram_waddr = 0; ram_raddr = 0; ram_wdata = 0;
        if (fill_accept) begin
            ram_we = 1; ram_waddr = {4'b0, front_slot, fill_index}; ram_wdata = w_data[owner*32 +: 32];
        end
        if (useful_read) begin
            ram_we = 1; ram_waddr = {4'b0, capture_slot, read_buffer_index}; ram_wdata = sd_dq_in;
        end
        if (write_command) begin ram_re = cmd_re; ram_raddr = cmd_raddr; end
        else begin ram_re = run_re; ram_raddr = run_raddr; end
        // read_fill counts the stream's captured words. Never read the address
        // currently being written by SDRAM.
        if (read_stream_active && (!read_output_valid || r_ready[stream_owner]) &&
                next_read_index < read_fill) begin
            ram_re = 1; ram_raddr = {4'b0, stream_slot, next_read_index};
        end

        sd_cke = 1; sd_cs_n = 0; sd_ras_n = 1; sd_cas_n = 1; sd_we_n = 1;
        sd_ba = burst_bank; sd_addr = 0; sd_dq_out = 0; sd_dq_oe = 0; sd_dqm = 0;
        if (state == INIT_PRE || (state == REFRESH_WAIT && refresh_safe)) begin
            sd_ras_n = 0; sd_we_n = 0; sd_addr[10] = 1; sd_ba = 0;
        end else if (state == INIT_REF_ONE || state == INIT_REF_TWO || state == REFRESH_COMMAND) begin
            sd_ras_n = 0; sd_cas_n = 0; sd_ba = 0;
        end else if (state == INIT_MODE) begin
            sd_ras_n = 0; sd_cas_n = 0; sd_we_n = 0;
            sd_addr = 11'((CAS_LATENCY << 4) | 3); sd_ba = 0;
        end else if (do_precharge) begin
            sd_ras_n = 0; sd_we_n = 0;
        end else if (do_activate) begin
            sd_ras_n = 0; sd_addr = burst_row;
        end else if (do_column) begin
            sd_cas_n = 0; sd_we_n = !request_write;
            sd_addr = {3'b0, seg_base[9:2]}; // auto-precharge disabled
        end
        if (write_beat_active) begin
            sd_dq_oe = 1;
            sd_dqm = useful_write ? ~mask_rdata[3:0] : 4'hf;
            sd_dq_out = useful_write ? ram_rdata : 0;
        end
        if (reset || draining_reset_tail) begin
            cmd_ready = 0; w_ready = 0; r_valid = 0; r_last = 0;
            ram_we = 0; ram_re = 0;
            sd_ras_n = 1; sd_cas_n = 1; sd_we_n = 1;
            sd_dq_oe = write_tail != 0; sd_dq_out = 0;
            sd_dqm = write_tail != 0 ? 4'hf : 4'h0;
        end
    end

    always @(posedge clk) begin
        if (write_tail != 0) write_tail <= write_tail - 1'b1;
        else if (!reset && write_command) write_tail <= 3'd7;
    end

    always @(posedge clk) begin
        done <= 0; error <= 0;
        if (reset) begin
            state <= POWER_WAIT; refresh_resume <= IDLE;
            power_count <= 0; wait_count <= 0; refresh_age <= 0; refresh_running <= 0;
            initialized <= 0; owner <= 0; round_robin <= 1; scanout_streak <= 0;
            request_write <= 0; request_address <= 0; request_words <= 0;
            fill_index <= 0; txn <= 0; issued <= 0;
            seg_base <= 0; seg_prefix <= 0; seg_useful <= 0;
            bank_open <= 0; turnaround <= 0; last_write_dir <= 0; column_gap <= 0;
            read_pipe <= 0; cap_active <= 0; cap_beat <= 0; cap_prefix <= 0; cap_useful <= 0; cap_index <= 0;
            pend_prefix <= 0; pend_useful <= 0; pend_index <= 0;
            wbeat_active <= 0; wbeat <= 0; w_prefix <= 0; w_useful <= 0; w_index <= 0; w_bank <= 0;
            w_owner <= 0; w_slot <= 0; w_last <= 0; pend_slot <= 0; cap_slot <= 0;
            head_loaded <= 0;
            front_slot <= 0; alloc_slot <= 0; slot_busy <= 0;
            read_stream_active <= 0; read_output_valid <= 0; read_index <= 0; read_fill <= 0;
            stream_front <= 0; stream_owner <= 0; stream_slot <= 0; stream_words <= 0;
            for (bank = 0; bank < 4; bank = bank + 1) begin
                bank_row[bank] <= 0; rp_wait[bank] <= 0; rcd_wait[bank] <= 0;
                ras_wait[bank] <= 0; rc_wait[bank] <= 0; wr_wait[bank] <= 0;
            end
        end else begin
            if (refresh_running && refresh_age < AGE_BITS'(REFRESH_INTERVAL + 1)) refresh_age <= refresh_age + 1'b1;
            if (turnaround != 0) turnaround <= turnaround - 1'b1;
            if (column_gap != 0) column_gap <= column_gap - 1'b1;
            for (bank = 0; bank < 4; bank = bank + 1) begin
                if (rp_wait[bank] != 0) rp_wait[bank] <= rp_wait[bank] - 1'b1;
                if (rcd_wait[bank] != 0) rcd_wait[bank] <= rcd_wait[bank] - 1'b1;
                if (ras_wait[bank] != 0) ras_wait[bank] <= ras_wait[bank] - 1'b1;
                if (rc_wait[bank] != 0) rc_wait[bank] <= rc_wait[bank] - 1'b1;
                if (wr_wait[bank] != 0) wr_wait[bank] <= wr_wait[bank] - 1'b1;
            end
            read_pipe <= {read_pipe[CAS_LATENCY-1:0], read_command};
            if (cap_start) begin
                cap_active <= 1; cap_beat <= 1; cap_slot <= pend_slot;
                cap_prefix <= pend_prefix; cap_useful <= pend_useful; cap_index <= pend_index;
            end else if (cap_active) begin
                if (cap_beat == 7) begin
                    cap_active <= 0; turnaround <= TURN_BITS'(READ_TO_WRITE - 1);
                end else cap_beat <= cap_beat + 1'b1;
            end
            if (wbeat_active) begin
                wr_wait[w_bank] <= WR_BITS'(T_WR - 1);
                if (wbeat == 7) begin
                    wbeat_active <= 0; turnaround <= TURN_BITS'(WRITE_TO_READ - 1);
                end else wbeat <= wbeat + 1'b1;
            end
            if (fill_accept) fill_index <= fill_index + 1'b1;
            if (useful_read) read_fill <= read_fill + 1'b1;
            if (last_useful_write && !beat_last) head_loaded <= next_head_ready;
            else if (write_command) head_loaded <= 0;
            else if (head_prefetch) head_loaded <= 1;
            // Completion events: the final beat of a write transaction's last
            // segment, or the final response handshake of a read stream. Each
            // frees that transaction's payload slot.
            if (write_done_event) begin
                done[w_owner] <= 1; slot_busy[w_slot] <= 0;
            end
            if (read_stream_active) begin
                // While a stream is active only it reads the payload RAM: write
                // beats and head prefetches wait for it (direction_ok).
                if (ram_re) read_output_valid <= 1;
                else if (read_handshake) read_output_valid <= 0;
                if (read_handshake) begin
                    read_index <= read_index + 1'b1;
                    if (read_stream_done) begin
                        read_stream_active <= 0; read_output_valid <= 0; stream_front <= 0;
                        done[stream_owner] <= 1; slot_busy[stream_slot] <= 0;
                    end
                end
            end
            case (state)
                POWER_WAIT: if (write_tail != 0) power_count <= 0;
                            else if (power_count == POWER_BITS'(POWERUP_CYCLES - 1)) state <= INIT_PRE;
                            else power_count <= power_count + 1'b1;
                INIT_PRE: begin wait_count <= WAIT_BITS'(T_RP - 1); state <= INIT_PRE_WAIT; end
                INIT_PRE_WAIT: if (wait_count == 0) state <= INIT_REF_ONE; else wait_count <= wait_count - 1'b1;
                INIT_REF_ONE: begin
                    refresh_age <= 0; refresh_running <= 1;
                    wait_count <= WAIT_BITS'(T_RFC - 1); state <= INIT_REF_ONE_WAIT;
                end
                INIT_REF_ONE_WAIT: if (wait_count == 0) state <= INIT_REF_TWO; else wait_count <= wait_count - 1'b1;
                INIT_REF_TWO: begin refresh_age <= 0; wait_count <= WAIT_BITS'(T_RFC - 1); state <= INIT_REF_TWO_WAIT; end
                INIT_REF_TWO_WAIT: if (wait_count == 0) state <= INIT_MODE; else wait_count <= wait_count - 1'b1;
                INIT_MODE: begin wait_count <= WAIT_BITS'(T_MRD - 1); state <= INIT_MODE_WAIT; end
                INIT_MODE_WAIT: if (wait_count == 0) begin initialized <= 1; state <= IDLE; end
                               else wait_count <= wait_count - 1'b1;
                IDLE: begin
                    if (refresh_due) begin refresh_resume <= IDLE; state <= REFRESH_WAIT; end
                    else if (winner_valid) begin
                        if (winner == 0) begin
                            if (scanout_streak < STREAK_BITS'(SCANOUT_PRIORITY_LIMIT)) scanout_streak <= scanout_streak + 1'b1;
                        end else begin
                            scanout_streak <= 0;
                            round_robin <= winner == OWNER_BITS'(CLIENTS - 1) ? 1 : winner + 1'b1;
                        end
                        owner <= winner; request_address <= winner_address;
                        request_words <= winner_words; request_write <= cmd_write[winner];
                        fill_index <= 0;
                        // An earlier transaction's response stream continues.
                        state <= CHECK_REQUEST;
                    end
                end
                CHECK_REQUEST: begin
                    if (refresh_due) begin refresh_resume <= CHECK_REQUEST; state <= REFRESH_WAIT; end
                    else if (request_invalid) begin done[owner] <= 1; error[owner] <= 1; state <= IDLE; end
                    else begin
                        txn <= 1; issued <= 0; head_loaded <= 0;
                        front_slot <= alloc_slot; alloc_slot <= !alloc_slot;
                        slot_busy[alloc_slot] <= 1;
                        seg_base <= {request_address[22:5], 5'b0};
                        seg_prefix <= request_address[4:2];
                        seg_useful <= request_words < {1'b0, first_room} ? request_words[3:0] : first_room;
                        // A read's response stream starts with its first READ.
                        state <= ACTIVE;
                    end
                end
                ACTIVE: begin
                    // Refresh only between bursts; staging and the read
                    // response stream continue while it runs.
                    if (refresh_due && !read_in_flight && !write_beat_active) begin
                        refresh_resume <= ACTIVE; state <= REFRESH_WAIT;
                    end else begin
                        if (do_precharge) begin
                            bank_open[burst_bank] <= 0; rp_wait[burst_bank] <= RP_BITS'(T_RP - 1);
                        end
                        if (do_activate) begin
                            bank_open[burst_bank] <= 1; bank_row[burst_bank] <= burst_row;
                            rcd_wait[burst_bank] <= RCD_BITS'(T_RCD - 1);
                            ras_wait[burst_bank] <= RAS_BITS'(T_RAS - 1); rc_wait[burst_bank] <= RC_BITS'(T_RC - 1);
                        end
                        if (do_column) begin
                            column_gap <= 3'd7; last_write_dir <= request_write;
                            issued <= issued + {1'b0, seg_useful};
                            seg_base <= seg_base + 23'd32; seg_prefix <= 0; seg_useful <= next_useful;
                            if (request_write) begin
                                wbeat_active <= 1; wbeat <= 1; w_bank <= burst_bank;
                                w_prefix <= seg_prefix; w_useful <= seg_useful; w_index <= issued;
                                w_owner <= owner; w_slot <= front_slot; w_last <= beat_last;
                                wr_wait[burst_bank] <= WR_BITS'(T_WR - 1);
                            end else begin
                                pend_prefix <= seg_prefix; pend_useful <= seg_useful; pend_index <= issued;
                                pend_slot <= front_slot;
                                if (!read_stream_active) begin
                                    read_stream_active <= 1; read_output_valid <= 0; read_index <= 0;
                                    read_fill <= 0; stream_front <= 1; stream_owner <= owner;
                                    stream_slot <= front_slot; stream_words <= request_words;
                                end
                            end
                        end
                        // Release the command slot once every column command
                        // has issued. The write still retires with its last
                        // physical beat (tWR is enforced per bank before any
                        // PRECHARGE), a read after its final capture and
                        // handshake: see the completion events above.
                        if (txn && issued == request_words) begin
                            txn <= 0; stream_front <= 0; state <= IDLE;
                        end
                    end
                end
                REFRESH_WAIT: if (refresh_safe) begin
                    bank_open <= 0;
                    for (bank = 0; bank < 4; bank = bank + 1) rp_wait[bank] <= RP_BITS'(T_RP - 1);
                    wait_count <= WAIT_BITS'(T_RP - 1); state <= REFRESH_PRE_WAIT;
                end
                REFRESH_PRE_WAIT: if (wait_count == 0) state <= REFRESH_COMMAND; else wait_count <= wait_count - 1'b1;
                REFRESH_COMMAND: begin refresh_age <= 0; wait_count <= WAIT_BITS'(T_RFC - 1); state <= REFRESH_RECOVERY; end
                REFRESH_RECOVERY: if (wait_count == 0) state <= refresh_resume; else wait_count <= wait_count - 1'b1;
                default: state <= POWER_WAIT;
            endcase
        end
    end

`ifndef SYNTHESIS
    initial begin
        if (POWERUP_CYCLES < 1) $fatal(1, "nano_sdram_service requires a positive power-up interval");
        if (CLIENTS < 2 || CLIENTS > 16) $fatal(1, "nano_sdram_service CLIENTS must be 2..16");
        if (CAS_LATENCY < 2 || CAS_LATENCY > 3) $fatal(1, "nano_sdram_service supports CL2/CL3");
        if (REFRESH_INTERVAL <= REFRESH_GUARD + T_RFC + T_RP)
            $fatal(1, "nano_sdram_service refresh interval leaves no bounded service window");
        if (T_RP < 1 || T_RCD < 1 || T_RFC < 1 || T_MRD < 1 || T_WR < 1 ||
            T_RAS < 1 || T_RC < T_RAS + T_RP || READ_TO_WRITE < 1 || WRITE_TO_READ < 1)
            $fatal(1, "nano_sdram_service timing parameters are invalid");
        if (T_RP > 255 || T_RCD > 255 || T_RFC > 255 || T_MRD > 255 || T_WR > 255 ||
            T_RAS > 255 || T_RC > 255 || READ_TO_WRITE > 255 || WRITE_TO_READ > 255 ||
            SCANOUT_PRIORITY_LIMIT < 1 || SCANOUT_PRIORITY_LIMIT > 255)
            $fatal(1, "nano_sdram_service timing/control exceeds its bounded counters");
    end
`endif
endmodule
