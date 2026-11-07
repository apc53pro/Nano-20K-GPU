// The service accepts a command tuple before validating its retained fields.
// Exercise ownership, refresh and cancellation separately from the physical
// SDRAM pin regression, which checks data transfers and timing constraints.
module nano_sdram_accept_check_tb;
    timeunit 1ns; timeprecision 1ps;
    reg clk=0, reset=1;
    reg [5:0] cmd_valid=0, cmd_write=0, w_valid=0, r_ready=6'h3f;
    reg [137:0] cmd_addr=0;
    reg [29:0] cmd_words=0;
    reg [191:0] w_data=0;
    reg [23:0] w_strb=24'hffffff;
    wire [5:0] cmd_ready,w_ready,r_valid,r_last,done,error;
    wire [191:0] r_data;
    wire initialized,sd_clk,sd_cke,sd_cs_n,sd_ras_n,sd_cas_n,sd_we_n,sd_dq_oe;
    wire [1:0] sd_ba;
    wire [10:0] sd_addr;
    wire [31:0] sd_dq_out;
    wire [3:0] sd_dqm;
    integer checks=0, cases=0, edges=0;
    nano_sdram_service #(.POWERUP_CYCLES(1)) dut(
        .clk(clk),.reset(reset),.cmd_valid(cmd_valid),.cmd_ready(cmd_ready),.cmd_write(cmd_write),
        .cmd_addr(cmd_addr),.cmd_words(cmd_words),.w_valid(w_valid),.w_ready(w_ready),
        .w_data(w_data),.w_strb(w_strb),.r_valid(r_valid),.r_ready(r_ready),.r_data(r_data),
        .r_last(r_last),.done(done),.error(error),.initialized(initialized),.sd_clk(sd_clk),
        .sd_cke(sd_cke),.sd_cs_n(sd_cs_n),.sd_ras_n(sd_ras_n),.sd_cas_n(sd_cas_n),.sd_we_n(sd_we_n),
        .sd_ba(sd_ba),.sd_addr(sd_addr),.sd_dq_in(32'b0),.sd_dq_out(sd_dq_out),.sd_dq_oe(sd_dq_oe),.sd_dqm(sd_dqm));
    task require(input bit condition,input string why);
        begin checks=checks+1; if(!condition)$fatal(1,"edge %0d case %0d: %s",edges,cases,why); end
    endtask
    task tick;
        begin #4;clk=1;#1;edges=edges+1;clk=0;#1;end
    endtask
    task no_write;
        begin require(!(!sd_cs_n&&sd_ras_n&&!sd_cas_n&&!sd_we_n),"unexpected physical WRITE command");end
    endtask
    task fresh;
        integer guard;
        begin
            cmd_valid=0;cmd_write=0;cmd_addr=0;cmd_words=0;w_valid=0;r_ready=6'h3f;
            reset=1;tick();tick();reset=0;guard=0;
            while(!initialized||dut.state!=9)begin tick();guard=guard+1;require(guard<80,"initialization timeout");no_write();end
            require(!dut.read_stream_active&&!dut.read_output_valid,"fresh response pipeline not empty");
        end
    endtask
    task check_quiet;
        begin
            require(dut.state==22,"accepted request did not enter CHECK22");
            require(cmd_ready==0&&w_ready==0,"CHECK accepted a second command or early write payload");
            require(!dut.ram_we&&!dut.ram_re,"CHECK touched transaction RAM");
            require(done==0&&error==0,"CHECK acceptance completed before validation");
            no_write();
        end
    endtask
    task tuple(input integer client,input bit writing,input [22:0]address,input [4:0]words);
        begin
            require(dut.owner==client,"captured owner changed");
            require(dut.request_address==address&&dut.request_words==words&&dut.request_write==writing,"captured command tuple changed");
        end
    endtask
    task accept(input integer client,input bit writing,input [22:0]address,input [4:0]words);
        begin
            require(dut.state==9&&initialized,"accept task requires initialized IDLE");
            cmd_valid=6'b1<<client;cmd_write=writing?(6'b1<<client):0;
            cmd_addr={138{1'b1}};cmd_words={30{1'b1}};
            cmd_addr[client*23+:23]=address;cmd_words[client*5+:5]=words;
            w_valid=6'h3f;#1;require(cmd_ready==(6'b1<<client),"unexpected IDLE ready grant");
            tick();check_quiet();tuple(client,writing,address,words);
            // All live request inputs may be withdrawn or changed immediately after acceptance.
            cmd_valid=0;cmd_write=~cmd_write;cmd_addr=0;cmd_words=0;
            cmd_addr[((client+1)%6)*23+:23]=23'h7fffff;cmd_words[((client+1)%6)*5+:5]=31;
            #1;check_quiet();tuple(client,writing,address,words);
        end
    endtask
    task validate(input integer client,input bit writing,input [22:0]address,input [4:0]words,input bit invalid);
        begin
            tick();tuple(client,writing,address,words);no_write();
            if(invalid)begin
                require(dut.state==9,"invalid request advanced beyond validation");
                require(done==(6'b1<<client)&&error==(6'b1<<client),"invalid request completed to wrong owner");
                require(w_ready==0&&!dut.ram_we,"invalid write consumed payload");
                tick();require(done==0&&error==0,"invalid completion repeated");no_write();
            end else begin
                require(dut.state==10&&dut.txn&&dut.request_write==writing,"valid request chose live rather than captured opcode");
                require(done==0&&error==0,"valid command was rejected");
            end
            cases=cases+1;
        end
    endtask
    task refresh_check(input integer client,input bit writing,input [22:0]address,input [4:0]words,input bit invalid);
        integer guard;
        begin
            fresh();accept(client,writing,address,words);
            dut.refresh_age=1575-dut.REFRESH_GUARD;#1;require(dut.refresh_due,"refresh forcing failed");
            tick();require(dut.state==18&&dut.refresh_resume==22,"CHECK refresh resume was not retained");
            tuple(client,writing,address,words);guard=0;
            while(dut.state!=22)begin
                no_write();require(done==0&&error==0,"refresh lost or prematurely completed staged command");
                require(w_ready==0&&!dut.ram_we,"refresh consumed staged write payload");
                tuple(client,writing,address,words);tick();guard=guard+1;require(guard<40,"refresh did not return to CHECK");
            end
            check_quiet();require(!dut.refresh_due,"refresh did not reset its age");
            validate(client,writing,address,words,invalid);
        end
    endtask
    integer client, mode, kind, guard;
    reg [22:0] a;
    reg [4:0] n;
    initial begin
        for(client=0;client<6;client=client+1)for(mode=0;mode<2;mode=mode+1)begin
            fresh();a=23'h123400+client*32;n=(client*3)%16+1;
            accept(client,mode,a,n);validate(client,mode,a,n,0);
        end
        // Every owner, both captured opcodes, and all malformed field classes.
        for(client=0;client<6;client=client+1)for(kind=0;kind<4;kind=kind+1)begin
            case(kind)
                0:begin a=23'h101;n=1;end
                1:begin a=23'h100;n=0;end
                2:begin a=23'h100;n=17;end
                3:begin a=23'h7ffffc;n=2;end
            endcase
            fresh();accept(client,kind[0],a,n);validate(client,kind[0],a,n,1);
        end
        refresh_check(2,1,23'h7fffc0,16,0);
        refresh_check(1,0,23'h7ffffc,1,0);
        refresh_check(4,1,23'h7ffffc,2,1);
        refresh_check(5,0,23'h101,1,1);
        // Reset cancels either a valid or malformed staged write before it touches RAM/pins.
        for(kind=0;kind<2;kind=kind+1)begin
            fresh();accept(3,1,23'h234500,kind?17:16);
            reset=1;tick();require(dut.state==0&&!initialized,"reset failed to cancel CHECK");
            require(done==0&&error==0&&dut.write_tail==0,"reset leaked completion or physical write tail");
            require(dut.owner==0&&dut.request_address==0&&dut.request_words==0&&!dut.request_write,"reset retained a staged command");
            no_write();reset=0;guard=0;
            while(!initialized||dut.state!=9)begin tick();no_write();require(done==0&&error==0&&w_ready==0,"reset request reappeared");guard=guard+1;require(guard<80,"reset reinitialization timeout");end
            cases=cases+1;
        end
        // A stalled old read response owns its metadata until the final handshake retires it.
        fresh();dut.state=10;dut.txn=1;dut.issued=1;dut.fill_index=1;dut.owner=3;
        dut.request_address=23'h30000;dut.request_words=1;
        dut.request_write=0;dut.read_stream_active=1;dut.read_output_valid=1;dut.read_index=0;
        cmd_valid=4;cmd_write=4;cmd_addr=0;cmd_words=0;cmd_addr[2*23+:23]=23'h456700;cmd_words[2*5+:5]=4;
        r_ready=0;#1;
        repeat(4)begin require(cmd_ready==0&&dut.owner==3,"new request overwrote stalled old read");tick();end
        r_ready=8;tick();require(done==0&&dut.owner==3&&dut.state==10&&!dut.read_stream_active,"old read did not finish before handoff");
        r_ready=0;tick();require(dut.state==9&&done==8&&dut.owner==3,"read did not retain its owner through completion");
        tick();require(dut.state==22&&dut.owner==2,"next request did not enter CHECK after old read drained");
        tuple(2,1,23'h456700,4);cases=cases+1;
        $display("PASS %0d targeted CHECK request cases, %0d assertions, %0d edges",cases,checks,edges);
        $finish;
    end
endmodule
