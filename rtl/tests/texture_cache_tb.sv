`timescale 1ns/1ps
module texture_cache_tb;
    reg clk=0; always #5 clk=~clk;
    reg reset=1,flush=0,cfg_valid=0,req_valid=0,rsp_ready=1;
    wire cfg_ready,req_ready,rsp_valid,fault;
    reg [22:0] cfg_base=0,cfg_pitch=0;
    reg [10:0] cfg_width=0,cfg_height=0;
    reg cfg_repeat_u=0,cfg_repeat_v=0;
    reg signed [31:0] req_x=0,req_y=0;
    reg [5:0] req_tag=0;
    wire [15:0] rsp_texel;
    wire [5:0] rsp_tag;
    wire mem_cmd_valid,mem_cmd_write,mem_w_valid,mem_r_ready;
    reg mem_cmd_ready=1,mem_w_ready=0,mem_r_valid=0,mem_r_last=0,mem_done=0,mem_error=0;
    wire [22:0] mem_cmd_addr;
    wire [4:0] mem_cmd_words;
    wire [31:0] mem_w_data;
    wire [3:0] mem_w_strb;
    reg [31:0] mem_r_data=0;
    reg stall_outputs=0,warm_timing=0;
    reg [15:0] memory_version=0;
    reg expected_fault=0,held_output=0,held_command=0,inject_error=0;
    reg [15:0] held_texel=0;
    reg [5:0] held_tag=0;
    reg [22:0] held_address=0;
    reg [4:0] held_words=0;
    integer cycle=0,sent=0,returned=0,cancelled=0,transactions=0,max_run=0,run=0,last_output=-100;
    reg [15:0] expected[0:8191];
    reg [5:0] expected_tag[0:8191];
    integer accepted_cycle[0:8191];
    reg active=0;
    reg [22:0] request_base=0;
    integer request_words=0,request_index=0,delay_cycles=0;
    nano_texture_cache dut(.*);
    function [15:0] texel;
        input [22:0] byte_address;
        begin texel = ((byte_address >> 1)*32'h45d9 + 32'h1237) ^ memory_version; end
    endfunction
    function integer coord;
        input integer value,size;
        input repeating;
        begin
            if(repeating) coord=value&(size-1);
            else if(value<0)coord=0;
            else if(value>=size)coord=size-1;
            else coord=value;
        end
    endfunction
    always @(negedge clk) begin
        if(stall_outputs) rsp_ready=(cycle%11)<6;
        else rsp_ready=1;
        mem_cmd_ready=(cycle%7)<4;
    end
    always @(posedge clk) begin
        cycle<=cycle+1;
        if(!reset) begin
            if(held_output&&(rsp_valid!==1'b1||rsp_texel!==held_texel||rsp_tag!==held_tag))
                $fatal(1,"texture response changed while backpressured");
            held_output<=rsp_valid&&!rsp_ready;
            held_texel<=rsp_texel;held_tag<=rsp_tag;
            if(held_command&&(!mem_cmd_valid||mem_cmd_addr!==held_address||mem_cmd_words!==held_words))
                $fatal(1,"refill command changed while backpressured");
            held_command<=mem_cmd_valid&&!mem_cmd_ready;
            held_address<=mem_cmd_addr;held_words<=mem_cmd_words;
            if(req_valid&&req_ready)begin
                expected[sent]=texel(cfg_base+coord(req_y,cfg_height,cfg_repeat_v)*cfg_pitch+
                    2*coord(req_x,cfg_width,cfg_repeat_u));
                expected_tag[sent]=req_tag;accepted_cycle[sent]=cycle;sent=sent+1;
            end
            if(rsp_valid&&rsp_ready)begin
                if(returned>=sent)$fatal(1,"unsolicited texture result");
                if(rsp_texel!==expected[returned]||rsp_tag!==expected_tag[returned])
                    $fatal(1,"texture result %0d differs: got %h/%0d wanted %h/%0d",returned,
                        rsp_texel,rsp_tag,expected[returned],expected_tag[returned]);
                if(warm_timing&&cycle-accepted_cycle[returned]!=7)
                    $fatal(1,"warm hit handshake latency differs from seven cycles");
                if(cycle==last_output+1)run=run+1;else run=1;
                if(run>max_run)max_run=run;
                last_output=cycle;returned=returned+1;
            end
            if(fault&&!expected_fault)$fatal(1,"unexpected texture fault at cycle %0d state %0d",cycle,dut.state);
        end else begin held_output<=0;held_command<=0;end
        mem_done<=0;
        mem_error<=0;
        if(reset)begin active<=0;mem_r_valid<=0;mem_r_last<=0;end
        else begin
            if(mem_cmd_valid&&mem_cmd_ready)begin
                if(active||mem_cmd_write||mem_cmd_words==0||mem_cmd_words>8||mem_cmd_addr[1:0]!=0)
                    $fatal(1,"invalid or overlapping cache refill command");
                if(mem_cmd_addr<cfg_base||mem_cmd_addr+4*mem_cmd_words>cfg_base+cfg_pitch*cfg_height)
                    $fatal(1,"cache refill read outside declared allocation");
                active<=1;request_base<=mem_cmd_addr;request_words<=mem_cmd_words;
                request_index<=0;delay_cycles<=3;transactions=transactions+1;
            end
            if(active&&!mem_r_valid)begin
                if(delay_cycles!=0)delay_cycles<=delay_cycles-1;
                else begin
                    mem_r_valid<=1;mem_r_last<=request_index+1==request_words;
                    mem_r_data<={texel(request_base+4*request_index+2),texel(request_base+4*request_index)};
                end
            end
            if(mem_r_valid&&mem_r_ready)begin
                if(request_index+1==request_words)begin
                    active<=0;mem_r_valid<=0;mem_r_last<=0;mem_done<=1;
                end else begin
                    request_index<=request_index+1;
                    mem_r_last<=request_index+2==request_words;
                    mem_r_data<={texel(request_base+4*(request_index+1)+2),
                                  texel(request_base+4*(request_index+1))};
                end
            end
            if(active&&inject_error)begin
                mem_error<=1;active<=0;mem_r_valid<=0;mem_r_last<=0;
            end
        end
        if(cycle>200000)$fatal(1,"texture cache test timed out");
    end
    task configure;
        input [22:0] address,stride;
        input [10:0] columns,rows;
        input wrap_u,wrap_v;
        begin
            @(negedge clk);cfg_base=address;cfg_pitch=stride;
            cfg_width=columns;cfg_height=rows;cfg_repeat_u=wrap_u;cfg_repeat_v=wrap_v;cfg_valid=1;
            while(!cfg_ready)@(negedge clk);
            @(posedge clk);@(negedge clk);cfg_valid=0;
        end
    endtask
    task drain;
        begin
            @(negedge clk);req_valid=0;
            while(returned!=sent||!cfg_ready)@(negedge clk);
        end
    endtask
    task send_one;
        input integer x,y,tag;
        begin
            @(negedge clk);req_valid=1;req_x=x;req_y=y;req_tag=tag;
            while(!req_ready)@(negedge clk);
            @(posedge clk);
        end
    endtask
    integer k,before_transactions;
    initial begin
        repeat(4)@(negedge clk);reset=0;
        configure(23'h258000,23'd32,11'd16,11'd64,0,0);
        for(k=0;k<64;k=k+1)send_one(0,k,k%64);
        drain();
        if(transactions!=64)$fatal(1,"prewarm did not fetch each of 64 lines exactly once");
        before_transactions=transactions;max_run=0;run=0;last_output=-100;warm_timing=1;
        for(k=0;k<1024;k=k+1)send_one(k%16,k/16,k%64);
        drain();
        if(transactions!=before_transactions||max_run!=1024)
            $fatal(1,"warm cache failed II1: requests %0d run %0d",transactions-before_transactions,max_run);
        warm_timing=0;
        configure(23'h258004,23'd4,11'd1,11'd1,0,0);
        stall_outputs=1;
        for(k=0;k<48;k=k+1)send_one(k-24,24-k,k%64);
        drain();
        configure(23'h258000,23'd32,11'd16,11'd128,1,1);
        for(k=0;k<128;k=k+1)send_one(k-80,(k%2)*64,k%64);
        drain();
        configure(23'h258000,23'd262148,11'd8,11'd2,1,0);
        for(k=0;k<64;k=k+1)send_one(k-40,k%5-2,k%64);
        drain();stall_outputs=0;
        configure(23'h258000,23'd2048,11'd1024,11'd1024,1,0);
        for(k=0;k<32;k=k+1)send_one((k%4)*1023-512,(k/4)*257-512,k%64);
        drain();
        configure(23'h258004,23'd16,11'd8,11'd1,0,1);
        for(k=0;k<16;k=k+1)send_one(k-4,k-8,k%64);
        drain();
        @(negedge clk);flush=1;memory_version=16'h5aa5;
        @(negedge clk);flush=0;
        for(k=0;k<32;k=k+1)send_one(k%8,k%2,k%64);
        drain();
        expected_fault=1;
        configure(23'h258000,23'd8,11'd3,11'd1,0,0);
        repeat(3)@(negedge clk);
        if(!fault)$fatal(1,"invalid non-power-of-two texture was not rejected");
        reset=1;repeat(3)@(negedge clk);reset=0;expected_fault=0;
        configure(23'h7ffffc,23'd4,11'd1,11'd1,1,1);
        for(k=0;k<32;k=k+1)send_one(k-16,16-k,k%64);
        drain();
        configure(23'h258000,23'd32,11'd16,11'd64,0,0);
        expected_fault=1;inject_error=1;
        send_one(0,0,17);
        @(negedge clk);req_valid=0;
        while(!fault)@(negedge clk);
        inject_error=0;
        if(sent-returned!=1)$fatal(1,"memory error did not cancel exactly its pending sample");
        cancelled=cancelled+sent-returned;
        returned=sent; // The failed sample is cancelled, never returned as a fabricated texel.
        reset=1;repeat(3)@(negedge clk);reset=0;expected_fault=0;
        configure(23'h258000,23'd32,11'd16,11'd64,0,0);
        send_one(7,9,3);drain();
        $display("PASS: %0d accepted (%0d returned, %0d cancelled), %0d bounded refills, 1024 consecutive warm outputs",
            sent,returned-cancelled,cancelled,transactions);
        $finish;
    end
endmodule
