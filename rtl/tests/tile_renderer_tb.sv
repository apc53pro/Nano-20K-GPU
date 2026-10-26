`timescale 1ns/1ps
// Canonical monolithic SRR supplies packed commands and every expected
// RGB565/D24 attachment value. The texture interface responder deliberately
// stalls requests; this is a raster unit test, not cache or SDRAM qualification.
module nano_tile_testbench;
 reg clk=0,reset=0,cmd_word_we=0,cmd_valid=0,pix_we=0,pix_re=0,tex_flush=0;
 reg [6:0]cmd_word_index=0;reg[31:0]cmd_word_data=0;reg[9:0]cmd_tile_x=0;reg[8:0]cmd_tile_y=0;
 wire cmd_ready,busy,done,fault;reg[9:0]pix_addr=0,pix_raddr=0;reg[15:0]pix_color=0;reg[23:0]pix_depth=0;
 wire pix_rvalid;wire[15:0]pix_rcolor;wire[23:0]pix_rdepth;
 wire tex_cfg_valid;wire[22:0]tex_cfg_base,tex_cfg_pitch;wire[10:0]tex_cfg_width,tex_cfg_height;
 wire tex_cfg_repeat_u,tex_cfg_repeat_v;wire tex_req_valid;wire signed[31:0]tex_req_x,tex_req_y;wire[5:0]tex_req_tag;
 reg tex_rsp_valid=0;wire tex_rsp_ready;reg[15:0]tex_rsp_texel=0;reg[5:0]tex_rsp_tag=0;
 wire tex_fault=0; integer cycles=0;reg active=0;integer delay_count=0;reg[15:0]held_texel;reg[5:0]held_tag;
 reg repeat_u=0,repeat_v=0;reg[15:0]image[0:63];
 wire tex_cfg_ready=cycles%7!=0;
 wire tex_req_ready=!active&&!tex_rsp_valid&&(cycles%3!=0);
 nano_tile_renderer dut(.*);
 integer ix,iy;
 always@(posedge clk)begin
  cycles<=cycles+1;
  if(reset)begin tex_rsp_valid<=0;active<=0;delay_count<=0;end
  else begin
   if(tex_cfg_valid&&tex_cfg_ready)begin repeat_u<=tex_cfg_repeat_u;repeat_v<=tex_cfg_repeat_v;end
   if(tex_rsp_valid&&tex_rsp_ready)tex_rsp_valid<=0;
   if(active)begin
    if(delay_count==0)begin tex_rsp_texel<=held_texel;tex_rsp_tag<=held_tag;tex_rsp_valid<=1;active<=0;end
    else delay_count<=delay_count-1;
   end
   if(tex_req_valid&&tex_req_ready)begin
    ix=$signed(tex_req_x);iy=$signed(tex_req_y);
    if(repeat_u)ix=ix&7;else begin if(ix<0)ix=0; if(ix>7)ix=7;end
    if(repeat_v)iy=iy&7;else begin if(iy<0)iy=0; if(iy>7)iy=7;end
    held_texel<=image[iy*8+ix];held_tag<=tex_req_tag;active<=1;delay_count<=cycles%5;
   end
  end
 end
 task tick;begin #5;clk=1;#5;clk=0;end endtask
 integer fd,ncases,c,tx,ty,ncmd,j,k,n,rv,steps;reg[15:0]clear_color;reg[23:0]clear_depth;
 reg[15:0]ec[0:1023];reg[23:0]ez[0:1023];
 string vectors_path;
 task require_read;
  input integer count;
  begin if(rv!=count)$fatal(1,"truncated or malformed tile vectors at case%0d",c);end
 endtask
 initial begin
  if(!$value$plusargs("vectors=%s",vectors_path))$fatal(1,"use +vectors=PATH");
  fd=$fopen(vectors_path,"r");if(!fd)$fatal(1,"cannot open tile vectors: %s",vectors_path);
  rv=$fscanf(fd,"%d",ncases);require_read(1);
  if(ncases<=0||ncases>10000)$fatal(1,"invalid tile vector case count");
  for(c=0;c<ncases;c=c+1)begin
   reset=1;tick();reset=0;tick();
   rv=$fscanf(fd,"%d %d %h %h %d",tx,ty,clear_color,clear_depth,ncmd);require_read(5);
   if(ncmd<=0||ncmd>1024)$fatal(1,"invalid tile command count");
   cmd_tile_x=tx;cmd_tile_y=ty;
   for(j=0;j<64;j=j+1)begin rv=$fscanf(fd,"%h",image[j]);require_read(1);end
   for(j=0;j<1024;j=j+1)begin pix_we=1;pix_addr=j;pix_color=clear_color;pix_depth=clear_depth;tick();end
   pix_we=0;tick();
   for(n=0;n<ncmd;n=n+1)begin
    for(j=0;j<80;j=j+1)begin rv=$fscanf(fd,"%h",cmd_word_data);require_read(1);cmd_word_index=j;cmd_word_we=1;tick();end
    cmd_word_we=0;cmd_valid=1;tick();cmd_valid=0;
    steps=0;while(!done&&!fault&&steps<200000)begin tick();steps=steps+1;end
    if(fault||steps==200000)$fatal(1,"case%0d command%0d fault=%b state=%d",c,n,fault,dut.state);
    tick();
   end
   for(j=0;j<1024;j=j+1)begin rv=$fscanf(fd,"%h %h",ec[j],ez[j]);require_read(2);end
   for(j=0;j<1024;j=j+1)begin
    pix_re=1;pix_raddr=j;tick();
    if(!pix_rvalid||pix_rcolor!==ec[j]||pix_rdepth!==ez[j])$fatal(1,"case%0d pixel%0d color%h expected%h depth%h expected%h",c,j,pix_rcolor,ec[j],pix_rdepth,ez[j]);
   end
   pix_re=0;tick();$display("PASS tile case %0d",c);
  end
  $fclose(fd);$display("%0d/%0d tile oracle cases passed",ncases,ncases);$finish;
 end
endmodule
