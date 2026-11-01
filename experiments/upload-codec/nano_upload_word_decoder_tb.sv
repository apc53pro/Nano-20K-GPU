`timescale 1ns/1ps
module nano_upload_word_decoder_tb;
    reg clk=0;
    always #5 clk=~clk;
    reg reset=1,start=0,s_valid=0,m_ready=0;
    reg [7:0] s_data=0;
    wire start_ready,s_ready,m_valid,busy,done;
    wire [31:0] m_data;
    wire [6:0] m_index;
    nano_upload_word_decoder dut(.*);
    reg [31:0] expected[0:79];
    reg [7:0] encoded[0:339];
    integer encoded_length,total_records=0,total_words=0,total_encoded_bytes=0;
    integer total_cycles=0,checks=0;
    reg [31:0] random_state=32'h51ecda7a;
    integer i,j,k,pattern;
    integer encoded_file,expected_file,lengths_file,record_count=0,scan_status;
    reg [2047:0] encoded_path,expected_path,lengths_path;

    function automatic [1:0] width_code(input [31:0] value);
        begin
            if(value==0) width_code=0;
            else if(value[31:8]=={24{value[7]}}) width_code=1;
            else if(value[31:16]=={16{value[15]}}) width_code=2;
            else width_code=3;
        end
    endfunction
    task automatic encode_record;
        reg [1:0] code;
        integer w,n,b,group_index,header;
        begin
            encoded_length=0;
            for(group_index=0;group_index<20;group_index=group_index+1) begin
                header=encoded_length;encoded[header]=0;encoded_length=encoded_length+1;
                for(w=4*group_index;w<4*group_index+4;w=w+1) begin
                    code=width_code(expected[w]);
                    encoded[header]=encoded[header] | ({6'd0,code} << (2*(w%4)));
                    n=code==0 ? 0 : code==1 ? 1 : code==2 ? 2 : 4;
                    for(b=0;b<n;b=b+1) begin
                        encoded[encoded_length]=expected[w] >> (8*b);
                        encoded_length=encoded_length+1;
                    end
                end
            end
        end
    endtask

    task automatic run_encoded(input [255:0] label_name,input integer stalls);
        integer tx,rx,cycles;
        reg held_input,previous_stall,finished;
        reg [31:0] stalled_data;
        reg [6:0] stalled_index;
        begin
            @(negedge clk);
            if(!start_ready || busy || done) $fatal(1,"decoder not idle before record");
            start=1; s_valid=0; m_ready=0;
            @(posedge clk); #1;
            if(!busy || start_ready) $fatal(1,"start handshake failed");
            @(negedge clk); start=0;
            tx=0; rx=0; cycles=0; held_input=0; previous_stall=0; finished=0;
            while(!finished) begin
                // Once offered, an input stays valid until accepted.
                if(!held_input && tx<encoded_length &&
                   (stalls==0 || ((cycles%7)!=1 && (cycles%11)!=4))) held_input=1;
                s_valid=held_input;
                s_data=tx<encoded_length ? encoded[tx] : 8'ha5;
                m_ready=stalls==0 || ((cycles%9)>2 && (cycles%13)!=7);
                @(posedge clk);
                if(previous_stall && (!m_valid || m_data!==stalled_data || m_index!==stalled_index))
                    $fatal(1,"output changed under backpressure");
                if(s_valid && s_ready) begin tx=tx+1; held_input=0; end
                if(m_valid && m_ready) begin
                    if(rx>=80 || m_index!==rx[6:0] || m_data!==expected[rx])
                        $fatal(1,"record %0d word %0d index %0d actual %h expected %h",total_records,rx,m_index,m_data,expected[rx]);
                    rx=rx+1; checks=checks+1;
                end
                previous_stall=m_valid && !m_ready;
                stalled_data=m_data; stalled_index=m_index;
                cycles=cycles+1;
                #1;
                if(done) begin
                    if(rx!=80 || tx!=encoded_length || busy || !start_ready)
                        $fatal(1,"completion framing failure rx=%0d tx=%0d length=%0d",rx,tx,encoded_length);
                    finished=1;
                end
                if(cycles>5000) $fatal(1,"decoder timeout");
                if(!finished) @(negedge clk);
            end
            // For an always-ready sink:20 descriptor cycles, one output cycle
            // per word, plus one cycle per consumed payload byte.
            if(stalls==0 && cycles!=encoded_length+80)
                $fatal(1,"unexpected no-stall cycle count %0d expected %0d",cycles,encoded_length+80);
            total_records=total_records+1; total_words=total_words+rx;
            total_encoded_bytes=total_encoded_bytes+tx; total_cycles=total_cycles+cycles;
            $display("RECORD label=%0s stalls=%0d encoded_bytes=%0d expanded_bytes=320 cycles=%0d",label_name,stalls,tx,cycles);
            @(negedge clk); s_valid=0; m_ready=0;
            @(posedge clk); #1;
            if(done || m_valid || busy) $fatal(1,"completion did not return to idle");
        end
    endtask

    task automatic cancel_partial(input integer bytes_to_send);
        integer sent;
        begin
            @(negedge clk);start=1;m_ready=1;s_valid=0;
            @(posedge clk);#1;
            @(negedge clk);start=0;
            sent=0;
            while(sent<bytes_to_send) begin
                s_valid=1;s_data=encoded[sent];
                @(posedge clk);
                if(s_ready) sent=sent+1;
                @(negedge clk);
            end
            reset=1;s_valid=0;
            @(posedge clk);#1;
            if(busy || done || m_valid || s_ready || start_ready) $fatal(1,"reset did not cancel record");
            @(negedge clk);reset=0;
            @(posedge clk);#1;
            if(!start_ready || busy || done) $fatal(1,"reset recovery failed");
            checks=checks+1;
        end
    endtask

    initial begin
        repeat(3) @(posedge clk);
        @(negedge clk);reset=0;
        for(pattern=0;pattern<8;pattern=pattern+1) begin
            for(i=0;i<80;i=i+1) begin
                case(pattern)
                0: expected[i]=0;
                1: expected[i]=$signed(8'(i*7-128));
                2: expected[i]=$signed(16'(i*719-32768));
                3: expected[i]=32'h12345678^(32'h10101*i);
                4: case(i%8)
                    0:expected[i]=32'h0000007f;1:expected[i]=32'hffffff80;
                    2:expected[i]=32'h00000080;3:expected[i]=32'hffffff7f;
                    4:expected[i]=32'h00007fff;5:expected[i]=32'hffff8000;
                    6:expected[i]=32'h00008000;7:expected[i]=32'hffff7fff;
                   endcase
                5:expected[i]=i==0 ? 32'h80000000 : i==79 ? 32'h7fffffff : 0;
                default:begin
                    random_state=random_state*32'd1664525+32'd1013904223;
                    case(i%4)
                    0:expected[i]=0;
                    1:expected[i]={{24{random_state[7]}},random_state[7:0]};
                    2:expected[i]={{16{random_state[15]}},random_state[15:0]};
                    3:expected[i]=random_state;
                    endcase
                end
                endcase
            end
            encode_record();run_encoded("synthetic",0);run_encoded("synthetic",1);
        end
        // Exercise cancellation, then prove each fresh group descriptor
        // replaces every descriptor bit after reset.
        cancel_partial(7);cancel_partial(21);
        encode_record();run_encoded("after-reset",1);
        for(pattern=0;pattern<32;pattern=pattern+1) begin
            for(i=0;i<80;i=i+1) begin
                random_state=random_state*32'd1664525+32'd1013904223;
                expected[i]=random_state;
            end
            encode_record();run_encoded("random-raw",pattern%2);
        end
        if($value$plusargs("encoded=%s",encoded_path)) begin
            if(!$value$plusargs("expected=%s",expected_path) ||
               !$value$plusargs("lengths=%s",lengths_path) ||
               !$value$plusargs("records=%d",record_count)) $fatal(1,"incomplete external vector arguments");
            encoded_file=$fopen(encoded_path,"r");expected_file=$fopen(expected_path,"r");lengths_file=$fopen(lengths_path,"r");
            if(!encoded_file || !expected_file || !lengths_file) $fatal(1,"could not open external vectors");
            for(k=0;k<record_count;k=k+1) begin
                scan_status=$fscanf(lengths_file,"%h",encoded_length);
                if(scan_status!=1 || encoded_length<20 || encoded_length>340) $fatal(1,"bad vector length");
                for(j=0;j<80;j=j+1) begin
                    scan_status=$fscanf(expected_file,"%h",expected[j]);
                    if(scan_status!=1) $fatal(1,"missing expected word");
                end
                for(j=0;j<encoded_length;j=j+1) begin
                    scan_status=$fscanf(encoded_file,"%h",encoded[j]);
                    if(scan_status!=1) $fatal(1,"missing encoded byte");
                end
                run_encoded("demo",k%2);
            end
            $fclose(encoded_file);$fclose(expected_file);$fclose(lengths_file);
        end
        $display("PASS upload word decoder records=%0d words=%0d encoded_bytes=%0d cycles=%0d checks=%0d",total_records,total_words,total_encoded_bytes,total_cycles,checks);
        $finish;
    end
    initial begin #100000000;$fatal(1,"global timeout");end
endmodule
