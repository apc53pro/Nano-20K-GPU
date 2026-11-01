`timescale 1ns/1ps
// Standalone decoder workload measurement. No SDRAM, packet parser or clock
// crossing is modeled here. The paced source is an explicit synthetic source.
module upload_codec_traffic_tb;
    reg clk=0;
    always #5 clk=~clk;
    reg reset=1,start=0,s_valid=0,m_ready=0;
    reg [7:0] s_data=0;
    wire start_ready,s_ready,m_valid,busy,done;
    wire [31:0] m_data;
    wire [6:0] m_index;
    nano_upload_word_decoder dut(.*);
    reg [7:0] encoded[0:1048575];
    reg [31:0] expected[0:1048575];
    reg [15:0] lengths[0:16383];
    reg [4095:0] encoded_path,expected_path,lengths_path,csv_path;
    integer records,encoded_bytes,mode,sink_period,csv;
    integer rec,tx,rx,offset,cycles,phase,total_cycles,total_input_stalls;
    integer record_input_stalls,slots;
    reg pending,finished,previous_stall;
    reg [31:0] stalled_data;
    reg [6:0] stalled_index;
    initial begin
        if(!$value$plusargs("encoded=%s",encoded_path) ||
           !$value$plusargs("expected=%s",expected_path) ||
           !$value$plusargs("lengths=%s",lengths_path) ||
           !$value$plusargs("csv=%s",csv_path) ||
           !$value$plusargs("records=%d",records) ||
           !$value$plusargs("bytes=%d",encoded_bytes))
            $fatal(1,"missing workload arguments");
        mode=0;sink_period=1;
        if($value$plusargs("mode=%d",mode)) begin end
        if($value$plusargs("sink_period=%d",sink_period)) begin end
        if(records<1 || records>13107 || encoded_bytes<1 || encoded_bytes>1048576 ||
           mode<0 || mode>1 || sink_period<1 || sink_period>16)
            $fatal(1,"invalid workload bounds");
        $readmemh(encoded_path,encoded,0,encoded_bytes-1);
        $readmemh(expected_path,expected,0,records*80-1);
        $readmemh(lengths_path,lengths,0,records-1);
        csv=$fopen(csv_path,"w");
        if(!csv) $fatal(1,"cannot create cycle report");
        $fdisplay(csv,"record,encoded_bytes,expanded_bytes,core_cycles,input_stall_cycles");
        repeat(3) @(posedge clk);
        @(negedge clk);reset=0;#1;
        offset=0;total_cycles=0;total_input_stalls=0;
        for(rec=0;rec<records;rec=rec+1) begin
            if(!start_ready || busy) $fatal(1,"not idle before record");
            start=1;
            @(posedge clk);#1;
            @(negedge clk);start=0;
            tx=0;rx=0;cycles=0;phase=26;pending=0;finished=0;
            record_input_stalls=0;previous_stall=0;
            while(!finished) begin
                // At most one source slot per 41/15 core cycles: 25.2 MHz
                // offered bytes at an assumed 68.88 MHz core. A pending byte
                // stays valid; missed slots are discarded, not accumulated.
                slots=0;
                if(mode==1) begin
                    phase=phase+15;
                    if(phase>=41) begin phase=phase-41;slots=1;end
                end else slots=1;
                if(slots && !pending && tx<lengths[rec]) pending=1;
                s_valid=pending;
                s_data=tx<lengths[rec] ? encoded[offset+tx] : 8'd0;
                m_ready=(cycles%sink_period)==0;
                @(posedge clk);
                if(previous_stall && (!m_valid || m_data!==stalled_data || m_index!==stalled_index))
                    $fatal(1,"output changed during stall");
                if(s_valid && !s_ready) record_input_stalls=record_input_stalls+1;
                if(s_valid && s_ready) begin tx=tx+1;pending=0;end
                if(m_valid && m_ready) begin
                    if(rx>=80 || m_index!==rx[6:0] || m_data!==expected[rec*80+rx])
                        $fatal(1,"record %0d word %0d got %h expected %h",rec,rx,m_data,expected[rec*80+rx]);
                    rx=rx+1;
                end
                previous_stall=m_valid && !m_ready;
                stalled_data=m_data;stalled_index=m_index;
                cycles=cycles+1;
                #1;
                if(done) begin
                    if(rx!=80 || tx!=lengths[rec] || busy || !start_ready)
                        $fatal(1,"record completion mismatch");
                    finished=1;
                end
                if(cycles>10000) $fatal(1,"workload timeout");
                if(!finished) @(negedge clk);
            end
            if(mode==0 && sink_period==1 && cycles!=lengths[rec]+80)
                $fatal(1,"unexpected no-stall cycle count");
            $fdisplay(csv,"%0d,%0d,320,%0d,%0d",rec,lengths[rec],cycles,record_input_stalls);
            total_cycles=total_cycles+cycles;
            total_input_stalls=total_input_stalls+record_input_stalls;
            offset=offset+lengths[rec];
            @(negedge clk);s_valid=0;m_ready=0;
        end
        if(offset!=encoded_bytes) $fatal(1,"incorrect consumed workload length");
        $fclose(csv);
        $display("PASS workload records=%0d words=%0d bytes=%0d mode=%0d sink_period=%0d cycles=%0d input_stalls=%0d",records,records*80,offset,mode,sink_period,total_cycles,total_input_stalls);
        $finish;
    end
endmodule
