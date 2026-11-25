`timescale 1ns/1ps

// A scalar signed-wide edge/plane walk supplies the observable oracle. The
// stage trace independently checks that every skipped sample was uncovered.
module nano_tile_uncovered_batch_tb;
    localparam integer RASTER=8, RASTER_SKIP4=14;
    localparam signed [79:0] MIN_PLANE=-(80'sd1<<63), MAX_PLANE=(80'sd1<<63)-1;
    localparam signed [63:0] MIN_EDGE=-(64'sd1<<31), MAX_EDGE=(64'sd1<<31)-1;
    localparam [15:0] OLD_COLOR=16'hba99, TEXEL=16'h29a7;
    localparam [23:0] OLD_DEPTH=24'h876543;

    reg clk=0,reset=0,cmd_word_we=0,cmd_valid=0,pix_re=0;
    reg [1:0] pix_we=0;
    reg [6:0] cmd_word_index=0;
    reg [31:0] cmd_word_data=0;
    reg [9:0] pix_addr=0,pix_raddr=0;
    reg [31:0] pix_color=0;
    reg [23:0] pix_depth=0;
    wire pix_write_pair=0;
    wire [9:0] cmd_tile_x=0;
    wire [8:0] cmd_tile_y=0;
    wire cmd_ready,busy,done,fault,pix_rvalid;
    wire [31:0] pix_rcolor;
    wire [23:0] pix_rdepth;

    wire tex_flush=0,tex_cfg_ready=1,tex_fault=0,tex_rsp_ready;
    wire tex_cfg_valid,tex_cfg_repeat_u,tex_cfg_repeat_v,tex_req_valid;
    wire [22:0] tex_cfg_base,tex_cfg_pitch;
    wire [10:0] tex_cfg_width,tex_cfg_height;
    wire signed [31:0] tex_req_x,tex_req_y;
    wire [5:0] tex_req_tag;
    reg tex_rsp_valid=0;
    reg [5:0] tex_rsp_tag=0;
    wire [15:0] tex_rsp_texel=TEXEL;
    integer clock_number=0;
    wire tex_req_ready=(!tex_rsp_valid||tex_rsp_ready)&&(clock_number%9>=3);
    nano_tile_renderer dut(.*);

    reg [31:0] record [0:79];
    integer ax [0:2],by [0:2],inclusive [0:2];
    reg signed [63:0] edge_seed [0:2];
    reg signed [63:0] seed [0:6],dx [0:6],dy [0:6];
    reg [15:0] wanted_color [0:1023];
    reg [23:0] wanted_depth [0:1023];
    reg [6:0] enabled,wanted_plane_fault;
    reg [2:0] wanted_edge_fault;
    integer left,top,right,bottom,shade,flags;
    integer fault_x,fault_y,drawn;
    integer cases=0,pixel_checks=0,staged_steps=0,faults=0,row_exits=0,texture_requests=0;
    integer blocked_pending=0,blocked_fragment=0;
    reg watch=0;

    task automatic tick;
        begin
            #5;clk=1;
            #5;clk=0;
        end
    endtask

    always @(posedge clk) begin
        clock_number<=clock_number+1;
        if(reset)tex_rsp_valid<=0;
        else begin
            if(tex_rsp_valid&&tex_rsp_ready)tex_rsp_valid<=0;
            if(tex_req_valid&&tex_req_ready)begin
                if(!watch||shade!=2||tex_req_x!==32'sd3||tex_req_y!==32'sd5)
                    $fatal(1,"case %0d wrong texture request %0d/%0d",cases,tex_req_x,tex_req_y);
                tex_rsp_valid<=1;tex_rsp_tag<=tex_req_tag;
                texture_requests=texture_requests+1;
            end
        end
    end

    function automatic signed [63:0] edge_at(input integer e,x,y);
        begin edge_at=edge_seed[e]+64'sd16*ax[e]*(x-left)+64'sd16*by[e]*(y-top);end
    endfunction

    function automatic signed [79:0] plane_at(input integer p,x,y);
        reg signed [79:0] start,step_x,step_y;
        begin
            start=seed[p];step_x=dx[p];step_y=dy[p];
            plane_at=start+step_x*(x-left)+step_y*(y-top);
        end
    endfunction

    function automatic edge_passes(input integer e,x,y);
        reg signed [63:0] value;
        begin value=edge_at(e,x,y);edge_passes=value>0||(value==0&&inclusive[e]);end
    endfunction

    function automatic covered(input integer x,y);
        begin covered=edge_passes(0,x,y)&&edge_passes(1,x,y)&&edge_passes(2,x,y);end
    endfunction

    function automatic [23:0] quantize(input signed [79:0] q,input integer limit);
        reg signed [79:0] whole,fraction;
        begin
            if(q<0)quantize=0;
            else begin
                whole=q/65536;fraction=q%65536;
                if(fraction>32768||(fraction==32768&&whole[0]))whole=whole+1;
                quantize=whole>limit?limit:whole;
            end
        end
    endfunction

    // Update only values that scalar execution actually consumes. A failing
    // nonincreasing edge abandons the rest of that row; the next row uses the
    // saved first-sample value, independent of the abandoned X steps.
    task automatic scalar_oracle;
        integer x,y,p,e,address,next_x,next_y;
        reg abandon;
        reg [7:0] r,g,b;
        reg signed [79:0] plane_value;
        reg signed [63:0] edge_value;
        begin
            fault_x=-1;fault_y=-1;wanted_plane_fault=0;wanted_edge_fault=0;drawn=0;
            for(address=0;address<1024;address=address+1)begin
                wanted_color[address]=OLD_COLOR;wanted_depth[address]=OLD_DEPTH;
            end
            for(y=top;y<bottom&&fault_x<0;y=y+1)begin
                abandon=0;
                for(x=left;x<right&&!abandon&&fault_x<0;x=x+1)begin
                    if(covered(x,y))begin
                        address=32*y+x;drawn=drawn+1;
                        r=quantize(plane_at(1,x,y),255);
                        g=quantize(plane_at(2,x,y),255);
                        b=quantize(plane_at(3,x,y),255);
                        wanted_color[address]=shade==2?TEXEL:{r[7:3],g[7:2],b[7:3]};
                        if(flags[2])wanted_depth[address]=quantize(plane_at(0,x,y),16777215);
                    end
                    for(e=0;e<3;e=e+1)
                        if(ax[e]<=0&&!edge_passes(e,x,y))abandon=1;
                    if(x+1==right||abandon)begin next_x=left;next_y=y+1;end
                    else begin next_x=x+1;next_y=y;end
                    if(next_y<bottom)begin
                        for(e=0;e<3;e=e+1)begin
                            edge_value=edge_at(e,next_x,next_y);
                            wanted_edge_fault[e]=edge_value<MIN_EDGE||edge_value>MAX_EDGE;
                        end
                        for(p=0;p<7;p=p+1)begin
                            plane_value=plane_at(p,next_x,next_y);
                            wanted_plane_fault[p]=enabled[p]&&(plane_value<MIN_PLANE||plane_value>MAX_PLANE);
                        end
                        if((|wanted_plane_fault)||(|wanted_edge_fault))begin
                            fault_x=next_x;fault_y=next_y;
                        end
                    end
                end
            end
        end
    endtask

    task automatic fixture(input integer x,y,w,h);
        integer e;
        begin
            left=x;top=y;right=x+w;bottom=y+h;shade=1;flags=5;
            for(e=0;e<3;e=e+1)begin
                ax[e]=0;by[e]=0;inclusive[e]=1;edge_seed[e]=32;
            end
            ax[0]=1;edge_seed[0]=-112;
            seed[0]=64'sd9345*65536+32768;dx[0]=64'sd23*65536;dy[0]=64'sd29*65536;
            seed[1]=64'sd62*65536+32768;dx[1]=64'sd3*65536;dy[1]=64'sd5*65536;
            seed[2]=64'sd198*65536;dx[2]=-64'sd2*65536;dy[2]=-64'sd10*65536;
            seed[3]=64'sd91*65536;dx[3]=64'sd4*65536;dy[3]=64'sd13*65536;
            seed[4]=64'sd1<<36;dx[4]=0;dy[4]=0;
            seed[5]=64'sd3<<26;dx[5]=0;dy[5]=0;
            seed[6]=64'sd5<<26;dx[6]=0;dy[6]=0;
        end
    endtask

    task automatic upload_record;
        integer word_index,p,e;
        reg signed [63:0] c;
        begin
            for(word_index=0;word_index<80;word_index=word_index+1)record[word_index]=0;
            record[0]=32'h00000200|(flags<<16);
            record[1]=shade;
            record[4]=left;record[5]=top;record[6]=right;record[7]=bottom;
            record[8]=left;record[9]=top;record[10]=32;record[11]=32;
            if(shade==2)begin
                record[1]=record[1]|32'h40000;
                record[12]=32'h258000;record[13]=8;record[14]=8;record[15]=16;
            end
            for(e=0;e<3;e=e+1)begin
                c=edge_seed[e]-64'sd1*(16*left+8)*ax[e]-64'sd1*(16*top+8)*by[e];
                if(c<MIN_EDGE||c>MAX_EDGE)$fatal(1,"fixture edge constant out of range");
                record[26+4*e]=ax[e];record[27+4*e]=by[e];
                record[28+4*e]=c;record[29+4*e]=inclusive[e];
            end
            for(p=0;p<7;p=p+1)begin
                word_index=38+6*p;
                {record[word_index+1],record[word_index]}=seed[p];
                {record[word_index+3],record[word_index+2]}=dx[p];
                {record[word_index+5],record[word_index+4]}=dy[p];
            end
            for(word_index=0;word_index<80;word_index=word_index+1)begin
                cmd_word_we=1;cmd_word_index=word_index;cmd_word_data=record[word_index];tick();
            end
            cmd_word_we=0;
        end
    endtask

    // Evaluate all intervening sample positions separately, including ties,
    // rather than reimplementing the hardware's rising-witness bound.
    function automatic clean_interval(input integer x,y);
        integer i,e;
        begin
            clean_interval=x+5<right;
            for(i=0;i<5;i=i+1)begin
                if(covered(x+i,y))clean_interval=0;
                for(e=0;e<3;e=e+1)
                    if(ax[e]<=0&&!edge_passes(e,x+i,y))clean_interval=0;
            end
        end
    endfunction

    task automatic run_fixture(input integer min_stages,max_stages,min_row_exits);
        integer address,e,p,prev_x,prev_y,phase,cycles,stages,exits,old_textures;
        reg interval_was_clear,pending_was_busy,fragment_was_busy;
        reg [15:0] read_color;
        reg signed [63:0] expected_edge;
        reg signed [79:0] expected_plane;
        begin
            enabled=(shade==1?7'b0001110:7'b1110000)|{6'd0,flags[2]};
            scalar_oracle();
            pix_we=3;pix_color={16'd0,OLD_COLOR};pix_depth=OLD_DEPTH;
            for(address=0;address<1024;address=address+1)begin pix_addr=address;tick();end
            pix_we=0;
            upload_record();
            watch=1;old_textures=texture_requests;cmd_valid=1;tick();cmd_valid=0;
            cycles=0;stages=0;exits=0;
            while(!fault&&!done&&cycles<700)begin
                prev_x=dut.x;prev_y=dut.y;phase=dut.state;
                interval_was_clear=clean_interval(prev_x,prev_y);
                pending_was_busy=dut.pending;fragment_was_busy=dut.fragment_busy;
                if(phase==RASTER)begin
                    if(prev_x<left||prev_x>=right||prev_y<top||prev_y>=bottom||
                       dut.inside_sample!==covered(prev_x,prev_y))
                        $fatal(1,"case %0d invalid scalar coverage or coordinates %0d,%0d",cases,prev_x,prev_y);
                    for(e=0;e<3;e=e+1)begin
                        expected_edge=edge_at(e,prev_x,prev_y);
                        if(dut.edge_current[e]!==expected_edge[31:0])
                            $fatal(1,"case %0d edge %0d disagrees with scalar evaluation",cases,e);
                    end
                    for(p=0;p<7;p=p+1)if(enabled[p])begin
                        expected_plane=plane_at(p,prev_x,prev_y);
                        if(dut.plane_current[p]!==expected_plane[63:0])
                            $fatal(1,"case %0d plane %0d disagrees with scalar evaluation",cases,p);
                    end
                    if(dut.row_exit)exits=exits+1;
                    if(interval_was_clear&&pending_was_busy)blocked_pending=blocked_pending+1;
                    if(interval_was_clear&&fragment_was_busy)blocked_fragment=blocked_fragment+1;
                end
                if(phase==RASTER_SKIP4&&(dut.color_we||dut.depth_we||dut.pending||dut.fragment_busy||tex_req_valid))
                    $fatal(1,"case %0d staged skip did not own a quiescent pipeline",cases);
                tick();cycles=cycles+1;
                if(phase==RASTER&&dut.state==RASTER_SKIP4&&!fault)begin
                    if(!interval_was_clear||pending_was_busy||fragment_was_busy||
                       dut.x!=prev_x+1||dut.y!=prev_y)
                        $fatal(1,"case %0d skipped a covered, terminal or occupied sample",cases);
                    stages=stages+1;
                end
                if(phase==RASTER_SKIP4&&!fault&&
                   (dut.state!=RASTER||dut.x!=prev_x+4||dut.y!=prev_y))
                    $fatal(1,"case %0d staged four-step resume differed from scalar coordinates",cases);
                if(fault&&
                   (fault_x<0||fault_y!=prev_y||fault_x<=prev_x||
                    fault_x>prev_x+(phase==RASTER_SKIP4?4:1)||
                    dut.plane_fault!==wanted_plane_fault||dut.edge_fault!==wanted_edge_fault))
                    $fatal(1,"case %0d overflow disagrees with consumed scalar updates",cases);
            end
            if((fault_x>=0)!==fault||(!fault&&!done)||busy||stages<min_stages||stages>max_stages||
               exits<min_row_exits||texture_requests-old_textures!=(shade==2?drawn:0))
                $fatal(1,"case %0d completion: fault%0d done%0d stages%0d exits%0d texture%0d wanted%0d",
                    cases,fault,done,stages,exits,texture_requests-old_textures,shade==2?drawn:0);
            staged_steps=staged_steps+stages;row_exits=row_exits+exits;watch=0;
            if(fault)begin
                faults=faults+1;repeat(2)tick();
                if(!fault||cmd_ready)$fatal(1,"case %0d numeric fault was not sticky",cases);
                reset=1;tick();reset=0;tick();
            end
            pix_re=1;
            for(address=0;address<1024;address=address+1)begin
                pix_raddr=address;tick();
                read_color=address[0]?pix_rcolor[31:16]:pix_rcolor[15:0];
                if(!pix_rvalid||read_color!==wanted_color[address]||pix_rdepth!==wanted_depth[address])
                    $fatal(1,"case %0d pixel %0d RGB565/D24 %h/%h wanted %h/%h",
                        cases,address,read_color,pix_rdepth,wanted_color[address],wanted_depth[address]);
                pixel_checks=pixel_checks+1;
            end
            pix_re=0;tick();cases=cases+1;
        end
    endtask

    integer parity;
    initial begin
        reset=1;tick();reset=0;tick();
        for(parity=0;parity<2;parity=parity+1)begin
            // Exclusive rising tie at x+4 skips, inclusive tie draws.
            fixture(parity,2,6,3);edge_seed[0]=-64;inclusive[0]=0;run_fixture(1,1,0);
            fixture(parity,2,6,3);edge_seed[0]=-64;run_fixture(0,0,0);
            // The final sample is a valid batch destination. A row with
            // only five samples must stay scalar even if all are uncovered.
            fixture(parity,2,6,3);edge_seed[0]=-80;run_fixture(1,1,0);
            fixture(parity,2,5,3);edge_seed[0]=-80;run_fixture(0,0,0);
        end
        fixture(0,0,23,1);edge_seed[0]=-256;run_fixture(3,3,0);
        // Nonincreasing exclusive tie makes x+4 the row exit. A hypothetical
        // arithmetic overflow at x+5 is outside scalar execution.
        fixture(0,0,10,2);edge_seed[0]=-128;
        ax[1]=-1;edge_seed[1]=64;inclusive[1]=0;
        seed[0]=MAX_PLANE-4;dx[0]=1;dy[0]=0;run_fixture(0,0,2);
        // Inclusive tie keeps x+4 in the scalar traversal, so x+5 must
        // receive its exact next values before the ordinary row exit.
        fixture(0,0,10,2);edge_seed[0]=-128;
        ax[1]=-1;edge_seed[1]=64;
        dx[0]=5;dy[0]=7;run_fixture(2,2,2);

        fixture(0,0,12,2);edge_seed[0]=0;by[0]=-7;run_fixture(1,1,0);
        fixture(0,0,12,2);edge_seed[0]=0;by[0]=-7;shade=2;run_fixture(0,0,0);

        // Eligibility sees only enabled X slopes. Poison unused depth, then
        // exercise the same wide step with depth enabled and scalar progress.
        fixture(0,0,10,1);flags=1;seed[0]=MAX_PLANE;dx[0]=80'sd1<<40;run_fixture(1,1,0);
        fixture(0,0,10,1);dx[0]=80'sd1<<40;run_fixture(0,0,0);

        // A positive non-witness edge may overflow on either the original
        // scalar update or a later update consumed in the staged interval.
        fixture(0,0,10,1);ax[1]=1;edge_seed[1]=MAX_EDGE-8;run_fixture(0,0,0);
        fixture(0,0,10,1);ax[1]=1;edge_seed[1]=MAX_EDGE-40;run_fixture(1,1,0);
        fixture(0,0,10,1);seed[0]=MAX_PLANE-2;dx[0]=1;run_fixture(1,1,0);
        fixture(0,0,10,1);seed[2]=MIN_PLANE+2;dx[2]=-1;run_fixture(1,1,0);
        if(blocked_pending==0||blocked_fragment==0)$fatal(1,"occupied next rows were not exercised");
        $display("PASS uncovered batches: cases%0d pixels%0d stages%0d faults%0d row_exits%0d pending%0d fragments%0d textures%0d",
            cases,pixel_checks,staged_steps,faults,row_exits,blocked_pending,blocked_fragment,texture_requests);
        $finish;
    end
endmodule
