`timescale 1ns/1ps
// A seven-edge elastic texture responder represents a resident cache hit.
// This proves fragment-pipeline II1 under those interface conditions; it does
// not include raster setup, cache misses, scanout or external-memory traffic.
module nano_fragment_throughput_tb;
 reg clk=0,reset=0,color_write=1,depth_test=1,depth_write=1,key_enable=0,in_valid=0,in_sprite=0;
 reg[1:0]depth_func=0;reg[15:0]key_color=0;reg[9:0]in_address=0;reg[23:0]in_depth=123,in_factor=24'hffffff;
 reg signed[63:0]in_u=0,in_v=0,in_q=64'd536870912;reg signed[31:0]in_source_x=0,in_source_y=0;
 wire[23:0]stored_depth=24'hffffff;wire in_ready,busy,fault,retired,commit_color_write,commit_depth_write;
 wire[9:0]commit_address;wire[23:0]commit_depth;wire[15:0]commit_color;
 wire tex_req_valid,tex_req_ready;wire signed[31:0]tex_req_x,tex_req_y;wire[5:0]tex_req_tag;
 wire tex_rsp_valid,tex_rsp_ready;wire[15:0]tex_rsp_texel;wire[5:0]tex_rsp_tag;
 reg[6:0]rv=0;reg[5:0]rt[0:6];reg[15:0]rd[0:6];
 assign tex_rsp_valid=rv[6];assign tex_rsp_tag=rt[6];assign tex_rsp_texel=rd[6];
 assign tex_req_ready=!rv[6]||tex_rsp_ready;
 nano_fragment_pipeline dut(.*);
 integer j,got=0,run=0,maxrun=0,accept_run=0,max_accept=0;
 always@(posedge clk)begin
  if(reset)begin rv<=0;got=0;run=0;maxrun=0;accept_run=0;max_accept=0;end
  else begin
   if(tex_req_ready)begin
    rv<={rv[5:0],tex_req_valid};
    for(j=6;j>0;j=j-1)begin rt[j]<=rt[j-1];rd[j]<=rd[j-1];end
    rt[0]<=tex_req_tag;rd[0]<=16'h0421+tex_req_x;
   end
   if(in_valid&&in_ready)begin accept_run=accept_run+1;if(accept_run>max_accept)max_accept=accept_run;end else accept_run=0;
   if(retired)begin
    if(!commit_color_write||!commit_depth_write||commit_address!==got[9:0]||commit_depth!==24'd123||commit_color!==(16'h0421+got))
     $fatal(1,"fragment%0d addr%0d depth%h color%h expected%h",got,commit_address,commit_depth,commit_color,16'h0421+got);
    got=got+1;run=run+1;if(run>maxrun)maxrun=run;
   end else run=0;
   if(fault)$fatal(1,"pipeline fault");
  end
 end
 task tick;begin #5;clk=1;#5;clk=0;end endtask
 integer test_id,sent,clocks;reg fire;
 initial begin
  for(test_id=0;test_id<2;test_id=test_id+1)begin
   reset=1;tick();reset=0;tick();in_sprite=test_id==0;sent=0;clocks=0;
   while(sent<1024&&clocks<10000)begin
    in_valid=1;in_address=sent;in_source_x=sent;in_u=$signed({32'b0,sent[31:0]})<<<19;
    fire=in_valid&&in_ready;tick();if(fire)sent=sent+1;clocks=clocks+1;
   end
   in_valid=0;
   while(busy&&clocks<10000)begin tick();clocks=clocks+1;end
   if(got!=1024||maxrun!=1024||max_accept!=1024)$fatal(1,"test%0d got%0d maxcommits%0d maxaccepts%0d",test_id,got,maxrun,max_accept);
   $display("PASS %s:1024 consecutive accepts and commits, elapsed%0d",in_sprite?"sprite":"triangle",clocks);
  end
  $finish;
 end
endmodule
