// Focused boundary test: the complete core and real SDRAM service are active.
// Four recovery cases skip attachment boot and control a cache-side producer;
// the actual service supplies ready, transaction completion and physical SDRAM
// commands. Four record cases instead control the accepted read-response bus
// to test the actual record register/renderer interface with bursts and gaps.
module nano_core_recovery_quiesce_tb;
    timeunit 1ns;
    timeprecision 1ps;
    reg clk=0, host_clk=0, reset=1, abort_request=0;
    always #5 clk=~clk;
    always #20 host_clk=~host_clk;
    reg inject_tex_cmd=0, inject_tex_fault=0;
    wire sd_clk,sd_cke,sd_cs_n,sd_ras_n,sd_cas_n,sd_we_n,sd_dq_oe;
    wire [1:0] sd_ba;
    wire [10:0] sd_addr;
    wire [31:0] sd_dq_out;
    wire [3:0] sd_dqm;
    wire ready,busy,fault;
    wire [7:0] fault_code;
    integer cycles=0,checks=0,texture_accepts=0,upload_accepts=0,upload_payloads=0;
    integer physical_reads=0,physical_writes=0,quiet_upload_payloads=0;
    reg record_mode=0,record_response_valid=0,record_response_last=0,record_response_done=0;
    reg [31:0] record_response_data=0;
    integer record_input_index=0,record_words_written=0,record_words_accepted=0;
    reg expected_record_valid=0;
    integer expected_record_index=0;
    reg [31:0] expected_record_data=0;
    nano_gpu_core #(.POWERUP_CYCLES(8),.REFRESH_INTERVAL(1575)) dut(
        .clk(clk),.reset(reset),.host_clk(host_clk),.abort_request(abort_request),
        .host_wr_valid(1'b0),.host_wr_data(8'd0),.host_wr_ready(),
        .host_rd_req(1'b0),.host_reg_addr(7'd0),.host_rd_data(),.host_rd_valid(),
        .sd_dq_in(32'h87654321),.sd_clk(sd_clk),.sd_cke(sd_cke),.sd_cs_n(sd_cs_n),
        .sd_ras_n(sd_ras_n),.sd_cas_n(sd_cas_n),.sd_we_n(sd_we_n),
        .sd_ba(sd_ba),.sd_addr(sd_addr),.sd_dq_out(sd_dq_out),.sd_dq_oe(sd_dq_oe),.sd_dqm(sd_dqm),
        .video_rgb565(),.video_hsync(),.video_vsync(),.video_de(),.video_x(),.video_y(),.video_pixel_ce(),
        .busy(busy),.ready(ready),.fault(fault),.fault_code(fault_code));
    task check(input bit condition,input string message);
        begin checks=checks+1;if(!condition)$fatal(1,"recovery check: %s cycle=%0d",message,cycles);end
    endtask
    task step;
        begin @(posedge clk);#1;cycles=cycles+1;end
    endtask
    task prepare;
        integer limit;
        begin
            @(negedge clk);reset=1;abort_request=0;inject_tex_cmd=0;inject_tex_fault=0;
            repeat(8)step();
            // Public pin/command tests cover attachment initialization. Here
            // hold the foreground idle while the real SDRAM initializes, then
            // release it; all subsequent controller states execute normally.
            @(negedge clk);force dut.state=8'd4;reset=0;
            limit=0;
            while(!dut.memory_initialized || dut.host_recovering)begin
                step();limit=limit+1;check(limit<5000,"physical SDRAM and host initialization");
            end
            @(negedge clk);release dut.state;dut.state=8'd4;dut.initialized=1;
            step();check(!dut.foreground_quiet,"fresh core is not quiescent");
        end
    endtask
    task await_soft_recovery(input [31:0] expected_epoch);
        integer limit;
        begin
            limit=0;
            while(dut.state!=8'd4 || dut.foreground_quiet || dut.host_recovering || fault)begin
                step();limit=limit+1;check(limit<5000,"soft recovery reaches running idle");
            end
            check(dut.epoch==expected_epoch,"exactly one epoch increment");
            check(dut.pending_memory[5:1]==0,"no accepted foreground transaction left behind");
            check(ready && !busy,"recovered command processor is ready and idle");
            @(negedge clk);abort_request=0;
            repeat(8)step();
        end
    endtask
    task texture_entry_race(input bit use_fault);
        integer before_accepts,before_reads,limit;
        begin
            prepare();before_accepts=texture_accepts;before_reads=physical_reads;
            @(negedge clk);
            if(use_fault)inject_tex_fault=1;else abort_request=1;
            limit=0;
            while(!(use_fault ? dut.fault_pending : dut.abort_pending))begin
                step();limit=limit+1;check(limit<32,"request becomes pending");
            end
            check(!dut.foreground_quiet,"quiescence has a registered entry edge");
            check(dut.pending_memory[5:1]==0,"race starts with empty registered tracker");
            @(negedge clk);inject_tex_cmd=1;
            #1;check(dut.mem_cmd_valid[4] && dut.mem_cmd_ready[4],"real service accepts cache command on quiet-entry edge");
            step();
            check(dut.foreground_quiet,"quiet entry is registered");
            check(dut.pending_memory[4],"entry-edge acceptance is retained");
            check(texture_accepts==before_accepts+1,"one cache command accepted");
            check(!dut.renderer_reset && !dut.host_abort,"entry-edge acceptance prevents premature reset");
            limit=0;
            while(dut.pending_memory[4])begin
                check(!dut.mem_cmd_valid[4],"quiet blocks held younger cache command");
                check(!dut.renderer_reset && !dut.host_abort,"accepted read drains before reset");
                step();limit=limit+1;check(limit<5000,"accepted cache read finishes");
            end
            check(texture_accepts==before_accepts+1,"no second command escapes while quiet");
            check(physical_reads==before_reads+1,"real SDRAM read command was issued");
            @(negedge clk);inject_tex_cmd=0;inject_tex_fault=0;
            if(use_fault)begin
                limit=0;
                while(dut.state!=8'd61)begin step();limit=limit+1;check(limit<32,"fault reaches drained stopped state");end
                check(dut.foreground_quiet,"fault stays quiet through renderer reset");
                @(negedge clk);abort_request=1;
            end
            await_soft_recovery(2);
            $display("PASS cache acceptance on %s quiesce entry drains before reset",use_fault?"fault":"abort");
        end
    endtask
    task upload_payload_drain;
        integer before_accepts,before_payloads,before_writes,limit;
        begin
            prepare();before_accepts=upload_accepts;before_payloads=upload_payloads;before_writes=physical_writes;
            // Arm the real synchronizer so the command is accepted on the
            // abort-detection edge, before the registered quiet gate rises.
            @(negedge clk);abort_request=1;
            repeat(2)step();
            @(negedge clk);
            // The normal upload command state reads an already staged native
            // word exactly as a host upload would. No service response is mocked.
            dut.upload_ram.memory[0]=16'h3412;dut.upload_ram.memory[1]=16'h7856;dut.upload_ram.memory[32]=16'h000f;
            dut.upload_address=23'h258000;dut.upload_cursor=0;dut.upload_length=4;
            dut.upload_chunk=4;dut.upload_words=1;dut.beat=0;dut.state=8'd9;
            #1;check(dut.mem_cmd_valid[1]&&dut.mem_cmd_ready[1],"real service accepts upload");
            step();check(dut.pending_memory[1],"accepted upload is tracked");
            limit=0;
            while(!dut.foreground_quiet)begin step();limit=limit+1;check(limit<32,"upload abort enters quiet");end
            check(dut.pending_memory[1],"write stays pending after quiet");
            while(dut.pending_memory[1])begin
                check(!dut.renderer_reset && !dut.host_abort,"write payload and physical burst drain before reset");
                step();limit=limit+1;check(limit<5000,"accepted upload finishes");
            end
            check(upload_accepts==before_accepts+1,"exactly one upload accepted");
            check(upload_payloads==before_payloads+1,"all accepted upload payload delivered");
            check(quiet_upload_payloads>0,"payload handshake is allowed while quiet");
            check(physical_writes==before_writes+1,"physical write burst finishes");
            await_soft_recovery(2);
            $display("PASS accepted upload payload and physical WRITE drain while quiet");
        end
    endtask
    task active_renderer_abort;
        integer before_accepts,before_reads,limit;
        begin
            prepare();before_accepts=texture_accepts;before_reads=physical_reads;
            // An active renderer is an upstream boundary condition here;
            // the real cache request, arbiter and physical read still drain.
            @(negedge clk);force dut.tile_busy=1;abort_request=1;
            limit=0;
            while(!dut.abort_pending)begin step();limit=limit+1;check(limit<32,"active abort becomes pending");end
            check(!dut.foreground_quiet,"active renderer delays quiescence");
            @(negedge clk);inject_tex_cmd=1;
            #1;check(dut.mem_cmd_ready[4],"active primitive may still fetch texture after abort request");
            step();check(dut.pending_memory[4],"active primitive's accepted fetch is tracked");
            @(negedge clk);inject_tex_cmd=0;
            while(dut.pending_memory[4])begin
                check(!dut.foreground_quiet && !dut.renderer_reset,"active primitive drains before abort quiescence");
                step();limit=limit+1;check(limit<5000,"active primitive fetch drains");
            end
            repeat(3)begin step();check(!dut.foreground_quiet,"busy renderer keeps texture path running");end
            check(texture_accepts==before_accepts+1 && physical_reads==before_reads+1,"active primitive completed its texture fetch");
            @(negedge clk);release dut.tile_busy;
            step();check(dut.foreground_quiet,"renderer completion enters quiet");
            await_soft_recovery(2);
            $display("PASS active renderer keeps texture fetches enabled until abort quiescence");
        end
    endtask
    task record_boundary_begin;
        begin
            prepare();
            // The separate pin-level integration suite verifies SDRAM data.
            // Here a controlled accepted-response stream targets the core's
            // record-word boundary, including sustained responses and gaps.
            @(negedge clk);
            force dut.mem_cmd_ready[2]=1;
            force dut.mem_r_valid[2]=record_response_valid;
            force dut.mem_r_last[2]=record_response_last;
            force dut.mem_r_data[95:64]=record_response_data;
            force dut.mem_done[2]=record_response_done;
            record_mode=1;expected_record_valid=0;
            record_words_written=0;record_words_accepted=0;
            record_response_valid=0;record_response_last=0;record_response_done=0;
            dut.state=8'd32;dut.record_word=0;dut.beat=0;dut.pending_memory[2]=1;
        end
    endtask
    task record_boundary_end;
        begin
            @(negedge clk);record_mode=0;
            record_response_valid=0;record_response_last=0;record_response_done=0;
            release dut.mem_cmd_ready[2];release dut.mem_r_valid[2];
            release dut.mem_r_last[2];release dut.mem_r_data[95:64];release dut.mem_done[2];
        end
    endtask
    task record_stream;
        integer index,gap;
        begin
            record_boundary_begin();
            for(index=0;index<80;index=index+1)begin
                if(index!=0 && index%16==0)begin
                    @(negedge clk);record_response_valid=0;record_response_last=0;record_response_done=1;
                    step();check(dut.state==8'd31,"completed burst advances record command");
                    @(negedge clk);record_response_done=0;
                    step();check(dut.state==8'd32,"next record burst accepts command");
                end
                // First burst is sixteen consecutive accepted words; later
                // bursts deliberately add response bubbles without reordering.
                gap=index<16 ? 0 : index%3;
                repeat(gap)begin
                    @(negedge clk);record_response_valid=0;record_response_last=0;
                    step();
                end
                @(negedge clk);record_input_index=index;
                record_response_data=32'ha5963cc3 ^ (index*32'h01020305);
                record_response_last=index%16==15;record_response_valid=1;
                step();
            end
            @(negedge clk);record_response_valid=0;record_response_last=0;record_response_done=1;
            step();
            check(record_words_accepted==80 && record_words_written==80,"all eighty words retire before draw launch");
            check(dut.state==8'd34 && dut.tile_cmd_valid && !dut.record_load_valid,"draw begins only after final staged write");
            @(negedge clk);record_response_done=0;
            step();check(!dut.tile_cmd_word_we,"no staged word leaks into active draw");
            record_boundary_end();
            $display("PASS eighty record words retain one-edge latency, burst/gap order and final-word-before-launch");
        end
    endtask
    task record_cancel(input integer kind);
        integer limit;
        begin
            record_boundary_begin();
            if(kind==0)begin
                @(negedge clk);abort_request=1;repeat(2)step();
            end
            @(negedge clk);record_input_index=0;record_response_data=32'h11223344;record_response_valid=1;
            if(kind==1)inject_tex_fault=1;
            step();check(dut.record_load_valid,"response first enters retained record stage");
            check(record_words_accepted==1 && record_words_written==0,"accepted word has not bypassed its register");
            @(negedge clk);record_response_valid=0;record_response_done=1;
            if(kind==2)reset=1;
            step();
            check(!dut.record_load_valid && record_words_written==0,"abort/fault/reset cancels queued word");
            if(kind==2)begin
                check(dut.tile_renderer.state==0,"reset clears the renderer before any queued record can return");
            end else begin
                @(negedge clk);record_response_done=0;inject_tex_fault=0;
                if(kind==1)begin
                    limit=0;
                    while(dut.state!=8'd61)begin step();limit=limit+1;check(limit<32,"record fault drains into stopped state");end
                    @(negedge clk);abort_request=1;
                end
                await_soft_recovery(2);
                repeat(4)begin step();check(!dut.tile_cmd_word_we,"canceled record stays absent after recovery");end
                // A fresh record after the epoch change must receive only its
                // own word, even though the old stage's data bits are retained.
                @(negedge clk);dut.state=8'd32;dut.record_word=0;dut.beat=0;
                record_input_index=0;record_response_data=32'h55667788;record_response_valid=1;
                step();
                @(negedge clk);record_response_valid=0;
                step();check(record_words_written==1,"fresh record is unaffected by canceled retained data");
            end
            record_boundary_end();
            $display("PASS queued record cancellation kind%0d preserves later draw state",kind);
        end
    endtask
    always @(posedge clk)if(!reset)begin
        if(record_mode)begin
            if(dut.abort_pending || dut.fault_pending || dut.foreground_quiet || dut.renderer_reset)
                check(!dut.tile_cmd_word_we,"recovery suppresses retained record write immediately");
            else begin
                check(dut.tile_cmd_word_we==expected_record_valid,"record word has exactly one-edge enabled latency");
                if(expected_record_valid)begin
                    check(dut.tile_cmd_word_index==expected_record_index && dut.tile_cmd_word_data==expected_record_data,
                          "retained record data and index stay associated");
                    check(!dut.tile_busy && !dut.tile_cmd_valid,"record never writes a busy or launching renderer");
                    record_words_written=record_words_written+1;
                end
            end
            expected_record_valid=dut.state==8'd32 && record_response_valid && dut.mem_r_ready[2] &&
                !dut.abort_pending && !dut.fault_pending && !dut.foreground_quiet && !dut.renderer_reset;
            if(expected_record_valid)begin
                expected_record_index=record_input_index;expected_record_data=record_response_data;
                record_words_accepted=record_words_accepted+1;
            end
        end
        if(dut.mem_cmd_valid[4]&&dut.mem_cmd_ready[4])texture_accepts=texture_accepts+1;
        if(dut.mem_cmd_valid[1]&&dut.mem_cmd_ready[1])upload_accepts=upload_accepts+1;
        if(dut.mem_w_valid[1]&&dut.mem_w_ready[1])begin
            upload_payloads=upload_payloads+1;
            if(dut.foreground_quiet)quiet_upload_payloads=quiet_upload_payloads+1;
            check(dut.mem_w_data[32 +:32]==32'h78563412 && dut.mem_w_strb[4 +:4]==4'hf,"native upload data and mask survive recovery");
        end
        if(sd_cke&&!sd_cs_n&&sd_ras_n&&!sd_cas_n)begin
            if(sd_we_n)physical_reads=physical_reads+1;else physical_writes=physical_writes+1;
        end
        if(dut.foreground_quiet)check(dut.mem_cmd_valid[5:1]==0,"all quiet foreground command ports blocked");
        if(dut.host_abort || dut.host_recovering)check(dut.foreground_quiet,"quiet covers the complete cross-domain host flush");
    end
    always @(posedge clk)if(reset)expected_record_valid=0;
    initial begin
        // Controlled upstream producer; the core gates its command, and the
        // real service generates every ready/data/done handshake below it.
        force dut.scan_cmd_valid=0;
        force dut.tex_cmd_valid=inject_tex_cmd;
        force dut.tex_cmd_write=0;force dut.tex_cmd_addr=23'h258000;force dut.tex_cmd_words=1;
        force dut.tex_r_ready=1;force dut.tex_fault=inject_tex_fault;
        texture_entry_race(0);
        texture_entry_race(1);
        upload_payload_drain();
        active_renderer_abort();
        record_stream();
        record_cancel(0);
        record_cancel(1);
        record_cancel(2);
        $display("PASS %0d recovery checks: exact entry races, native payload draining, fault/abort and resumed idle",checks);
        $finish;
    end
    initial begin #2000000;$fatal(1,"recovery testbench timeout");end
endmodule
