`timescale 1ns/1ps
// Exercise real raster commands, attachment write edges and fault timing.
// Arithmetic-overflow flags must cancel the current pending sample on the
// same edge as the original direct fault register, but a final unused sum
// must never fault a completed primitive.
module nano_tile_step_control_tb;
 reg clk=0,reset=0,cmd_word_we=0,cmd_valid=0,pix_we=0,pix_re=0,tex_flush=0;
 reg [6:0]cmd_word_index=0;reg[31:0]cmd_word_data=0;reg[9:0]cmd_tile_x=0;reg[8:0]cmd_tile_y=0;
 wire cmd_ready,busy,done,fault;reg[9:0]pix_addr=0,pix_raddr=0;reg[15:0]pix_color=0;reg[23:0]pix_depth=0;
 wire pix_rvalid;wire[15:0]pix_rcolor;wire[23:0]pix_rdepth;
 wire tex_cfg_valid;wire[22:0]tex_cfg_base,tex_cfg_pitch;wire[10:0]tex_cfg_width,tex_cfg_height;
 wire tex_cfg_repeat_u,tex_cfg_repeat_v;wire tex_req_valid;wire signed[31:0]tex_req_x,tex_req_y;wire[5:0]tex_req_tag;
 wire tex_cfg_ready=1,tex_req_ready=1,tex_rsp_valid=0;wire[15:0]tex_rsp_texel=0;wire[5:0]tex_rsp_tag=0;
 wire tex_rsp_ready,tex_fault=0;
 nano_tile_renderer dut(.*);

 reg [31:0] record[0:79];
 integer i,p,axis,neg,steps,candidates,writes,cases=0,mode=0,span=0,height=0;
 integer expected_addr,first_addr,second_addr,start_x,start_y;
 reg active=0;
 reg [15:0] expected_color;
 reg [23:0] expected_depth;
 reg signed [63:0] seed,delta;
 task tick;begin #5;clk=1;#5;clk=0;end endtask
 always @(posedge clk) if(!reset&&active)begin
  if(dut.state==8&&dut.advance_raster&&!fault)candidates=candidates+1;
  if(dut.color_we)begin
   if(mode==1)begin
    expected_addr=(start_y-448)*32+(start_x-608)+(writes/span)*32+(writes%span);
    if(dut.write_addr!==expected_addr[9:0]||dut.write_color!==16'h3102||
       !dut.depth_we||dut.write_depth!==24'h123456)
     $fatal(1,"span%0d height%0d write%0d addr%0d expected%0d",span,height,writes,dut.write_addr,expected_addr);
   end
   writes=writes+1;
  end
 end
 task base;begin
  for(i=0;i<80;i=i+1)record[i]=0;
  record[0]=32'h00050100;record[2]=24'h112233;
  record[6]=640;record[7]=480;record[10]=3;record[11]=1;
  record[28]=1;record[32]=1;record[36]=1;
 end endtask
 task launch;begin
  active=0;reset=1;tick();reset=0;tick();
  for(i=0;i<4;i=i+1)begin
   pix_we=1;pix_addr=axis?32*i:i;pix_color=16'hbeef;pix_depth=24'h654321;tick();
  end
  pix_we=0;
  for(i=0;i<80;i=i+1)begin cmd_word_index=i;cmd_word_data=record[i];cmd_word_we=1;tick();end
  cmd_word_we=0;cmd_valid=1;tick();cmd_valid=0;candidates=0;writes=0;active=1;
 end endtask
 task inspect;begin
  active=0;reset=1;tick();reset=0;tick();
  pix_re=1;pix_raddr=0;tick();
  if(!pix_rvalid||pix_rcolor!==expected_color||pix_rdepth!==expected_depth)
   $fatal(1,"overflow case%0d first pixel %h/%h expected%h/%h",cases,pix_rcolor,pix_rdepth,expected_color,expected_depth);
  pix_raddr=axis?32:1;tick();pix_re=0;
  if(!pix_rvalid||pix_rcolor!==16'hbeef||pix_rdepth!==24'h654321)
   $fatal(1,"overflow case%0d did not suppress current pending sample",cases);
 end endtask
 task expect_fault;begin
  steps=0;while(!fault&&!done&&steps<1000)begin tick();steps=steps+1;end
  if(!fault||done||busy||cmd_ready||candidates!=2)
   $fatal(1,"overflow case%0d fault%0d done%0d busy%0d candidates%0d",cases,fault,done,busy,candidates);
  repeat(4)tick();
  if(dut.state!=18)$fatal(1,"overflow controller did not reach sticky FAILED");
 end endtask
 initial begin
  cmd_tile_x=0;cmd_tile_y=0;
  for(p=0;p<7;p=p+1)for(axis=0;axis<2;axis=axis+1)for(neg=0;neg<2;neg=neg+1)begin
   base();if(axis)begin record[10]=1;record[11]=3;end
   if(p>=1&&p<=3)record[1]=1;
   if(p>=4)begin
    record[1]=32'h00040002;record[12]=32'h258000;record[13]=4;record[14]=4;record[15]=8;
    record[63]=16; // Otherwise-valid q=2^36.
   end
   seed=neg?64'sh8000000000000001:64'sh7ffffffffffffffe;
   delta=neg?-64'sd1:64'sd1;
   record[38+6*p]=seed[31:0];record[39+6*p]=seed[63:32];
   record[40+6*p+2*axis]=delta[31:0];record[41+6*p+2*axis]=delta[63:32];
   mode=0;launch();expect_fault();
   if(dut.plane_fault!==(7'b1<<p)||dut.edge_fault!==0)
    $fatal(1,"plane overflow flag lost identity case%0d",cases);
   expected_color=16'h3102;expected_depth=neg?0:24'hffffff;
   if(p>=1&&p<=3)begin
    expected_color=neg?0:(p==1?16'hf800:(p==2?16'h07e0:16'h001f));expected_depth=0;
   end
   if(p>=4)begin expected_color=16'hbeef;expected_depth=24'h654321;end
   inspect();cases=cases+1;
  end
  for(p=0;p<3;p=p+1)for(axis=0;axis<2;axis=axis+1)for(neg=0;neg<2;neg=neg+1)begin
   base();if(axis)begin record[10]=1;record[11]=3;end
   record[26+4*p+axis]=neg?-32'sd1:32'sd1;
   record[28+4*p]=neg?32'sh80000027:32'sh7fffffd8;
   mode=0;launch();expect_fault();
   if(dut.edge_fault!==(3'b1<<p)||dut.plane_fault!==0)
    $fatal(1,"edge overflow flag lost identity case%0d",cases);
   expected_color=neg?16'hbeef:16'h3102;
   expected_depth=neg?24'h654321:0;
   inspect();cases=cases+1;
  end
  // The mathematically overflowing next sum is not consumed at the last
  // sample. Width/height two therefore complete with both valid samples.
  for(axis=0;axis<2;axis=axis+1)for(neg=0;neg<2;neg=neg+1)begin
   base();record[10]=axis?1:2;record[11]=axis?2:1;
   seed=neg?64'sh8000000000000001:64'sh7ffffffffffffffe;delta=neg?-64'sd1:64'sd1;
   record[38]=seed[31:0];record[39]=seed[63:32];
   record[40+2*axis]=delta[31:0];record[41+2*axis]=delta[63:32];
   mode=0;launch();steps=0;while(!fault&&!done&&steps<1000)begin tick();steps=steps+1;end
   if(fault||!done||candidates!=2||writes!=2||dut.plane_fault!==0||dut.edge_fault!==0)
    $fatal(1,"unused final sum faulted axis%0d sign%0d",axis,neg);
   cases=cases+1;
  end
  // Trace every possible clipped tile width against independent coordinates.
  // The scissor clips a larger raw rectangle into the bottom/right tile.
  cmd_tile_x=608;cmd_tile_y=448;axis=0;mode=1;
  for(span=1;span<=32;span=span+1)begin
   base();record[0]=32'h00050101;record[16]=24'h123456;
   record[28]=0;record[32]=0;record[36]=0;
   record[4]=640-span;record[5]=448;record[8]=600;record[9]=440;record[10]=640;record[11]=480;
   start_x=640-span;start_y=448;height=32;
   launch();steps=0;while(!fault&&!done&&steps<2000)begin tick();steps=steps+1;end
   if(fault||!done||candidates!=span*height||writes!=span*height)
    $fatal(1,"clipped span%0d incomplete candidates%0d writes%0d",span,candidates,writes);
  end
  $display("PASS %0d signed plane/edge overflow timing cases and 32 clipped span traces",cases);
  $finish;
 end
endmodule
