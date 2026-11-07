`timescale 1ns/1ps
// Reject reserved fields, unsupported state and arithmetic overflow before
// corrupting attachments. Reset must cancel control without clearing RAM.
module nano_tile_fault_tb;
 reg clk=0,reset=0,cmd_word_we=0,cmd_valid=0,pix_re=0,tex_flush=0;
 reg [1:0] pix_we=0;
 reg [6:0]cmd_word_index=0;reg[31:0]cmd_word_data=0;reg[9:0]cmd_tile_x=0;reg[8:0]cmd_tile_y=0;
 wire cmd_ready,busy,done,fault;reg[9:0]pix_addr=0,pix_raddr=0;reg[15:0]pix_color=0;reg[23:0]pix_depth=0;
 wire pix_rvalid;wire[15:0]pix_rcolor;wire[23:0]pix_rdepth;
 wire tex_cfg_valid;wire[22:0]tex_cfg_base,tex_cfg_pitch;wire[10:0]tex_cfg_width,tex_cfg_height;
 wire tex_cfg_repeat_u,tex_cfg_repeat_v;wire tex_req_valid;wire signed[31:0]tex_req_x,tex_req_y;wire[5:0]tex_req_tag;
 wire tex_cfg_ready=1,tex_req_ready=1,tex_rsp_valid=0;wire[15:0]tex_rsp_texel=0;wire[5:0]tex_rsp_tag=0;
 wire tex_rsp_ready,tex_fault=0;
 nano_tile_renderer dut(.*);
 reg[31:0]record[0:79];integer i,c,steps;
 task tick;begin #5;clk=1;#5;clk=0;end endtask
 task base;begin
  for(i=0;i<80;i=i+1)record[i]=0;
  record[0]=32'h00050101;record[2]=32'h00112233;record[6]=640;record[7]=480;
  record[10]=4;record[11]=4;record[16]=24'h123456;
 end endtask
 task triangle;begin
  base();record[0]=32'h00050100;record[16]=0;
  record[28]=1;record[32]=1;record[36]=1;
 end endtask
 task launch;begin
  reset=1;tick();reset=0;tick();
  pix_we=3;pix_addr=0;pix_color=16'hbeef;pix_depth=24'h654321;tick();pix_we=0;
  for(i=0;i<80;i=i+1)begin cmd_word_index=i;cmd_word_data=record[i];cmd_word_we=1;tick();end
  cmd_word_we=0;cmd_valid=1;tick();cmd_valid=0;
 end endtask
 initial begin
  for(c=0;c<28;c=c+1)begin
   base();
   case(c)
   0:record[19]=1;
   1:record[3]=1;
   2:record[17]=1;
   3:record[79]=1;
   4:record[1]=1;
   5:begin record[0]=32'h00050102;record[8]=0;record[9]=0;record[10]=0;record[11]=0;record[20]=640;end
   6:begin record[1]=32'h00040000;record[12]=32'h80258000;record[13]=8;record[14]=8;record[15]=16;end
   7:record[12]=32'h00258000;
   8:begin record[0]=32'h00050103;record[1]=32'h00040002;record[12]=32'h00258000;record[13]=8;record[14]=8;record[15]=16;record[17]=7;end
   9:begin triangle();record[26]=32'h00020000;end
   10:begin triangle();record[26]=1;record[28]=32'h7fffffff;end
   11:begin triangle();record[38]=32'hffffffff;record[39]=32'h7fffffff;record[40]=1;end
   12:begin triangle();record[29]=2;end
   13:begin triangle();record[33]=32'h80000000;end
   14:begin triangle();record[37]=32'h00010000;end
   15:begin triangle();record[10]=641;end
   16:begin triangle();record[10]=32'h80000000;end
   17:begin triangle();record[11]=481;end
   18:begin triangle();record[11]=512;end
   19:begin record[0]=32'h00050102;record[8]=0;record[9]=0;record[10]=0;record[11]=0;record[17]=32'h80000000;end
   20:begin record[0]=32'h00050102;record[8]=0;record[9]=0;record[10]=0;record[11]=0;record[20]=32'h00010000;end
   21:begin record[0]=32'h00050102;record[8]=0;record[9]=0;record[10]=0;record[11]=0;record[18]=480;end
   22:begin record[0]=32'h00050102;record[8]=0;record[9]=0;record[10]=0;record[11]=0;record[21]=512;end
   23:begin record[0]=32'h00050103;record[1]=32'h00040002;record[12]=32'h258000;record[13]=8;record[14]=8;record[15]=16;record[17]=32'h7fffffff;end
   24:record[0]=32'h00050104;
   25:record[1]=5;
   26:record[1]=32'h00000300;
   27:record[1]=32'h00008000;
   endcase
   launch();steps=0;while(!fault&&!done&&steps<10000)begin tick();steps=steps+1;end
   if(!fault)$fatal(1,"malformed/numeric case %0d failed to fault",c);
   tick();pix_re=1;pix_raddr=0;tick();pix_re=0;
   // A sticky fault inhibits all attachment accesses; reset restores idle
   // control without clearing or writing any attachment RAM.
   reset=1;tick();reset=0;tick();pix_re=1;pix_raddr=0;tick();pix_re=0;
   if(!pix_rvalid||pix_rcolor!==16'hbeef||pix_rdepth!==24'h654321)$fatal(1,"fault case%0d modified attachment",c);
  end
  $display("PASS 28 malformed/numeric records, fault write suppression and reset retention");$finish;
 end
endmodule
