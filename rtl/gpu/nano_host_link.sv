// Source-synchronous eight-bit host transport. The RP2350 owns host_clk.
// Native RAMs and Gray-pointer FIFOs are shared by simulation and synthesis;
// packet payloads never pass through an untimed C++/register-array back door.
module nano_host_link (
    input wire clk, input wire reset, input wire host_clk, input wire core_abort,
    input wire host_wr_valid, input wire [7:0] host_wr_data,
    output wire host_wr_ready,
    input wire host_rd_req, input wire [6:0] host_reg_addr,
    output reg [7:0] host_rd_data, output reg host_rd_valid,
    input wire [511:0] status_snapshot, input wire [511:0] capabilities,
    output wire cmd_valid, output wire [15:0] cmd_opcode,
    output wire [31:0] cmd_sequence, output wire [10:0] cmd_payload_length,
    input wire cmd_pop,
    input wire payload_req, input wire [9:0] payload_offset,
    output reg payload_valid, output wire [7:0] payload_data,
    input wire response_valid, output wire response_ready,
    input wire [7:0] response_data,
    output reg [7:0] fault, output reg recovering
);
    // Soft recovery is a cross-domain flush handshake. The caller first
    // quiesces host DMA and drains device memory work. Both clocks must run
    // until recovering deasserts; no old payload or reply survives the flush.
    reg abort_toggle;
    (* async_reg = "true" *) reg abort_meta, abort_sync;
    reg host_abort_seen, host_abort_ack;
    reg host_reset_active;
    reg [1:0] host_flush_count;
    (* async_reg = "true" *) reg ack_meta, ack_sync;
    reg [1:0] settle_count;
    wire core_flush = reset || core_abort || recovering;
    wire host_flush = reset || host_reset_active;
    always @(posedge clk or posedge reset) begin
        if (reset) begin
            abort_toggle <= 0; recovering <= 0; ack_meta <= 0; ack_sync <= 0;
            settle_count <= 0;
        end else begin
            ack_meta <= host_abort_ack; ack_sync <= ack_meta;
            if (core_abort && !recovering) begin
                abort_toggle <= !abort_toggle; recovering <= 1; settle_count <= 0;
            end else if (recovering) begin
                if (ack_sync == abort_toggle) begin
                    if (settle_count == 3) begin recovering <= 0; settle_count <= 0; end
                    else settle_count <= settle_count + 1'b1;
                end else settle_count <= 0;
            end
        end
    end
    always @(posedge host_clk or posedge reset) begin
        if (reset) begin
            abort_meta <= 0; abort_sync <= 0; host_abort_seen <= 0;
            host_abort_ack <= 0; host_flush_count <= 0; host_reset_active <= 0;
        end else begin
            abort_meta <= abort_toggle; abort_sync <= abort_meta;
            if (abort_sync != host_abort_seen) begin
                host_abort_seen <= abort_sync; host_flush_count <= 3; host_reset_active <= 1;
            end else if (host_flush_count != 0) begin
                host_flush_count <= host_flush_count - 1'b1;
                if (host_flush_count == 1) begin
                    host_abort_ack <= host_abort_seen; host_reset_active <= 0;
                end
            end
        end
    end

    wire rx_write_ready, rx_valid, rx_ready;
    wire [7:0] rx_data;
    wire tx_read_valid, tx_read_ready;
    wire [7:0] tx_read_data;
    reg [7:0] host_error;
    reg status_pending;
    reg waiting_write;
    reg [7:0] waiting_data;
    reg [6:0] waiting_address;
    wire host_write_legal = host_reg_addr == 0 && !host_rd_req && host_error == 0 && !host_flush;
    assign host_wr_ready = rx_write_ready && host_write_legal;
    nano_host_async_fifo #(.ADDRESS_BITS(8)) rx_fifo (
        .reset(reset), .wclk(host_clk), .wflush(host_flush),
        .wvalid(host_wr_valid && host_write_legal), .wready(rx_write_ready), .wdata(host_wr_data),
        .rclk(clk), .rflush(core_flush), .rvalid(rx_valid), .rready(rx_ready), .rdata(rx_data)
    );
    nano_host_async_fifo #(.ADDRESS_BITS(10)) tx_fifo (
        .reset(reset), .wclk(clk), .wflush(core_flush), .wvalid(response_valid && !core_flush),
        .wready(response_ready), .wdata(response_data),
        .rclk(host_clk), .rflush(host_flush), .rvalid(tx_read_valid),
        .rready(tx_read_ready), .rdata(tx_read_data)
    );
    assign tx_read_ready = host_rd_req && host_reg_addr == 0 && !host_wr_valid && !host_flush && !status_pending;
    always @(posedge host_clk or posedge reset) begin
        if (reset) begin host_error <= 0; waiting_write <= 0; waiting_data <= 0; waiting_address <= 0; end
        else if (host_flush) begin host_error <= 0; waiting_write <= 0; end
        else begin
            if (host_error == 0) begin
                if (host_wr_valid && host_rd_req) host_error <= 8'd1;
                else if (host_wr_valid && host_reg_addr != 0) host_error <= 8'd2;
                else if (waiting_write && host_wr_valid &&
                    (waiting_data != host_wr_data || waiting_address != host_reg_addr)) host_error <= 8'd5;
            end
            if (!host_wr_valid || host_wr_ready) waiting_write <= 0;
            else if (!waiting_write) begin
                waiting_write <= 1; waiting_data <= host_wr_data; waiting_address <= host_reg_addr;
            end
        end
    end

    // Packet ownership: two published queue entries and one staging packet.
    // Three logical leases share two SDPB instances; each has one write/read port.
    localparam [1:0] HEADER = 0, CHECK = 1, PAYLOAD = 2, PUBLISH = 3;
    reg [1:0] parser_state;
    // Validate complete raw words before narrowing, retaining only fields
    // needed after CHECK. A single rolling word also holds the final sequence.
    reg [31:0] header_word;
    reg [3:0] header_bad;
    reg [3:0] header_opcode;
    reg [10:0] header_length;
    reg [3:0] header_offset;
    reg [10:0] payload_received;
    reg [31:0] last_sequence;
    reg [1:0] staging_bank;
    reg [1:0] queue_bank [0:1];
    // CHECK validates the full u16 opcode before publishing a bank. Only
    // values 1..8 are retained; the public command port remains u16.
    reg [3:0] bank_opcode [0:2];
    reg [31:0] bank_sequence [0:2];
    reg [10:0] bank_length [0:2];
    reg queue_head, queue_tail;
    reg [1:0] queue_count;
    wire pop = cmd_valid && cmd_pop;
    wire publish = parser_state == PUBLISH && (queue_count != 2 || pop) && !core_flush && fault == 0;
    reg [2:0] used_banks;
    reg [1:0] next_staging;
    integer scan;
    always @* begin
        used_banks = 3'b000;
        used_banks[staging_bank] = 1;
        if (queue_count != 0 && !pop) used_banks[queue_bank[queue_head]] = 1;
        if (queue_count == 2) used_banks[queue_bank[!queue_head]] = 1;
        next_staging = 0;
        for (scan = 2; scan >= 0; scan = scan - 1)
            if (!used_banks[scan]) next_staging = scan[1:0];
    end
    assign cmd_valid = queue_count != 0 && !core_flush && fault == 0;
    assign cmd_opcode = {12'b0,bank_opcode[queue_bank[queue_head]]};
    assign cmd_sequence = bank_sequence[queue_bank[queue_head]];
    assign cmd_payload_length = bank_length[queue_bank[queue_head]];
    assign rx_ready = !core_flush && fault == 0 && (parser_state == HEADER || parser_state == PAYLOAD);
    wire consume_rx = rx_valid && rx_ready;
    wire [31:0] header_word_next = {rx_data,header_word[31:8]};
    wire [15:0] decoded_opcode = {12'b0,header_opcode};
    wire [31:0] decoded_length = {21'b0,header_length};
    wire [31:0] decoded_sequence = header_word;
    reg [7:0] header_error;
    always @* begin
        header_error = 0;
        if (header_bad[0]) header_error = 7;
        else if (header_bad[1]) header_error = 8;
        else if (header_bad[2]) header_error = 9;
        else if (header_bad[3]) header_error = 10;
        else if (decoded_sequence == 0 || decoded_sequence <= last_sequence) header_error = 11;
    end
    (* async_reg = "true" *) reg host_error_meta, host_error_sync;
    always @(posedge clk or posedge reset) begin
        if (reset) begin host_error_meta <= 0; host_error_sync <= 0; fault <= 0; end
        else if (core_flush) begin host_error_meta <= 0; host_error_sync <= 0; fault <= 0; end
        else begin
            host_error_meta <= host_error != 0; host_error_sync <= host_error_meta;
            if (fault == 0) begin
                // host_error is a bundled, sticky value established before
                // its synchronized flag; it never changes until recovery.
                if (host_error_sync) fault <= host_error;
                else if (parser_state == CHECK && header_error != 0) fault <= header_error;
            end
        end
    end
    integer bank_index;
    always @(posedge clk or posedge reset) begin
        if (reset) begin
            parser_state <= HEADER; header_word <= 0; header_bad <= 0;
            header_opcode <= 0; header_length <= 0; header_offset <= 0; payload_received <= 0;
            last_sequence <= 0; staging_bank <= 2; queue_head <= 0; queue_tail <= 0; queue_count <= 0;
            queue_bank[0] <= 0; queue_bank[1] <= 0;
            for (bank_index = 0; bank_index < 3; bank_index = bank_index + 1) begin
                bank_opcode[bank_index] <= 0; bank_sequence[bank_index] <= 0; bank_length[bank_index] <= 0;
            end
        end else if (core_flush) begin
            parser_state <= HEADER; header_word <= 0; header_bad <= 0;
            header_opcode <= 0; header_length <= 0; header_offset <= 0; payload_received <= 0;
            last_sequence <= 0; staging_bank <= 2; queue_head <= 0; queue_tail <= 0; queue_count <= 0;
        end else begin
            case ({publish, pop})
                2'b10: queue_count <= queue_count + 1'b1;
                2'b01: queue_count <= queue_count - 1'b1;
                default: begin end
            endcase
            if (pop) queue_head <= !queue_head;
            if (publish) begin
                queue_bank[queue_tail] <= staging_bank; queue_tail <= !queue_tail;
                staging_bank <= next_staging; parser_state <= HEADER; header_offset <= 0;
                payload_received <= 0;
            end else if (fault == 0) begin
                case (parser_state)
                    HEADER: if (consume_rx) begin
                        header_word <= header_word_next;
                        if (header_offset == 0) header_bad <= 0;
                        case (header_offset)
                            3: header_bad[0] <= header_word_next != 32'h5550474e;
                            7: begin
                                header_bad[1] <= header_word_next[15:0] != 16'h0200;
                                header_bad[2] <= header_word_next[31:16] == 0 ||
                                    (|header_word_next[31:20]) || header_word_next[19:16] > 4'd8;
                                header_opcode <= header_word_next[19:16];
                            end
                            11: begin
                                header_bad[3] <= (|header_word_next[31:11]) ||
                                    (header_word_next[10] && (|header_word_next[9:0]));
                                header_length <= header_word_next[10:0];
                            end
                            default: begin end
                        endcase
                        if (header_offset == 15) parser_state <= CHECK;
                        else header_offset <= header_offset + 1'b1;
                    end
                    CHECK: if (header_error == 0) begin
                        bank_opcode[staging_bank] <= decoded_opcode;
                        bank_sequence[staging_bank] <= decoded_sequence;
                        bank_length[staging_bank] <= decoded_length[10:0];
                        last_sequence <= decoded_sequence; payload_received <= 0;
                        parser_state <= decoded_length == 0 ? PUBLISH : PAYLOAD;
                    end
                    PAYLOAD: if (consume_rx) begin
                        payload_received <= payload_received + 1'b1;
                        if (payload_received + 11'd1 == bank_length[staging_bank]) parser_state <= PUBLISH;
                    end
                    default: begin end
                endcase
            end
        end
    end

    wire [7:0] packet_output [0:1];
    reg payload_bank;
    wire payload_read = payload_req && cmd_valid && !cmd_pop;
    genvar p;
    // Three logical 1-KiB leases fit in two native 2-KiB byte RAMs. Banks
    // zero/one occupy disjoint halves of RAM zero; bank two uses RAM one.
    // Lease ownership keeps simultaneous read/write addresses distinct.
    generate for (p = 0; p < 2; p = p + 1) begin: packets
        nano_sdp8 packet (
            .wclk(clk), .wce(1'b1),
            .we(consume_rx && parser_state == PAYLOAD && staging_bank[1] == (p != 0)),
            .waddr({(p == 0 ? staging_bank[0] : 1'b0),payload_received[9:0]}), .wdata(rx_data),
            .rclk(clk), .rce(1'b1), .re(payload_read && queue_bank[queue_head][1] == (p != 0)),
            .raddr({(p == 0 ? queue_bank[queue_head][0] : 1'b0),payload_offset}),
            .reset_output(core_flush), .rdata(packet_output[p])
        );
    end endgenerate
    always @(posedge clk or posedge reset) begin
        if (reset) begin payload_valid <= 0; payload_bank <= 0; end
        else if (core_flush) begin payload_valid <= 0; payload_bank <= 0; end
        else begin
            payload_valid <= payload_read;
            if (payload_read) payload_bank <= queue_bank[queue_head][1];
        end
    end
    assign payload_data = packet_output[payload_bank];

    // A register-1 request captures one complete core-domain snapshot. A
    // synchronized request/acknowledge toggle keeps the 512-bit bundled bus
    // stable while it crosses; individual data bits are not synchronized.
    reg snapshot_request;
    (* async_reg = "true" *) reg snapshot_req_meta, snapshot_req_sync;
    reg snapshot_ack;
    reg [511:0] snapshot_hold;
    (* async_reg = "true" *) reg snapshot_ack_meta, snapshot_ack_sync;
    reg status_latched;
    reg [5:0] status_address;
    wire [5:0] selected_status_address = status_pending ? status_address : host_reg_addr[5:0];
    wire [7:0] selected_status_byte = snapshot_hold[8*selected_status_address +: 8];
    always @(posedge clk or posedge reset) begin
        if (reset) begin snapshot_req_meta <= 0; snapshot_req_sync <= 0; snapshot_ack <= 0; snapshot_hold <= 0; end
        else if (core_flush) begin snapshot_req_meta <= 0; snapshot_req_sync <= 0; snapshot_ack <= 0; snapshot_hold <= 0; end
        else begin
            snapshot_req_meta <= snapshot_request; snapshot_req_sync <= snapshot_req_meta;
            if (snapshot_req_sync != snapshot_ack) begin
                snapshot_hold <= status_snapshot;
                snapshot_hold[15:8] <= (status_snapshot[15:8] & 8'h0f) |
                    (fault != 0 ? 8'h80 : 8'h00) | (queue_count == 2 ? 8'h20 : 8'h00);
                snapshot_ack <= snapshot_req_sync;
            end
        end
    end
    always @(posedge host_clk or posedge reset) begin
        if (reset) begin
            snapshot_request <= 0; snapshot_ack_meta <= 0; snapshot_ack_sync <= 0;
            status_latched <= 0; status_pending <= 0; status_address <= 0;
            host_rd_data <= 0; host_rd_valid <= 0;
        end else if (host_flush) begin
            snapshot_request <= 0; snapshot_ack_meta <= 0; snapshot_ack_sync <= 0;
            status_latched <= 0; status_pending <= 0; host_rd_valid <= 0; host_rd_data <= 0;
        end else begin
            snapshot_ack_meta <= snapshot_ack; snapshot_ack_sync <= snapshot_ack_meta;
            host_rd_valid <= 0;
            if (status_pending) begin
                if (snapshot_ack_sync == snapshot_request) begin
                    status_latched <= 1; status_pending <= 0;
                    host_rd_data <= selected_status_byte |
                        (status_address == 1 ? ((!rx_write_ready ? 8'h40 : 8'h00) |
                            (tx_read_valid ? 8'h10 : 8'h00) | (host_error != 0 ? 8'h80 : 8'h00)) : 8'h00);
                    host_rd_valid <= 1;
                end
            end else if (host_rd_req && !host_wr_valid) begin
                if (host_reg_addr == 0) begin
                    if (tx_read_valid) begin host_rd_data <= tx_read_data; host_rd_valid <= 1; end
                end else if (host_reg_addr >= 64) begin
                    // Capabilities are immutable for the complete session.
                    host_rd_data <= capabilities[8*host_reg_addr[5:0] +: 8]; host_rd_valid <= 1;
                end else if (host_reg_addr == 1 || !status_latched) begin
                    snapshot_request <= !snapshot_request; status_pending <= 1;
                    status_address <= host_reg_addr[5:0];
                end else begin
                    // The acknowledged core bank stays unchanged until the
                    // next snapshot request. status_pending blocks all reads
                    // during that refresh, so no second wide host copy is
                    // needed. Reset/recovery invalidates the old session.
                    host_rd_data <= selected_status_byte; host_rd_valid <= 1;
                end
            end
        end
    end
endmodule


// One fixed native SDPB FIFO with independent clocks. The read pointer retires
// only when the consumer accepts the registered RAM head, so prefetched data
// remains within the advertised 2**ADDRESS_BITS capacity (no hidden extra slot).
module nano_host_async_fifo #(parameter ADDRESS_BITS = 8) (
    input wire reset, input wire wclk, input wire wflush, input wire wvalid,
    output wire wready, input wire [7:0] wdata,
    input wire rclk, input wire rflush,
    output reg rvalid, input wire rready, output wire [7:0] rdata
);
    reg [ADDRESS_BITS:0] write_binary, write_gray, read_binary, read_gray;
    (* async_reg = "true" *) reg [ADDRESS_BITS:0] read_gray_meta, read_gray_sync;
    (* async_reg = "true" *) reg [ADDRESS_BITS:0] write_gray_meta, write_gray_sync;
    reg full;
    wire write_accepted = wvalid && wready;
    wire read_accepted = rvalid && rready;
    wire [ADDRESS_BITS:0] next_write = write_binary + {{ADDRESS_BITS{1'b0}}, write_accepted};
    wire [ADDRESS_BITS:0] next_write_gray = (next_write >> 1) ^ next_write;
    wire [ADDRESS_BITS:0] next_read = read_binary + {{ADDRESS_BITS{1'b0}}, read_accepted};
    wire [ADDRESS_BITS:0] next_read_gray = (next_read >> 1) ^ next_read;
    wire full_next = next_write_gray == {~read_gray_sync[ADDRESS_BITS:ADDRESS_BITS-1], read_gray_sync[ADDRESS_BITS-2:0]};
    wire next_empty = next_read_gray == write_gray_sync;
    wire read_head = !reset && !rflush && (!rvalid || rready) && !next_empty;
    wire [15:0] native_data;
    assign wready = !full && !reset && !wflush;
    assign rdata = native_data[7:0];
    nano_sdp16 storage (
        .wclk(wclk), .wce(1'b1), .we(write_accepted),
        .waddr({{(10-ADDRESS_BITS){1'b0}}, write_binary[ADDRESS_BITS-1:0]}), .wdata({8'b0, wdata}),
        .rclk(rclk), .rce(1'b1), .re(read_head),
        .raddr({{(10-ADDRESS_BITS){1'b0}}, next_read[ADDRESS_BITS-1:0]}),
        .reset_output(reset || rflush), .rdata(native_data)
    );
    always @(posedge wclk or posedge reset) begin
        if (reset) begin write_binary <= 0; write_gray <= 0; read_gray_meta <= 0; read_gray_sync <= 0; full <= 0; end
        else if (wflush) begin write_binary <= 0; write_gray <= 0; read_gray_meta <= 0; read_gray_sync <= 0; full <= 0; end
        else begin
            read_gray_meta <= read_gray; read_gray_sync <= read_gray_meta;
            write_binary <= next_write; write_gray <= next_write_gray; full <= full_next;
        end
    end
    always @(posedge rclk or posedge reset) begin
        if (reset) begin read_binary <= 0; read_gray <= 0; write_gray_meta <= 0; write_gray_sync <= 0; rvalid <= 0; end
        else if (rflush) begin read_binary <= 0; read_gray <= 0; write_gray_meta <= 0; write_gray_sync <= 0; rvalid <= 0; end
        else begin
            write_gray_meta <= write_gray; write_gray_sync <= write_gray_meta;
            read_binary <= next_read; read_gray <= next_read_gray;
            if (read_head) rvalid <= 1;
            else if (read_accepted) rvalid <= 0;
        end
    end
endmodule
