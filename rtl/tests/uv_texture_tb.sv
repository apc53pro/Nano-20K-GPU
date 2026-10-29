`timescale 1ns/1ps
module uv_texture_tb;
    reg clk=0;always #5 clk=~clk;
    reg reset=1,ce=1,in_valid=0,out_ready=1;
    reg signed [63:0] in_u=0,in_v=0,in_q=0;
    reg [5:0] in_tag=0;
    wire in_ready,out_valid,out_overflow,out_invalid_q;
    wire signed [31:0] out_u,out_v;
    wire [5:0] out_tag;
    nano_uv_texture_pipeline dut(clk,reset,ce,in_valid,in_ready,in_u,in_v,in_q,in_tag,
        out_valid,out_ready,out_u,out_v,out_tag,out_overflow,out_invalid_q);
    reg [63:0] values_u[0:499999],values_v[0:499999],values_q[0:499999];
    reg [31:0] expected_u[0:499999],expected_v[0:499999];
    reg expected_overflow[0:499999],expected_invalid[0:499999];
    integer file,got,count,accepted,retired,cycle,stalls,phase;
    reg [1023:0] filename;
    reg signed [31:0] held_u,held_v;
    reg [5:0] held_tag;
    reg held_valid,held_overflow,held_invalid;
    initial begin
        if(!$value$plusargs("vectors=%s",filename)) $fatal(1,"missing vectors path");
        file=$fopen(filename,"r");if(!file)$fatal(1,"cannot open vectors");
        count=0;
        while(!$feof(file)) begin
            got=$fscanf(file,"%h %h %h %h %h %h %h\n",values_u[count],values_v[count],values_q[count],
                expected_u[count],expected_v[count],expected_overflow[count],expected_invalid[count]);
            if(got==7)count=count+1;else if(!$feof(file))$fatal(1,"invalid vector");
        end
        $fclose(file);
        if(count<30000)$fatal(1,"too few adversarial vectors");
        accepted=0;retired=0;cycle=0;stalls=0;
        repeat(2)@(negedge clk);reset=0;
        // First all-hit stream provesII1; the second half injects both global
        // CE stalls and independent output backpressure, retaining all tokens.
        while(retired<count)begin
            @(negedge clk);
            cycle=cycle+1;
            ce=accepted<count/2 || (cycle%7!=2 && cycle%11!=3);
            out_ready=accepted<count/2 || cycle%5!=1;
            in_valid=accepted<count;
            if(in_valid)begin
                in_u=values_u[accepted];in_v=values_v[accepted];in_q=values_q[accepted];in_tag=accepted%64;
            end
            held_u=out_u;held_v=out_v;held_tag=out_tag;
            held_valid=out_valid;held_overflow=out_overflow;held_invalid=out_invalid_q;
            @(posedge clk);
            if(ce && out_ready && out_valid)begin
                if(retired<count/2-19 && cycle!=retired+20)
                    $fatal(1,"19-edge latency/II1 mismatch at%0d cycle%0d",retired,cycle);
                if(out_tag!==retired%64)$fatal(1,"context tag mismatch at%0d",retired);
                if(out_invalid_q!==expected_invalid[retired])$fatal(1,"invalid-q flag mismatch at%0d",retired);
                if(!expected_invalid[retired] && out_overflow!==expected_overflow[retired])
                    $fatal(1,"overflow flag mismatch at%0d",retired);
                if(!expected_invalid[retired] && !expected_overflow[retired] &&
                    (out_u!==($signed(expected_u[retired])>>>16) || out_v!==($signed(expected_v[retired])>>>16)))
                    $fatal(1,"texel mismatch at%0d: U%h/%h V%h/%h q%h",retired,out_u,($signed(expected_u[retired])>>>16),out_v,($signed(expected_v[retired])>>>16),values_q[retired]);
                retired=retired+1;
            end
            if(in_ready && in_valid)accepted=accepted+1;
            if(!ce || (held_valid && !out_ready))begin
                stalls=stalls+1;
                #1;
                if({out_valid,out_u,out_v,out_tag,out_overflow,out_invalid_q} !==
                    {held_valid,held_u,held_v,held_tag,held_overflow,held_invalid})
                    $fatal(1,"backpressure changed a held output");
            end
            if(cycle>1500000)$fatal(1,"stream made no bounded progress");
        end
        $display("RTL texture coordinates: %0d exact vectors, %0d accepted/retired, %0d stalled edges",count,retired,stalls);
        // Reset cancellation and reset priority over CE: no old valid token
        // may reappear when the pipeline resumes.
        @(negedge clk);in_valid=1;ce=1;out_ready=1;
        repeat(10)@(negedge clk);
        reset=1;ce=0;
        @(negedge clk);reset=0;in_valid=0;ce=1;
        repeat(30)begin @(posedge clk);#1;if(out_valid)$fatal(1,"reset leaked an old token");end
        $display("RTL reset cancellation and CE priority passed");$finish;
    end
endmodule
