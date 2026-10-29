`timescale 1ns/1ps
// An empty clipped draw must still submit every present texture descriptor.
// Configuration may stall and may reject: neither path may touch attachments.
module nano_tile_setup_tb;
 reg clk=0,reset=0,cmd_word_we=0,cmd_valid=0,pix_we=0,pix_re=0,tex_flush=0;
 reg [6:0]cmd_word_index=0;reg[31:0]cmd_word_data=0;reg[9:0]cmd_tile_x=0;reg[8:0]cmd_tile_y=0;
 wire cmd_ready,busy,done,fault;reg[9:0]pix_addr=0,pix_raddr=0;reg[15:0]pix_color=0;reg[23:0]pix_depth=0;
 wire pix_rvalid;wire[15:0]pix_rcolor;wire[23:0]pix_rdepth;
 wire tex_cfg_valid;wire[22:0]tex_cfg_base,tex_cfg_pitch;wire[10:0]tex_cfg_width,tex_cfg_height;
 wire tex_cfg_repeat_u,tex_cfg_repeat_v;wire tex_req_valid;wire signed[31:0]tex_req_x,tex_req_y;wire[5:0]tex_req_tag;
 reg tex_cfg_ready=0;wire tex_req_ready=1,tex_rsp_valid=0;wire[15:0]tex_rsp_texel=0;wire[5:0]tex_rsp_tag=0;
 wire tex_rsp_ready;reg tex_fault=0;
 nano_tile_renderer dut(.*);

 reg [31:0] record[0:79];
 integer i,kind,reject,steps,cases=0,held=0;
 task tick;begin #5;clk=1;#5;clk=0;end endtask
 task no_writes;begin
  if(dut.color_we||dut.depth_we||tex_req_valid)
   $fatal(1,"empty setup generated attachment or texture request side effect");
 end endtask
 initial begin
  for(kind=0;kind<2;kind=kind+1)for(reject=0;reject<2;reject=reject+1)begin
   reset=1;tex_fault=0;tex_cfg_ready=0;tick();reset=0;tick();
   for(i=0;i<80;i=i+1)record[i]=0;
   record[0]=kind?32'h00050103:32'h00050101;
   record[1]=kind?32'h00040002:32'h00040000;
   record[2]=24'h112233;record[6]=640;record[7]=480;
   // This raw positive eight-pixel span is entirely left of the viewport.
   // Full signed bounds must remain empty rather than aliasing into a tile.
   record[8]=32'h80000000;record[10]=32'h80000008;
   record[9]=0;record[11]=8;
   record[12]=32'h258000;record[13]=reject?0:8;record[14]=8;record[15]=16;
   // Zero descriptor width is legal at the tile prefix but the cache rejects
   // it. For a sprite with positive raw spans that would independently fail
   // source bounds, use zero raw width so the descriptor owns the rejection.
   if(kind&&reject)record[10]=record[8];
   for(i=79;i>=0;i=i-1)begin
    cmd_word_index=i;cmd_word_data=record[i];cmd_word_we=1;tick();
   end
   cmd_word_we=0;cmd_valid=1;tick();cmd_valid=0;steps=0;
   while(!tex_cfg_valid&&!fault&&!done&&steps<30)begin tick();no_writes();steps=steps+1;end
   if(!tex_cfg_valid||fault||done||!busy||!dut.window_empty)
    $fatal(1,"empty descriptor was skipped kind%0d reject%0d state%0d",kind,reject,dut.state);
   repeat(5)begin
    tick();no_writes();held=held+1;
    if(!tex_cfg_valid||fault||done||!busy||tex_cfg_base!==23'h258000||
       tex_cfg_width!==(reject?11'd0:11'd8)||tex_cfg_height!==11'd8||tex_cfg_pitch!==23'd16)
     $fatal(1,"configuration request changed under setup backpressure");
   end
   tex_cfg_ready=1;tick();no_writes();
   if(reject)begin
    tex_fault=1;tick();no_writes();
    if(!fault||done||busy)$fatal(1,"empty invalid descriptor did not propagate its cache fault");
   end else begin
    steps=0;while(!done&&!fault&&steps<5)begin tick();no_writes();steps=steps+1;end
    if(!done||fault||busy)$fatal(1,"empty valid descriptor did not finish after acceptance");
   end
   cases=cases+1;
  end
  $display("PASS %0d empty-descriptor setup cases, %0d held configuration edges",cases,held);
  $finish;
 end
endmodule
