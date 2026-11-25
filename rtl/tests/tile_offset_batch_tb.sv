`timescale 1ns/1ps

// Independent signed80 scalar offsets and 1x1 tile windows. The sole sample
// isolates setup arithmetic from raster stepping and makes writes observable.
module nano_tile_offset_batch_tb;
    localparam integer TILE_X=608, TILE_Y=448;
    localparam integer OFFSET_Y=6, OFFSET_X=7, RASTER=8;
    localparam [63:0] MAX64=64'h7fffffffffffffff, MIN64=64'h8000000000000000;
    localparam signed [63:0] BATCH_MAX=64'sd4294967295, BATCH_MIN=-64'sd4294967296;
    localparam signed [79:0] WIDE_MAX=80'sh00007fffffffffffffff;
    localparam signed [79:0] WIDE_MIN=80'shffff8000000000000000;
    localparam [15:0] OLD_COLOR=16'hbeef, TEXTURE_COLOR=16'h29a7;
    localparam [23:0] OLD_DEPTH=24'h654321, FLAT_RGB=24'h552b1f;

    reg clk=0, reset=0, cmd_word_we=0, cmd_valid=0, pix_re=0;
    reg [1:0] pix_we=0;
    reg [6:0] cmd_word_index=0;
    reg [31:0] cmd_word_data=0;
    reg [9:0] pix_addr=0, pix_raddr=0;
    reg [31:0] pix_color=0;
    reg [23:0] pix_depth=0;
    wire pix_write_pair=0;
    wire [9:0] cmd_tile_x=TILE_X;
    wire [8:0] cmd_tile_y=TILE_Y;
    wire cmd_ready,busy,done,fault,pix_rvalid;
    wire [31:0] pix_rcolor;
    wire [23:0] pix_rdepth;

    wire tex_flush=0, tex_cfg_ready=1, tex_fault=0;
    wire tex_cfg_valid, tex_cfg_repeat_u, tex_cfg_repeat_v, tex_req_valid;
    wire tex_rsp_ready;
    reg tex_rsp_valid=0;
    wire [22:0] tex_cfg_base, tex_cfg_pitch;
    wire [10:0] tex_cfg_width, tex_cfg_height;
    wire signed [31:0] tex_req_x,tex_req_y;
    wire [5:0] tex_req_tag;
    reg [5:0] tex_rsp_tag=0;
    wire [15:0] tex_rsp_texel=TEXTURE_COLOR;
    wire tex_req_ready=!tex_rsp_valid||tex_rsp_ready;
    nano_tile_renderer dut(.*);

    reg [31:0] record [0:79];
    reg signed [63:0] seeds [0:6], dx [0:6], dy [0:6];
    reg signed [79:0] expected [0:6];
    reg [6:0] enabled,fault_plane;
    integer x_offset,y_offset,start_x,start_y,sample_address,shade,flags;
    integer fault_axis,fault_step,cases=0,y_faults=0,x_faults=0,write_checks=0;
    integer offset_edges=0,expected_scalar_edges=0,textured_cases=0;
    integer narrow_boundary_cases=0,wide_boundary_cases=0;
    integer configuration_count=0,texture_count=0,color_writes=0,depth_writes=0;
    reg watching=0;

    task automatic tick;
        begin
            #5; clk=1;
            #5; clk=0;
        end
    endtask

    always @(posedge clk) begin
        if(reset)tex_rsp_valid<=0;
        else if(tex_req_ready)begin
            tex_rsp_valid<=tex_req_valid;
            if(tex_req_valid)begin
                if(!watching||shade!=2||tex_req_x!==32'sd6||tex_req_y!==32'sd9)
                    $fatal(1,"case %0d wrong texture coordinate %0d,%0d",cases,tex_req_x,tex_req_y);
                tex_rsp_tag<=tex_req_tag;
                texture_count=texture_count+1;
            end
        end
        if(watching&&!reset)begin
            if(tex_cfg_valid)configuration_count=configuration_count+1;
            if(dut.color_we)color_writes=color_writes+1;
            if(dut.depth_we)depth_writes=depth_writes+1;
            if((dut.color_we||dut.depth_we)&&dut.state!=RASTER&&dut.state!=17)
                $fatal(1,"triangle offset setup wrote an attachment");
        end
    end

    function automatic [23:0] quantized(
        input signed [79:0] value,
        input integer maximum);
        reg signed [79:0] integer_part, fraction;
        begin
            if(value<0)quantized=0;
            else begin
                integer_part=value/65536;
                fraction=value%65536;
                if(fraction>32768||(fraction==32768&&integer_part[0]))
                    integer_part=integer_part+1;
                quantized=integer_part>maximum?maximum:integer_part;
            end
        end
    endfunction

    task automatic setup_vector(
        input integer x_distance,y_distance,ordinal,shading_mode,write_flags);
        integer plane;
        begin
            x_offset=x_distance;y_offset=y_distance;
            shade=shading_mode;flags=write_flags;
            start_x=TILE_X+ordinal%32;
            start_y=TILE_Y+(ordinal*13)%32;
            if(start_x<x_offset)start_x=x_offset;
            if(start_y<y_offset)start_y=y_offset;
            sample_address=(start_y-TILE_Y)*32+start_x-TILE_X;
            case(shade)
                0: enabled=flags[2]?7'b0000001:0;
                1: enabled=7'b0001110|{6'd0,flags[2]};
                2: enabled=7'b1110000|{6'd0,flags[2]};
                default: $fatal(1,"test fixture used unsupported shade");
            endcase
            seeds[0]=(24'sh124562+ordinal)*64'sd65536+32768;
            dx[0]=64'sd65536+8192;dy[0]=-64'sd65536+4096;
            seeds[1]=64'sd95*65536+32768;
            dx[1]=64'sd65536;dy[1]=64'sd32768;
            seeds[2]=64'sd154*65536+32768;
            dx[2]=-(64'sd65536+16384);dy[2]=64'sd65536;
            seeds[3]=64'sd210*65536-32768;
            dx[3]=-(64'sd65536-32768);dy[3]=-(64'sd65536+8192);
            // Poison every inactive plane with a real out-of-range scalar
            // step if it is ever incorrectly included.
            for(plane=0;plane<7;plane=plane+1)if(!enabled[plane])begin
                seeds[plane]=MAX64;
                dx[plane]=1;dy[plane]=1;
            end
            if(shade==2)begin
                seeds[4]=64'sd1<<36;dx[4]=0;dy[4]=0;
                seeds[5]=64'sd1<<26;dx[5]=64'sd1<<26;dy[5]=0;
                seeds[6]=64'sd2<<26;dx[6]=0;dy[6]=64'sd1<<26;
            end
        end
    endtask

    task automatic scalar_reference;
        integer plane,axis,step,steps_on_axis;
        reg signed [79:0] slope;
        begin
            fault_axis=0;fault_step=0;fault_plane=0;
            for(plane=0;plane<7;plane=plane+1)expected[plane]=seeds[plane];
            for(axis=OFFSET_Y;axis<=OFFSET_X;axis=axis+1)begin
                steps_on_axis=axis==OFFSET_Y?y_offset:x_offset;
                for(step=1;step<=steps_on_axis&&fault_axis==0;step=step+1)begin
                    for(plane=0;plane<7;plane=plane+1)if(enabled[plane])begin
                        slope=axis==OFFSET_Y?dy[plane]:dx[plane];
                        expected[plane]=expected[plane]+slope;
                        if(expected[plane]<WIDE_MIN||expected[plane]>WIDE_MAX)begin
                            fault_axis=axis;fault_step=step;fault_plane[plane]=1;
                        end
                    end
                end
            end
        end
    endtask

    // Mathematical signed range check, independent of the coefficient
    // word flags captured by the implementation.
    function automatic axis_can_batch(input integer axis);
        integer plane;
        reg signed [63:0] slope;
        begin
            axis_can_batch=1;
            for(plane=0;plane<7;plane=plane+1)if(enabled[plane])begin
                slope=axis==OFFSET_Y?dy[plane]:dx[plane];
                if(slope<BATCH_MIN||slope>BATCH_MAX)axis_can_batch=0;
            end
        end
    endfunction

    task automatic set_slope(input integer axis,plane,input signed [63:0] slope);
        begin
            if(axis==OFFSET_Y)dy[plane]=slope;
            else dx[plane]=slope;
        end
    endtask

    task automatic load_vector;
        integer index,plane,word_base;
        begin
            for(index=0;index<80;index=index+1)record[index]=0;
            record[0]=32'h00000200|(flags<<16);
            record[1]=shade;
            record[2]=FLAT_RGB;
            record[4]=start_x;record[5]=start_y;
            record[6]=640;record[7]=480;
            record[8]=start_x-x_offset;record[9]=start_y-y_offset;
            record[10]=start_x+1;record[11]=start_y+1;
            if(shade==2)begin
                record[1]=record[1]|32'h00040000;
                record[12]=32'h258000;record[13]=16;record[14]=16;record[15]=32;
            end
            // Three positive edge constants cover the one-pixel window.
            record[28]=1;record[32]=1;record[36]=1;
            for(plane=0;plane<7;plane=plane+1)begin
                word_base=38+6*plane;
                {record[word_base+1],record[word_base]}=seeds[plane];
                {record[word_base+3],record[word_base+2]}=dx[plane];
                {record[word_base+5],record[word_base+4]}=dy[plane];
            end
            for(index=79;index>=0;index=index-1)begin
                cmd_word_we=1;cmd_word_index=index;cmd_word_data=record[index];tick();
            end
            cmd_word_we=0;
        end
    endtask

    task automatic run_vector;
        integer remaining_y,remaining_x,step,phase,remaining,available;
        integer cycles,batch_edges,plane;
        reg next_batch_fault;
        reg [7:0] r,g,b;
        reg [15:0] wanted_color,read_color;
        reg [23:0] wanted_depth;
        begin
            scalar_reference();
            pix_addr=sample_address;pix_we=3;pix_color=OLD_COLOR;pix_depth=OLD_DEPTH;
            tick();pix_we=0;
            load_vector();
            color_writes=0;depth_writes=0;configuration_count=0;texture_count=0;
            watching=1;cmd_valid=1;tick();cmd_valid=0;
            remaining_y=y_offset;remaining_x=x_offset;
            cycles=0;batch_edges=0;
            while(dut.state!=RASTER&&!fault&&!done&&cycles<400)begin
                phase=dut.state;
                next_batch_fault=0;
                if(phase==OFFSET_Y||phase==OFFSET_X)begin
                    remaining=phase==OFFSET_Y?remaining_y:remaining_x;
                    if(enabled==0||remaining<=0||(phase==OFFSET_X&&remaining_y!=0)||
                       (phase==OFFSET_Y&&dut.offset_y!==remaining[9:0])||
                       (phase==OFFSET_X&&dut.offset_x!==remaining[10:0]))
                        $fatal(1,"case %0d invalid offset count or axis order",cases);
                    available=phase==OFFSET_Y?y_offset:x_offset;
                    step=remaining>=4&&axis_can_batch(phase)?4:1;
                    next_batch_fault=phase==fault_axis&&available-remaining+step>=fault_step;
                    if(phase==OFFSET_Y)remaining_y=remaining_y-step;
                    else remaining_x=remaining_x-step;
                    batch_edges=batch_edges+1;
                end
                tick();cycles=cycles+1;
                if(fault!==next_batch_fault)
                    $fatal(1,"case %0d fault edge mismatch phase%0d fault%0d expected%0d",
                        cases,phase,fault,next_batch_fault);
            end
            if(fault_axis!=0)begin
                if(!fault||busy||done||cmd_ready||dut.plane_fault!==fault_plane||
                    color_writes!=0||depth_writes!=0||texture_count!=0)
                    $fatal(1,"case %0d setup overflow escaped or had side effects",cases);
                if(fault_axis==OFFSET_Y)y_faults=y_faults+1;
                else x_faults=x_faults+1;
                repeat(2)tick();
                if(!fault||color_writes!=0||depth_writes!=0)
                    $fatal(1,"case %0d setup fault was not sticky",cases);
                watching=0;reset=1;tick();reset=0;tick();
                wanted_color=OLD_COLOR;wanted_depth=OLD_DEPTH;
            end else begin
                if(fault||done||dut.state!=RASTER||color_writes!=0||depth_writes!=0||
                   (enabled!=0&&(remaining_x!=0||remaining_y!=0))||dut.plane_enable!==enabled)
                    $fatal(1,"case %0d failed to prepare one-sample triangle",cases);
                for(plane=0;plane<7;plane=plane+1)if(enabled[plane]&&
                    (dut.plane_row[plane]!==expected[plane][63:0]||
                     dut.plane_current[plane]!==expected[plane][63:0]))
                    $fatal(1,"case %0d plane %0d exact signed value mismatch, got %h expected %h",
                        cases,plane,dut.plane_current[plane],expected[plane][63:0]);
                offset_edges=offset_edges+batch_edges;
                if(enabled!=0)expected_scalar_edges=expected_scalar_edges+x_offset+y_offset;
                r=shade==1?quantized(expected[1],255):FLAT_RGB[7:0];
                g=shade==1?quantized(expected[2],255):FLAT_RGB[15:8];
                b=shade==1?quantized(expected[3],255):FLAT_RGB[23:16];
                wanted_color=shade==2?TEXTURE_COLOR:{r[7:3],g[7:2],b[7:3]};
                wanted_depth=flags[2]?quantized(expected[0],16777215):OLD_DEPTH;
                cycles=0;
                while(!done&&!fault&&cycles<100)begin tick();cycles=cycles+1;end
                if(fault||!done||busy||color_writes!=1||depth_writes!=flags[2]||
                   configuration_count!=(shade==2)||texture_count!=(shade==2))
                    $fatal(1,"case %0d wrong raster retirement or attachment enables",cases);
                if(shade==2)textured_cases=textured_cases+1;
                watching=0;
            end
            pix_re=1;pix_raddr=sample_address;tick();pix_re=0;
            read_color=sample_address[0]?pix_rcolor[31:16]:pix_rcolor[15:0];
            if(!pix_rvalid||read_color!==wanted_color||pix_rdepth!==wanted_depth)
                $fatal(1,"case %0d actual RGB565/D24 %h/%h expected %h/%h",
                    cases,read_color,pix_rdepth,wanted_color,wanted_depth);
            write_checks=write_checks+1;
            cases=cases+1;
        end
    endtask

    integer n,axis,sign_case,tail,target_plane;
    initial begin
        reset=1;tick();reset=0;tick();
        // Multiplication by 19 permutes the 32 low-bit offsets.
        for(n=0;n<32;n=n+1)begin
            setup_vector(n,(19*n)%32,n,1,5);run_vector();
        end
        for(n=0;n<3;n=n+1)begin
            setup_vector(637+n,477+n,n,1,5);run_vector();
        end
        setup_vector(7,0,5,1,5);run_vector();
        setup_vector(0,11,6,1,5);run_vector();
        for(axis=OFFSET_Y;axis<=OFFSET_X;axis=axis+1)
            for(sign_case=0;sign_case<2;sign_case=sign_case+1)begin
                setup_vector(axis==OFFSET_X?4:0,axis==OFFSET_Y?4:0,sign_case,1,5);
                seeds[0]=sign_case?MAX64:MIN64;
                if(axis==OFFSET_Y)dy[0]=sign_case?-64'sh3fffffffffffffff:64'sh3fffffffffffffff;
                else dx[0]=sign_case?-64'sh3fffffffffffffff:64'sh3fffffffffffffff;
                run_vector();
            end
        // In Y overflow cases the later opposite X slope would make the
        // cumulative result fit. A Y fault must still occur before any X.
        for(axis=OFFSET_Y;axis<=OFFSET_X;axis=axis+1)
            for(sign_case=0;sign_case<2;sign_case=sign_case+1)
                for(tail=0;tail<2;tail=tail+1)begin
                    setup_vector(7,7,sign_case+tail,1,5);
                    target_plane=2*sign_case+tail;
                    seeds[target_plane]=sign_case?MIN64+(tail?5:1):MAX64-(tail?5:1);
                    dy[target_plane]=axis==OFFSET_Y?(sign_case?-1:1):0;
                    dx[target_plane]=axis==OFFSET_Y?(sign_case?1:-1):(sign_case?-1:1);
                    run_vector();
                end
        setup_vector(11,13,2,0,1);run_vector();
        setup_vector(5,9,3,1,1);run_vector();
        setup_vector(5,7,4,2,5);run_vector();
        // Exercise both inclusive signed33 endpoints and their nearest
        // excluded values, in every active D24/RGB plane independently.
        // Also put the wide slopes into inactive planes before a depth-only
        // batch so they cannot silently reduce issue width.
        for(axis=OFFSET_Y;axis<=OFFSET_X;axis=axis+1)
            for(target_plane=0;target_plane<4;target_plane=target_plane+1)
                for(sign_case=0;sign_case<2;sign_case=sign_case+1)begin
                    setup_vector(5,7,sign_case,1,5);
                    set_slope(axis,target_plane,sign_case?BATCH_MIN:BATCH_MAX);
                    run_vector();narrow_boundary_cases=narrow_boundary_cases+1;
                    setup_vector(5,7,sign_case,1,5);
                    set_slope(axis,target_plane,sign_case?BATCH_MIN-1:BATCH_MAX+1);
                    run_vector();wide_boundary_cases=wide_boundary_cases+1;
                    setup_vector(5,7,sign_case,0,5);
                    set_slope(axis,2,sign_case?BATCH_MIN-1:BATCH_MAX+1);
                    run_vector();
                end
        $display("PASS batched offsets: cases%0d Yfaults%0d Xfaults%0d pixels%0d textured%0d offset_edges%0d serial_edges%0d narrow%0d wide%0d",
            cases,y_faults,x_faults,write_checks,textured_cases,offset_edges,expected_scalar_edges,
            narrow_boundary_cases,wide_boundary_cases);
        $finish;
    end
endmodule
