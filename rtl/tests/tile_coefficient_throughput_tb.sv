`timescale 1ns/1ps
// Prove the tile's registered coefficient lookahead, not only the downstream
// fragment block, can sustain a complete warm 32x32 tile at II1 across rows.
module nano_tile_coefficient_throughput_tb;
 reg clk=0,reset=0,cmd_word_we=0,cmd_valid=0,pix_re=0,tex_flush=0;
 reg [1:0] pix_we=0;
 reg [6:0]cmd_word_index=0;reg[31:0]cmd_word_data=0;reg[9:0]cmd_tile_x=0;reg[8:0]cmd_tile_y=0;
 wire cmd_ready,busy,done,fault;reg[9:0]pix_addr=0,pix_raddr=0;reg[15:0]pix_color=0;reg[23:0]pix_depth=24'hffffff;
 wire pix_rvalid;wire[15:0]pix_rcolor;wire[23:0]pix_rdepth;
 wire tex_cfg_valid;wire[22:0]tex_cfg_base,tex_cfg_pitch;wire[10:0]tex_cfg_width,tex_cfg_height;
 wire tex_cfg_repeat_u,tex_cfg_repeat_v;wire tex_req_valid;wire signed[31:0]tex_req_x,tex_req_y;wire[5:0]tex_req_tag;
 wire tex_cfg_ready=1,tex_fault=0;
 wire tex_req_ready,tex_rsp_valid,tex_rsp_ready;wire[15:0]tex_rsp_texel;wire[5:0]tex_rsp_tag;
 reg[6:0]rv=0;reg[5:0]rt[0:6];reg[15:0]rd[0:6];
 assign tex_rsp_valid=rv[6];assign tex_rsp_tag=rt[6];assign tex_rsp_texel=rd[6];
 assign tex_req_ready=!rv[6]||tex_rsp_ready;
 nano_tile_renderer dut(.*);
 reg[31:0]record[0:79];integer i,j,kind,steps,got=0,accept_run=0,max_accept=0,commit_run=0,max_commit=0;
 always@(posedge clk)begin
  if(reset)begin rv<=0;got=0;accept_run=0;max_accept=0;commit_run=0;max_commit=0;end
  else begin
   if(tex_req_ready)begin
    rv<={rv[5:0],tex_req_valid};
    for(j=6;j>0;j=j-1)begin rt[j]<=rt[j-1];rd[j]<=rd[j-1];end
    rt[0]<=tex_req_tag;rd[0]<=16'h4000+32*tex_req_y+tex_req_x;
   end
   if(tex_req_valid&&tex_req_ready&&
      (tex_req_x<0||tex_req_x>=32||tex_req_y<0||tex_req_y>=32))
    $fatal(1,"coefficient lookup generated invalid warm texel %0d/%0d",tex_req_x,tex_req_y);
   if(dut.state==8&&dut.inside_sample&&dut.need_texture&&dut.fragment_ready)begin
    accept_run=accept_run+1;if(accept_run>max_accept)max_accept=accept_run;
   end else accept_run=0;
   if(dut.fragment_retired)begin
    if(!dut.fragment_color_write||!dut.fragment_depth_write||dut.fragment_address!==got[9:0]||
       dut.fragment_depth!==((kind==2)?24'd0:24'd123)||dut.fragment_color!==(16'h4000+got))
     $fatal(1,"tile fragment%0d addr%0d depth%h color%h",got,dut.fragment_address,dut.fragment_depth,dut.fragment_color);
    got=got+1;commit_run=commit_run+1;if(commit_run>max_commit)max_commit=commit_run;
   end else commit_run=0;
   if(fault)$fatal(1,"tile coefficient throughput fault");
  end
 end
 task tick;begin #5;clk=1;#5;clk=0;end endtask
 initial begin
  for(kind=0;kind<3;kind=kind+1)begin
   reset=1;tick();reset=0;tick();
   for(i=0;i<1024;i=i+1)begin pix_we=3;pix_addr=i;tick();end
   pix_we=0;
   for(i=0;i<80;i=i+1)record[i]=0;
   record[0]=(kind==0)?32'h00070103:32'h00070100;
   record[1]=32'h00040002;record[6]=640;record[7]=480;
   record[10]=32;record[11]=32;
   record[12]=32'h258000;record[13]=32;record[14]=32;record[15]=64;
   if(kind==0)record[16]=123;
   else begin
    record[10]=64;record[11]=64;
    record[22]=1024;record[25]=1024; // Q4: (0,0),(64,0),(0,64).
    record[27]=1024;record[29]=1;
    record[30]=-1024;record[31]=-1024;record[32]=1048576;
    record[34]=1024;record[37]=1;
    record[38]=(kind==2)?0:(123<<16);
    record[39]=(kind==2)?32'h80000000:0; // Exact minimum signed-64 seed.
    record[63]=16; // q=2^36 (normalized 1/2).
    record[70]=32'h04000000; // U.dx=(1/2)*2^27.
    record[78]=32'h04000000; // V.dy=(1/2)*2^27.
   end
   // Reverse loading plus a seed rewrite coincident with cmd_valid proves
   // the native seed read cannot collide with that last accepted write.
   for(i=79;i>=0;i=i-1)begin cmd_word_index=i;cmd_word_data=record[i];cmd_word_we=1;tick();end
   cmd_word_index=39;cmd_word_data=record[39];cmd_valid=1;tick();
   cmd_word_we=0;cmd_valid=0;steps=0;
   while(!done&&steps<10000)begin tick();steps=steps+1;end
   if(!done||got!=1024||max_accept!=1024||max_commit!=1024)
    $fatal(1,"tile kind%0d got%0d maxaccept%0d maxcommit%0d steps%0d",kind,got,max_accept,max_commit,steps);
   $display("PASS tile %s:1024 consecutive accepts and commits, elapsed%0d",kind==0?"sprite":(kind==1?"triangle":"minseed"),steps);
  end
  $finish;
 end
endmodule
