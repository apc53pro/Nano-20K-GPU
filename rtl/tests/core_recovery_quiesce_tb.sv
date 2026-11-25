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
    localparam [7:0] REF_CMD=28,REF_DATA=29,RECORD_CMD=31,RECORD_DATA=32,STORE_CMD=36;
    localparam [22:0] OLD_LIST=23'h25a000,FRESH_LIST=23'h26b000;
    localparam [31:0] OLD_RECORD=32'h0025a100,FRESH_RECORD=32'h0026b100;
    integer reference_mode=0,reference_reads=0,reference_beats=0,quiet_reference_beats=0;
    integer reference_writes=0,fresh_record_reads=0;
    localparam [7:0] COPY_READ_CMD=53,COPY_WRITE_CMD=58,COPY_WRITE_DONE=60;
    localparam [22:0] COPY_SOURCE=23'h268000,COPY_DESTINATION=23'h096000;
    localparam integer COPY_PIXELS=70,COPY_PITCH=192,COPY_WORDS=(COPY_PIXELS+2)/2;
    localparam integer COPY_BURSTS=(COPY_WORDS+15)/16,COPY_ROW_COMMANDS=2*COPY_BURSTS;
    localparam [31:0] COPY_SEQUENCE=32'h17482831;
    reg monitor_copy=0,boundary_ready=0,boundary_done=0,boundary_payload_ready=0;
    integer copy_accepted=0,copy_completed=0,copy_peak=0,copy_payloads=0,copy_pins=0,quiet_copy_payloads=0;
    reg [22:0] fixture_read_address=0;
    function automatic [31:0] fixture_copy_word(input [22:0] byte_address);
        begin fixture_copy_word=32'hf18335ac ^ (32'h00274109*{9'd0,byte_address});end
    endfunction
    function automatic [3:0] fixture_copy_strobe(input integer word_index);
        begin
            if(word_index==0)fixture_copy_strobe=4'hc;
            else if(word_index==COPY_WORDS-1)fixture_copy_strobe=4'h3;
            else fixture_copy_strobe=4'hf;
        end
    endfunction
    wire [4:0] physical_word=dut.memory_service.read_buffer_index;
    wire [31:0] sd_dq_in=monitor_copy ?
        fixture_copy_word(fixture_read_address+{16'd0,physical_word,2'b0}) :
        reference_mode==0 ? 32'h87654321 :
        (dut.state==RECORD_DATA ? rectangle_word(dut.record_word+{2'b0,physical_word}) :
         ((reference_mode==1 ? OLD_RECORD : FRESH_RECORD)+32'd128*physical_word));
    function automatic [31:0] rectangle_word(input [6:0] word_index);
        begin
            case(word_index)
            0: rectangle_word=32'h00000201; // Version 2 rectangle, no attachment writes.
            6: rectangle_word=640;
            7: rectangle_word=480;
            10,11: rectangle_word=1;
            default: rectangle_word=0;
            endcase
        end
    endfunction
    nano_gpu_core #(.POWERUP_CYCLES(8),.REFRESH_INTERVAL(1575)) dut(
        .pixel_advance(1'b0), .clk(clk),.reset(reset),.host_clk(host_clk),.abort_request(abort_request),
        .host_wr_valid(1'b0),.host_wr_data(8'd0),.host_wr_ready(),
        .host_rd_req(1'b0),.host_reg_addr(7'd0),.host_rd_data(),.host_rd_valid(),
        .sd_dq_in(sd_dq_in),.sd_clk(sd_clk),.sd_cke(sd_cke),.sd_cs_n(sd_cs_n),
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
    task start_copy_fixture(input [7:0] first_state,input [9:0] first_word,input [10:0] rows);
        begin
            dut.copy_active=1;dut.copy_target_id=1;dut.copy_previous_valid=1;dut.color_valid=2'b01;
            dut.copy_source_row=COPY_SOURCE+23'd2;dut.copy_destination_row=COPY_DESTINATION+23'd2;
            dut.copy_pitch=23'(COPY_PITCH);dut.copy_count_x=11'(COPY_PIXELS);dut.copy_count_y=rows;
            dut.copy_reverse=0;dut.copy_row_number=0;dut.copy_word=first_word;dut.copy_comp=0;
            force dut.command_sequence=COPY_SEQUENCE;
            dut.completed_sequence=0;dut.state=first_state;
        end
    endtask
    always @(posedge clk) if(!reset&&monitor_copy)begin : real_copy_monitor
        integer row,command_in_row,burst,word_index;
        reg expected_write;
        reg [22:0] expected_address;
        // Compute the number of live requests from interface events, rather
        // than using the implementation's arithmetic as the reference.
        check(dut.mem5_outstanding==2'(copy_accepted-copy_completed)&&
              dut.pending_memory[5]==(copy_accepted!=copy_completed),"COPY tracker matches accepted minus done");
        if(copy_completed<COPY_ROW_COMMANDS)begin
            check(dut.copy_source_row==COPY_SOURCE+23'd2&&
                  dut.copy_destination_row==COPY_DESTINATION+23'd2&&dut.copy_row_number==0,
                  "next source/destination row waits for previous physical writes");
        end
        if(copy_completed<2*COPY_ROW_COMMANDS)
            check(dut.completed_sequence==0&&!dut.host_cmd_pop&&!dut.color_valid[1],"COPY does not publish completion or target early");
        if(dut.mem_done[5])begin
            check(copy_completed<copy_accepted&&!dut.mem_error[5],"COPY completion retires an accepted request");
            copy_completed=copy_completed+1;
        end
        if(dut.mem_cmd_valid[5]&&dut.mem_cmd_ready[5])begin
            row=copy_accepted/COPY_ROW_COMMANDS;
            command_in_row=copy_accepted%COPY_ROW_COMMANDS;
            expected_write=command_in_row>=COPY_BURSTS;
            burst=command_in_row%COPY_BURSTS;
            expected_address=expected_write ? 23'(COPY_DESTINATION+1280*row+64*burst) :
                23'(COPY_SOURCE+COPY_PITCH*row+64*burst);
            check(dut.mem_cmd_write[5]==expected_write&&dut.mem_cmd_addr[115+:23]==expected_address&&
                  dut.mem_cmd_words[25+:5]==(burst==COPY_BURSTS-1?5'(COPY_WORDS-16*burst):5'd16),
                  "COPY commands stay in order at exact source and destination extents");
            if(!expected_write)begin
                check(copy_completed==copy_accepted,"COPY source does not overtake pending writes");
                fixture_read_address=expected_address;
            end
            copy_accepted=copy_accepted+1;
            if(copy_accepted-copy_completed>copy_peak)copy_peak=copy_accepted-copy_completed;
            check(copy_peak<=2,"COPY overlap stays within the two physical slots");
        end
        if(dut.mem_w_valid[5]&&dut.mem_w_ready[5])begin
            row=copy_payloads/COPY_WORDS;
            word_index=copy_payloads%COPY_WORDS;
            check(dut.mem_w_data[160+:32]==fixture_copy_word(23'(COPY_SOURCE+COPY_PITCH*row+4*word_index))&&
                  dut.mem_w_strb[20+:4]==fixture_copy_strobe(word_index),
                  "COPY staging preserves each 32-bit row word and odd endpoint masks");
            copy_payloads=copy_payloads+1;
            if(dut.foreground_quiet)quiet_copy_payloads=quiet_copy_payloads+1;
        end
        if(sd_dq_oe&&sd_dqm!=4'hf)begin
            row=copy_pins/COPY_WORDS;
            word_index=copy_pins%COPY_WORDS;
            check(sd_dq_out==fixture_copy_word(23'(COPY_SOURCE+COPY_PITCH*row+4*word_index))&&
                  sd_dqm==~fixture_copy_strobe(word_index),"COPY SDRAM pins preserve data and byte masks");
            copy_pins=copy_pins+1;
        end
    end
    task copy_write_overlap(input bit abort_copy);
        integer limit,before_writes;
        begin
            prepare();
            @(negedge clk);
            copy_accepted=0;copy_completed=0;copy_peak=0;copy_payloads=0;copy_pins=0;quiet_copy_payloads=0;
            before_writes=physical_writes;monitor_copy=1;
            start_copy_fixture(COPY_READ_CMD,0,2);
            limit=0;
            if(abort_copy)begin
                while(copy_accepted<COPY_BURSTS+2)begin
                    step();limit=limit+1;check(!fault&&limit<5000,"two COPY write commands are granted");
                end
                check(dut.mem5_outstanding==2,"abort starts with two real COPY writes in flight");
                @(negedge clk);abort_request=1;
                while(!dut.foreground_quiet)begin
                    step();limit=limit+1;check(!fault&&limit<5000,"COPY abort enters quiet");
                end
                while(dut.pending_memory[5])begin
                    check(!dut.renderer_reset&&!dut.host_abort,"both accepted COPY writes drain before reset");
                    step();limit=limit+1;check(!fault&&limit<5000,"accepted COPY writes retire");
                end
                check(copy_accepted==COPY_BURSTS+2&&copy_completed==copy_accepted&&
                      copy_payloads==32&&copy_pins==32&&quiet_copy_payloads>0&&
                      physical_writes-before_writes==4,"aborted COPY drains two writes without another command");
                check(dut.completed_sequence==0&&!dut.host_cmd_pop&&!dut.color_valid[1],"aborted COPY remains incomplete and invalid");
                monitor_copy=0;release dut.command_sequence;
                await_soft_recovery(2);
                $display("PASS two accepted COPY writes and masked payloads drain during abort");
            end else begin
                while(dut.completed_sequence!=COPY_SEQUENCE)begin
                    step();limit=limit+1;
                    if(fault||limit==5000)
                        $fatal(1,"COPY completion state=%0d accepted=%0d completed=%0d payload=%0d pins=%0d pending=%0d fault=%0d memory state=%0d word=%0d burst_end=%0d sequence=%h/%h",
                               dut.state,copy_accepted,copy_completed,copy_payloads,copy_pins,dut.mem5_outstanding,
                               fault_code,dut.memory_service.state,dut.copy_word,dut.copy_burst_end,
                               dut.completed_sequence,dut.command_sequence);
                end
                check(copy_accepted==2*COPY_ROW_COMMANDS&&copy_completed==copy_accepted&&copy_peak==2&&
                      copy_payloads==2*COPY_WORDS&&copy_pins==copy_payloads&&physical_writes-before_writes==10,
                      "two COPY rows complete with physical write overlap and all words masked correctly");
                check(!dut.copy_active&&!dut.pending_memory[5]&&dut.color_valid[1]&&dut.host_cmd_pop,
                      "retired COPY publishes target validity and sequence only after drain");
                monitor_copy=0;release dut.command_sequence;
                $display("PASS overlapped COPY writes preserve SDRAM masks and wait before the next row and command completion");
            end
        end
    endtask
    task copy_simultaneous_completion;
        integer index,limit;
        begin
            prepare();
            @(negedge clk);
            // The physical cases above do not force a particular alignment
            // between command accept and DONE. Hold the actual service idle
            // and control only these client handshakes for this exact race.
            force dut.memory_service.winner_valid=1'b0;
            force dut.mem_cmd_ready[5]=boundary_ready;
            force dut.mem_done[5]=boundary_done;
            force dut.mem_w_ready[5]=boundary_payload_ready;
            for(index=32;index<COPY_WORDS;index=index+1)
                dut.copy_row_ram.memory[index]=fixture_copy_word(23'(COPY_SOURCE+4*index));
            start_copy_fixture(COPY_WRITE_CMD,32,1);
            dut.mem5_outstanding=1;dut.pending_memory[5]=1;
            abort_request=1;
            repeat(2)step();
            @(negedge clk);boundary_ready=1;boundary_done=1;
            #1;check(dut.mem_cmd_valid[5]&&!dut.abort_pending,
                     "ready younger COPY write overlaps old DONE on the synchronized abort edge");
            step();
            check(dut.abort_pending&&dut.mem5_outstanding==1&&dut.pending_memory[5],
                  "same-edge COPY completion and accept retain the younger write");
            @(negedge clk);boundary_ready=0;boundary_done=0;boundary_payload_ready=1;
            limit=0;
            for(index=32;index<COPY_WORDS;index=index+1)begin
                while(!dut.mem_w_valid[5])begin
                    step();limit=limit+1;check(limit<32,"already accepted COPY loads its next RAM word");
                end
                check(dut.mem_w_data[160+:32]==fixture_copy_word(23'(COPY_SOURCE+4*index))&&
                      dut.mem_w_strb[20+:4]==fixture_copy_strobe(index),
                      "younger accepted COPY payload retains order and mask");
                step();
            end
            repeat(3)begin
                check(dut.foreground_quiet&&dut.state==COPY_WRITE_DONE&&dut.mem5_outstanding==1&&
                      !dut.renderer_reset&&dut.completed_sequence==0&&!dut.host_cmd_pop&&!dut.color_valid[1],
                      "quiet COPY waits for younger write physical completion without publishing");
                step();
            end
            @(negedge clk);boundary_payload_ready=0;boundary_done=1;
            step();check(!dut.pending_memory[5]&&dut.completed_sequence==0&&!dut.host_cmd_pop&&!dut.color_valid[1],
                         "final abort-time DONE drains without completing the command");
            @(negedge clk);boundary_done=0;
            release dut.mem_cmd_ready[5];release dut.mem_w_ready[5];release dut.mem_done[5];
            release dut.memory_service.winner_valid;release dut.command_sequence;
            await_soft_recovery(2);
            $display("PASS simultaneous COPY DONE and accept retain the younger write on the abort edge");
        end
    endtask
    task start_reference_read(input [22:0] address,input [20:0] count);
        integer limit;
        begin
            @(negedge clk);
            dut.list_address=address;dut.reference_count=count;dut.reference_index=0;dut.state=REF_CMD;
            limit=0;
            while(dut.state==REF_CMD)begin
                step();limit=limit+1;
                check(limit<5000,"SDRAM accepts reference window");
            end
            check(dut.state==REF_DATA&&dut.pending_memory[2],"accepted reference window remains tracked");
        end
    endtask
    task reference_abort_drain;
        integer limit,before_reads;
        begin
            prepare();
            reference_mode=1;reference_reads=0;reference_beats=0;quiet_reference_beats=0;
            reference_writes=0;fresh_record_reads=0;before_reads=physical_reads;
            start_reference_read(OLD_LIST,21'd8);
            limit=0;
            while(reference_beats<2)begin
                step();limit=limit+1;
                check(limit<5000,"reference window begins returning useful words");
            end
            check(reference_writes==1,"one successor pointer is written before abort");
            @(negedge clk);abort_request=1;
            while(!dut.foreground_quiet)begin
                step();limit=limit+1;
                check(limit<5000,"list abort enters quiescence");
            end
            check(dut.pending_memory[2]&&reference_beats<8,"quiet begins with reference words outstanding");
            while(dut.pending_memory[2])begin
                check(!dut.renderer_reset&&!dut.host_abort,"accepted reference window drains before reset");
                check(!dut.copy_ram_we&&!dut.copy_ram_re,"quiet consumes responses without enabling list RAM");
                step();limit=limit+1;
                check(limit<5000,"accepted reference window finishes");
            end
            check(reference_reads==1&&reference_beats==8&&quiet_reference_beats>0,
                  "aborted reference transaction consumed every accepted word and no younger request");
            check(physical_reads==before_reads+1,"SDRAM issued the accepted BL8 reference read");
            await_soft_recovery(2);

            reference_mode=2;reference_reads=0;reference_beats=0;reference_writes=0;
            // Refill the same native RAM slots in the new epoch. All
            // three records execute through SDRAM and the real renderer.
            start_reference_read(FRESH_LIST,21'd3);
            limit=0;
            while(dut.state!=STORE_CMD)begin
                step();limit=limit+1;
                check(!fault&&limit<5000,"fresh reference window renders and reaches the tile store");
            end
            check(reference_reads==1&&reference_beats==3&&reference_writes==2&&fresh_record_reads==6,
                  "one fresh three-word list reads and draws all exact records in order");
            reference_mode=0;
            $display("PASS accepted reference burst drains on abort and next epoch renders fresh pointers");
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
    task record_stream(input integer word_count, input [7:0] kind,
                       input [22:0] address, input bit range_fault);
        integer index,gap,requested_words,supplied_words;
        begin
            record_boundary_begin();
            dut.record_address=address;
            supplied_words=range_fault ? 16 : word_count;
            for(index=0;index<supplied_words;index=index+1)begin
                if(index!=0 && index%16==0)begin
                    @(negedge clk);record_response_valid=0;record_response_last=0;record_response_done=1;
                    step();check(dut.state==8'd31,"completed burst advances record command");
                    requested_words=word_count-index;
                    if(requested_words>16)requested_words=16;
                    check(dut.mem_cmd_words[10+:5]==5'(requested_words),"record command requests only the remaining words");
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
                record_response_data=index==0 ? {24'h000102,kind} : (32'ha5963cc3 ^ (index*32'h01020305));
                record_response_last=index%16==15 || index==supplied_words-1;
                record_response_valid=1;
                step();
            end
            @(negedge clk);record_response_valid=0;record_response_last=0;record_response_done=1;
            step();
            check(record_words_accepted==supplied_words && record_words_written==supplied_words,
                  "all supplied words retire at the record boundary");
            if(range_fault)
                check(dut.fault_pending && dut.fault_code==3 && !dut.mem_cmd_valid[2] && !dut.tile_cmd_valid,
                      "truncated heap record faults before followup or draw");
            else
                check(dut.state==8'd34 && dut.tile_cmd_valid && !dut.record_load_valid,"draw begins only after final staged write");
            @(negedge clk);record_response_done=0;
            step();check(!dut.tile_cmd_word_we,"no staged word leaks into active draw");
            record_boundary_end();
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
        if(reference_mode!=0)begin
            if(dut.mem_cmd_valid[2]&&dut.mem_cmd_ready[2])begin
                if(dut.state==REF_CMD)begin
                    check(dut.mem_cmd_addr[46+:23]==(reference_mode==1?OLD_LIST:FRESH_LIST)&&
                          dut.mem_cmd_words[10+:5]==(reference_mode==1?5'd8:5'd3),
                          "reference command uses the requested allocation and exact tail");
                    reference_reads=reference_reads+1;
                end else begin
                    check(reference_mode==2&&dut.state==RECORD_CMD&&fresh_record_reads<6,
                          "no record request escapes from an aborted list");
                    check(dut.mem_cmd_addr[46+:23]==23'(FRESH_RECORD+
                          128*(fresh_record_reads/2)+64*(fresh_record_reads%2))&&
                          dut.mem_cmd_words[10+:5]==(fresh_record_reads%2==0?5'd16:5'd4),
                          "fresh pointer order and kind-specific record bursts");
                    fresh_record_reads=fresh_record_reads+1;
                end
            end
            if(dut.state==REF_DATA&&dut.mem_r_valid[2])begin
                check(dut.mem_r_ready[2],"reference data can retire under abort");
                check(dut.mem_r_data[64+:32]==(reference_mode==1?OLD_RECORD:FRESH_RECORD)+128*reference_beats,
                      "physical service delivers reference words in order");
                if(dut.foreground_quiet)quiet_reference_beats=quiet_reference_beats+1;
                if(dut.abort_pending||dut.fault_pending||dut.foreground_quiet)
                    check(!dut.copy_ram_we&&!dut.copy_ram_re,"abort blocks retaining younger reference data");
                else if(reference_beats!=0)begin
                    check(dut.copy_ram_we&&dut.copy_ram_waddr==9'(reference_beats)&&
                          dut.copy_ram_wdata==dut.mem_r_data[64+:32],"later reference fills the matching native slot");
                    reference_writes=reference_writes+1;
                end
                reference_beats=reference_beats+1;
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
        copy_write_overlap(0);
        copy_write_overlap(1);
        copy_simultaneous_completion();
        reference_abort_drain();
        record_stream(80,0,23'h7ffec0,0);
        $display("PASS eighty record words retain one-edge latency, burst/gap order and final-word-before-launch");
        record_stream(20,1,23'h7fffb0,0);
        record_stream(22,2,23'h7fffa8,0);
        record_stream(20,3,23'h7fffb0,0);
        record_stream(20,1,23'h7fffb4,1);
        record_stream(22,2,23'h7fffac,1);
        record_stream(20,3,23'h7fffb4,1);
        $display("PASS 2D record fetches use exact tail size and reject out-of-heap tails");
        record_cancel(0);
        record_cancel(1);
        record_cancel(2);
        $display("PASS %0d recovery checks: exact entry races, native payload draining, fault/abort and resumed idle",checks);
        $finish;
    end
    initial begin #2000000;$fatal(1,"recovery testbench timeout");end
endmodule
