`timescale 1ns/1ps
module nano_host_link_tb;
    parameter HOST_HALF_PERIOD = 13;
    parameter HOST_PHASE = 3;
    reg clk = 0, host_clk = 0, reset = 1;
    always #5 clk = !clk;
    initial begin #(HOST_PHASE); forever #(HOST_HALF_PERIOD) host_clk = !host_clk; end
    reg core_abort = 0, host_wr_valid = 0, host_rd_req = 0;
    reg [7:0] host_wr_data = 0;
    reg [6:0] host_reg_addr = 0;
    wire host_wr_ready, host_wr_ready_base, host_rd_valid;
    wire [7:0] host_rd_data;
    reg [511:0] status_snapshot = 0, capabilities = 0;
    wire cmd_valid; wire [15:0] cmd_opcode; wire [31:0] cmd_sequence;
    wire [10:0] cmd_payload_length;
    reg cmd_pop = 0, payload_req = 0;
    reg [9:0] payload_offset = 0;
    wire payload_valid; wire [7:0] payload_data;
    reg response_valid = 0; reg [7:0] response_data = 0;
    wire response_ready, recovering; wire [7:0] fault;
    nano_host_link dut(.*);
    integer checks = 0, i;
    // The board pad's register-only write ready equals host_wr_ready whenever
    // register 0 is addressed and no read is requested.
    always @(posedge host_clk)
        if (host_reg_addr == 0 && !host_rd_req && host_wr_ready_base !== host_wr_ready)
            $fatal(1, "host_wr_ready_base differs from host_wr_ready");
    reg [7:0] byte_result;
    reg producer_done = 0;
    reg tx_backpressure_seen = 0;
    always @(posedge clk) if (response_valid && !response_ready) tx_backpressure_seen <= 1;

    task automatic check(input bit condition, input string message);
        if (!condition) $fatal(1, "%s at %0t", message, $time);
        checks = checks + 1;
    endtask
    task automatic send_byte(input [7:0] value);
        integer timeout;
        begin
            @(negedge host_clk); host_reg_addr = 0; host_wr_data = value; host_wr_valid = 1;
            timeout = 0;
            @(posedge host_clk);
            while (!host_wr_ready) begin
                timeout = timeout + 1;
                if (timeout > 200000) $fatal(1, "host write timed out");
                @(posedge host_clk);
            end
            @(negedge host_clk); host_wr_valid = 0;
        end
    endtask
    task automatic header_version(input [15:0] opcode, input [31:0] sequence_id,
                                  input [31:0] length, input [7:0] minor);
        begin
            send_byte(8'h4e); send_byte(8'h47); send_byte(8'h50); send_byte(8'h55);
            send_byte(0); send_byte(minor); send_byte(opcode[7:0]); send_byte(opcode[15:8]);
            for (integer b = 0; b < 4; b = b + 1) send_byte(length[8*b +:8]);
            for (integer b = 0; b < 4; b = b + 1) send_byte(sequence_id[8*b +:8]);
        end
    endtask
    task automatic header(input [15:0] opcode, input [31:0] sequence_id, input [31:0] length);
        begin header_version(opcode, sequence_id, length, 2); end
    endtask
    task automatic packet(input [31:0] sequence_id, input integer length, input [7:0] seed);
        begin
            header(3, sequence_id, length);
            for (integer b = 0; b < length; b = b + 1) send_byte(seed + 8'(b));
        end
    endtask
    task automatic wait_command(input [31:0] sequence_id, input integer length);
        integer timeout;
        begin
            timeout = 0;
            while (!cmd_valid) begin
                @(posedge clk); #1;
                timeout = timeout + 1;
                if (timeout > 200000) $fatal(1, "command timed out, fault=%0d", fault);
            end
            check(cmd_opcode == 3 && cmd_sequence == sequence_id && cmd_payload_length == length,
                  "command metadata lost framing or ordering");
        end
    endtask
    task automatic payload(input integer offset, input [7:0] expected);
        begin
            @(negedge clk); payload_req = 1; payload_offset = 10'(offset);
            @(posedge clk); #1;
            check(payload_valid && payload_data == expected, "registered packet RAM read differs");
            @(negedge clk); payload_req = 0;
        end
    endtask
    task automatic pop;
        begin
            @(negedge clk); cmd_pop = 1;
            @(negedge clk); cmd_pop = 0;
        end
    endtask
    task automatic read_register(input [6:0] address, output [7:0] value);
        integer timeout;
        begin
            @(negedge host_clk); host_reg_addr = address; host_rd_req = 1;
            timeout = 0;
            @(posedge host_clk); #1;
            while (!host_rd_valid) begin
                timeout = timeout + 1;
                if (timeout > 200000) $fatal(1, "host read timed out");
                @(posedge host_clk); #1;
            end
            value = host_rd_data;
            @(negedge host_clk); host_rd_req = 0; host_reg_addr = 0;
        end
    endtask
    task automatic recover;
        integer timeout;
        begin
            @(negedge clk); core_abort = 1;
            @(negedge clk); core_abort = 0;
            timeout = 0;
            while (recovering) begin
                @(posedge clk); #1;
                timeout = timeout + 1;
                if (timeout > 2000) $fatal(1, "cross-clock recovery timed out");
            end
            repeat (4) @(posedge host_clk);
            check(fault == 0 && !cmd_valid, "recovery retained fault or command");
        end
    endtask
    task automatic wait_fault(input [7:0] expected);
        integer timeout;
        begin
            timeout = 0;
            while (fault == 0) begin
                @(posedge clk); #1;
                timeout = timeout + 1;
                if (timeout > 10000) $fatal(1, "fault never appeared");
            end
            check(fault == expected, "wrong parser/bus fault category");
        end
    endtask

    initial begin
        for (i = 0; i < 64; i = i + 1) begin
            status_snapshot[8*i +:8] = 8'(i) ^ 8'ha5;
            capabilities[8*i +:8] = 8'(i + 32);
        end
        repeat (8) @(posedge clk);
        @(negedge clk); reset = 0;
        repeat (8) @(posedge host_clk);

        // Complete publication only, synchronous payload reads, and three
        // physical packet banks retained while both command leases are full.
        packet(1, 12, 8'h10); wait_command(1, 12);
        for (i = 0; i < 12; i = i + 1) payload(i, 8'h10 + 8'(i));
        packet(2, 1024, 8'h30);
        packet(3, 100, 8'h70);
        repeat (10) @(posedge clk);
        check(cmd_valid && cmd_sequence == 1 && fault == 0, "owned front bank changed while producer queued");

        read_register(1, byte_result);
        check((byte_result & 8'h2f) == 8'h24, "status read failed while the command queue was blocked");
        status_snapshot = {64{8'h11}};
        read_register(2, byte_result); check(byte_result == (8'd2 ^ 8'ha5), "status snapshot was torn");
        read_register(63, byte_result); check(byte_result == (8'd63 ^ 8'ha5), "status snapshot changed before refresh");
        read_register(1, byte_result);
        read_register(2, byte_result); check(byte_result == 8'h11, "refresh did not publish the new snapshot");
        status_snapshot = {64{8'h22}};
        read_register(63, byte_result); check(byte_result == 8'h11, "refreshed snapshot changed without a new request");
        read_register(64, byte_result); check(byte_result == 8'd32, "capability offset zero differs");
        read_register(127, byte_result); check(byte_result == 8'd95, "capability final byte differs");

        fork
            begin packet(4, 400, 8'h90); producer_done = 1; end
            begin
                // Third packet is staged, so a fourth packet must eventually
                // backpressure the external producer without dropping bytes.
                wait (!host_wr_ready && host_wr_valid);
                repeat (12) @(posedge host_clk);
                check(!producer_done, "full packet banks did not backpressure producer");
                pop(); wait_command(2, 1024);
                for (integer b = 0; b < 1024; b = b + 1) payload(b, 8'h30 + 8'(b));
                pop(); wait_command(3, 100);
                for (integer b = 0; b < 100; b = b + 1) payload(b, 8'h70 + 8'(b));
                pop(); wait_command(4, 400);
                for (integer b = 0; b < 400; b = b + 1) payload(b, 8'h90 + 8'(b));
                pop();
            end
        join
        check(fault == 0, "legal asynchronous transfers faulted");

        // TX fills and wraps while the host applies independent backpressure.
        fork
            begin
                for (integer b = 0; b < 2050; b = b + 1) begin
                    @(negedge clk); response_valid = 1; response_data = 8'(b ^ 8'h5a);
                    @(posedge clk);
                    while (!response_ready) @(posedge clk);
                    @(negedge clk); response_valid = 0;
                end
            end
            begin
                // Fill TX before starting the consumer even when host_clk is
                // faster than clk. The producer issues one byte per two core
                // edges; 2200 edges exceeds the 1024-byte capacity.
                repeat (2200) @(posedge clk);
                for (integer b = 0; b < 2050; b = b + 1) begin
                    read_register(0, byte_result);
                    check(byte_result == 8'(b ^ 8'h5a), "TX FIFO lost data through asynchronous wrap/full");
                end
            end
        join
        check(tx_backpressure_seen, "TX full/backpressure path was not exercised");
        check(fault == 0, "TX traffic changed parser fault state");

        // Partial packets never publish; recovery resets sequence tracking and
        // both CDC FIFOs while leaving physical RAM contents inaccessible.
        header(3, 5, 17); send_byte(1); send_byte(2); send_byte(3);
        repeat (20) @(posedge clk); check(!cmd_valid, "partial payload executed");
        recover();
        packet(1, 1, 8'hdc); wait_command(1, 1); payload(0, 8'hdc); pop();
        header(3, 1, 0); wait_fault(11); recover();
        header(3, 0, 0); wait_fault(11); recover();
        header(9, 1, 0); wait_fault(9); recover();
        header(16'h0101, 1, 0); wait_fault(9); recover();
        header(16'h0010, 1, 0); wait_fault(9); recover();
        header(3, 1, 1025); wait_fault(10); recover();
        header(3, 1, 32'h10000); wait_fault(10); recover();
        // Only the current 0.2 wire format is implemented. An otherwise valid
        // obsolete header must fault and leave no command lease executable.
        header_version(3, 1, 0, 1); wait_fault(8); recover();
        header_version(3, 1, 0, 3); wait_fault(8); recover();
        send_byte(8'h00); for (i = 1; i < 16; i = i + 1) send_byte(0);
        wait_fault(7); recover();
        @(negedge host_clk); host_reg_addr = 1; host_wr_data = 4; host_wr_valid = 1;
        @(negedge host_clk); host_wr_valid = 0; host_reg_addr = 0;
        wait_fault(2); recover();
        @(negedge host_clk); host_wr_valid = 1; host_rd_req = 1;
        @(negedge host_clk); host_wr_valid = 0; host_rd_req = 0;
        wait_fault(1); recover();
        packet(1, 0, 0); wait_command(1, 0); pop();
        $display("PASS nano_host_link: %0d checks, asynchronous RX/TX wrapping, bank leases, snapshots, faults and recovery", checks);
        $finish;
    end
    initial begin #10000000; $fatal(1, "global host transport timeout"); end
endmodule
