// Shared 8 MiB, x32 SDR SDRAM service for the GW2AR QN88 SiP device.
// Every external access is a physical sequential BL8 command. A single native
// 16x32 logical payload slot (two SDPB banks) stages an unpausable write burst
// or retains a completed read response. Client zero has bounded scanout
// priority; the other clients rotate fairly. No RAM content lives in a host
// simulation back door.
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
        IDLE = 9, FILL = 10, CHOOSE = 11, PREPARE = 12,
        READ_WAIT = 13, READ_DATA = 14, WRITE_DATA = 15,
        WRITE_RECOVERY = 16, DRAIN = 17, REFRESH_WAIT = 18,
        REFRESH_PRE_WAIT = 19, REFRESH_COMMAND = 20, REFRESH_RECOVERY = 21,
        CHECK_REQUEST = 22;
    reg [4:0] state, refresh_resume;
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
    reg [4:0] request_words, fill_index, processed;
    reg [22:0] burst_base;
    reg [2:0] burst_prefix, burst_beat;
    reg [4:0] burst_useful;

    reg [3:0] bank_open;
    reg [10:0] bank_row [0:3];
    reg [RP_BITS-1:0] rp_wait [0:3];
    reg [RCD_BITS-1:0] rcd_wait [0:3];
    reg [RAS_BITS-1:0] ras_wait [0:3];
    reg [RC_BITS-1:0] rc_wait [0:3];
    reg [WR_BITS-1:0] wr_wait [0:3];
    reg [TURN_BITS-1:0] turnaround;
    wire [1:0] burst_bank = burst_base[11:10];
    wire [10:0] burst_row = burst_base[22:12];
    wire row_matches = bank_open[burst_bank] && bank_row[burst_bank] == burst_row;
    wire may_precharge = ras_wait[burst_bank] == 0 && wr_wait[burst_bank] == 0;
    wire may_activate = !bank_open[burst_bank] && rp_wait[burst_bank] == 0 && rc_wait[burst_bank] == 0;
    wire may_column = row_matches && rcd_wait[burst_bank] == 0 && turnaround == 0;

    // The native RAM output itself is the elastic read-response register.
    // Holding its read enable low holds valid/data under response backpressure.
    // The stream can continue during periodic refresh, avoiding retention
    // dependence on the consumer's readiness.
    reg read_stream_active, read_output_valid;
    reg [4:0] read_index;
    wire read_handshake = read_stream_active && read_output_valid && r_ready[owner];
    wire [4:0] next_read_index = read_index + {4'b0,read_handshake};
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
    // Every accepted write fills all useful entries before CHOOSE reads them;
    // no reset or combinational read of a mask register array is required.
    nano_sdp16 buffer_masks (
        .wclk(clk), .wce(1'b1),
        .we(state == FILL && !refresh_due && w_valid[owner] && !reset),
        .waddr({5'b0,fill_index}), .wdata({12'b0,w_strb[owner*4 +: 4]}),
        .rclk(clk), .rce(1'b1), .re(ram_re && request_write),
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
        refresh_safe = turnaround == 0;
        for (integer k = 0; k < 4; k = k + 1)
            if (ras_wait[k] != 0 || wr_wait[k] != 0 || rc_wait[k] != 0 || rp_wait[k] != 0)
                refresh_safe = 0;
    end

    wire read_capture = state == READ_DATA || (state == READ_WAIT && wait_count == 0);
    wire write_command = state == PREPARE && !refresh_due && may_column && request_write;
    wire write_beat_active = write_command || state == WRITE_DATA;
    // This small physically initialized tracker deliberately survives runtime
    // logic reset until a physical WRITE finishes. FPGA configuration sets it
    // to zero; runtime reset must not erase the remaining-beat obligation.
    reg [2:0] write_tail = 3'd0;
    wire draining_reset_tail = write_tail != 0 && state != WRITE_DATA;
    wire [2:0] physical_beat = write_command ? 3'd0 : burst_beat;
    wire useful_read = read_capture && burst_beat >= burst_prefix &&
        {2'b0, burst_beat} < {2'b0, burst_prefix} + burst_useful;
    wire useful_write = write_beat_active && physical_beat >= burst_prefix &&
        {2'b0, physical_beat} < {2'b0, burst_prefix} + burst_useful;
    wire [4:0] read_buffer_index = processed + {2'b0, burst_beat} - {2'b0, burst_prefix};
    wire [4:0] write_buffer_index = processed + {2'b0, physical_beat} - {2'b0, burst_prefix};
    wire [22:0] next_address = request_address + {16'b0, processed, 2'b0};
    wire [4:0] segment_room = 5'd8 - {2'b0, next_address[4:2]};
    wire [4:0] remaining_words = request_words - processed;
    wire [22:0] winner_address = cmd_addr[winner*23 +: 23];
    wire [4:0] winner_words = cmd_words[winner*5 +: 5];
    // With an aligned address and 1..16 words, only the final 64-byte line
    // can cross the 8 MiB boundary. Keep malformed alignment/count rejection,
    // but check that last line with a six-bit sum instead of carrying through
    // the complete byte address. Ending exactly at 8 MiB remains valid.
    wire [5:0] winner_last_line_end = {2'b0, winner_address[5:2]} + {1'b0, winner_words};
    wire winner_past_end = (&winner_address[22:6]) && winner_last_line_end > 6'd16;
    wire winner_invalid = winner_address[1:0] != 0 || winner_words == 0 ||
        winner_words > 16 || winner_past_end;

    // Capture command fields on the handshake before evaluating their range.
    // This stage consumes only the retained request, never live client inputs.
    wire [5:0] request_last_line_end = {2'b0, request_address[5:2]} + {1'b0, request_words};
    wire request_past_end = (&request_address[22:6]) && request_last_line_end > 6'd16;
    wire request_invalid = request_address[1:0] != 0 || request_words == 0 ||
        request_words > 16 || request_past_end;

    assign sd_clk = clk;
    always @* begin
        cmd_ready = 0; w_ready = 0; r_valid = 0; r_data = 0; r_last = 0;
        if (state == IDLE && initialized && !refresh_due) cmd_ready = arbitration_grant;
        if (state == FILL && !refresh_due) w_ready[owner] = 1;
        if (read_stream_active && read_output_valid) begin
            r_valid[owner] = 1;
            r_data[owner*32 +: 32] = ram_rdata;
            r_last[owner] = read_index + 1 == request_words;
        end
        ram_we = 0; ram_re = 0; ram_waddr = 0; ram_raddr = 0; ram_wdata = 0;
        if (state == FILL && !refresh_due && w_valid[owner]) begin
            ram_we = 1; ram_waddr = {5'b0, fill_index}; ram_wdata = w_data[owner*32 +: 32];
        end
        if (useful_read) begin
            ram_we = 1; ram_waddr = {5'b0, read_buffer_index}; ram_wdata = sd_dq_in;
        end
        if (state == CHOOSE && !refresh_due && request_write) begin
            ram_re = 1; ram_raddr = {5'b0, processed};
        end
        if (useful_write && {2'b0, physical_beat} + 1 < {2'b0, burst_prefix} + burst_useful) begin
            ram_re = 1; ram_raddr = {5'b0, write_buffer_index + 5'd1};
        end
        // fill_index counts captured read words as well as accepted write
        // words. Never read the address currently being written by SDRAM.
        if (read_stream_active && (!read_output_valid || r_ready[owner]) &&
                next_read_index < fill_index) begin
            ram_re = 1; ram_raddr = {5'b0,next_read_index};
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
        end else if (state == PREPARE && !refresh_due) begin
            if (bank_open[burst_bank] && !row_matches && may_precharge) begin
                sd_ras_n = 0; sd_we_n = 0;
            end else if (may_activate) begin
                sd_ras_n = 0; sd_addr = burst_row;
            end else if (may_column) begin
                sd_cas_n = 0; sd_we_n = !request_write;
                sd_addr = {3'b0, burst_base[9:2]}; // auto-precharge disabled
            end
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
            fill_index <= 0; processed <= 0; burst_base <= 0;
            burst_prefix <= 0; burst_beat <= 0; burst_useful <= 0;
            bank_open <= 0; turnaround <= 0;
            read_stream_active <= 0; read_output_valid <= 0; read_index <= 0;
            for (bank = 0; bank < 4; bank = bank + 1) begin
                bank_row[bank] <= 0; rp_wait[bank] <= 0; rcd_wait[bank] <= 0;
                ras_wait[bank] <= 0; rc_wait[bank] <= 0; wr_wait[bank] <= 0;
            end
        end else begin
            if (refresh_running && refresh_age < AGE_BITS'(REFRESH_INTERVAL + 1)) refresh_age <= refresh_age + 1'b1;
            if (turnaround != 0) turnaround <= turnaround - 1'b1;
            for (bank = 0; bank < 4; bank = bank + 1) begin
                if (rp_wait[bank] != 0) rp_wait[bank] <= rp_wait[bank] - 1'b1;
                if (rcd_wait[bank] != 0) rcd_wait[bank] <= rcd_wait[bank] - 1'b1;
                if (ras_wait[bank] != 0) ras_wait[bank] <= ras_wait[bank] - 1'b1;
                if (rc_wait[bank] != 0) rc_wait[bank] <= rc_wait[bank] - 1'b1;
                if (wr_wait[bank] != 0) wr_wait[bank] <= wr_wait[bank] - 1'b1;
            end
            if (useful_read) fill_index <= fill_index + 1'b1;
            if (read_stream_active) begin
                if (ram_re) read_output_valid <= 1;
                else if (read_handshake) read_output_valid <= 0;
                if (read_handshake) begin
                    read_index <= read_index + 1'b1;
                    if (read_index + 1 == request_words) begin
                        read_stream_active <= 0; read_output_valid <= 0;
                        if(state==DRAIN) begin done[owner]<=1;read_index<=0;end
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
                        fill_index <= 0; processed <= 0;
                        read_stream_active <= 0; read_output_valid <= 0;
                        state <= CHECK_REQUEST;
                    end
                end
                CHECK_REQUEST: begin
                    if (refresh_due) begin refresh_resume <= CHECK_REQUEST; state <= REFRESH_WAIT; end
                    else if (request_invalid) begin done[owner] <= 1; error[owner] <= 1; state <= IDLE; end
                    else begin
                        if(!request_write) begin
                            read_stream_active<=1;read_output_valid<=0;read_index<=0;
                        end
                        state <= request_write ? FILL : CHOOSE;
                    end
                end
                FILL: begin
                    if (refresh_due) begin refresh_resume <= FILL; state <= REFRESH_WAIT; end
                    else if (w_valid[owner]) begin
                        if (fill_index + 1 == request_words) state <= CHOOSE;
                        else fill_index <= fill_index + 1'b1;
                    end
                end
                CHOOSE: begin
                    if (refresh_due) begin refresh_resume <= CHOOSE; state <= REFRESH_WAIT; end
                    else begin
                        burst_base <= {next_address[22:5], 5'b0};
                        burst_prefix <= next_address[4:2]; burst_beat <= 0;
                        burst_useful <= remaining_words < segment_room ? remaining_words : segment_room;
                        state <= PREPARE;
                    end
                end
                PREPARE: begin
                    if (refresh_due) begin refresh_resume <= PREPARE; state <= REFRESH_WAIT; end
                    else if (bank_open[burst_bank] && !row_matches && may_precharge) begin
                        bank_open[burst_bank] <= 0; rp_wait[burst_bank] <= RP_BITS'(T_RP - 1);
                    end else if (may_activate) begin
                        bank_open[burst_bank] <= 1; bank_row[burst_bank] <= burst_row;
                        rcd_wait[burst_bank] <= RCD_BITS'(T_RCD - 1);
                        ras_wait[burst_bank] <= RAS_BITS'(T_RAS - 1); rc_wait[burst_bank] <= RC_BITS'(T_RC - 1);
                    end else if (may_column) begin
                        if (request_write) begin
                            burst_beat <= 1; wr_wait[burst_bank] <= WR_BITS'(T_WR - 1); state <= WRITE_DATA;
                        end else begin
                            burst_beat <= 0; wait_count <= WAIT_BITS'(CAS_LATENCY); state <= READ_WAIT;
                        end
                    end
                end
                READ_WAIT: if (wait_count == 0) begin burst_beat <= 1; state <= READ_DATA; end
                           else wait_count <= wait_count - 1'b1;
                READ_DATA: begin
                    if (burst_beat == 7) begin
                        turnaround <= TURN_BITS'(READ_TO_WRITE - 1);
                        processed <= processed + burst_useful;
                        if (processed + burst_useful == request_words) begin
                            state <= DRAIN;
                        end else state <= CHOOSE;
                    end else burst_beat <= burst_beat + 1'b1;
                end
                WRITE_DATA: begin
                    wr_wait[burst_bank] <= WR_BITS'(T_WR - 1);
                    if (burst_beat == 7) begin
                        turnaround <= TURN_BITS'(WRITE_TO_READ - 1); wait_count <= WAIT_BITS'(T_WR - 1); state <= WRITE_RECOVERY;
                    end else burst_beat <= burst_beat + 1'b1;
                end
                WRITE_RECOVERY: begin
                    if (wait_count == 0) begin
                        processed <= processed + burst_useful;
                        if (processed + burst_useful == request_words) begin done[owner] <= 1; state <= IDLE; end
                        else state <= CHOOSE;
                    end else wait_count <= wait_count - 1'b1;
                end
                DRAIN: begin
                    if (refresh_due) begin refresh_resume <= DRAIN; state <= REFRESH_WAIT; end
                    else if (!read_stream_active) begin
                        // An early response can retire before the physical
                        // burst tail. Its completion waits for this state.
                        if(read_index==request_words) done[owner]<=1;
                        read_index<=0;state <= IDLE;
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
