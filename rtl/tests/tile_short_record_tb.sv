`timescale 1ns/1ps
// Short prefixes must replace all live 2D state while leaving an arbitrary
// triangle tail untouched. Expected pixels/depth use simple independent shapes.
module nano_tile_short_record_tb;
 reg clk=0,reset=0,cmd_word_we=0,cmd_valid=0,pix_re=0,tex_flush=0;
 reg [1:0]pix_we=0;reg pix_write_pair=0;
 reg [6:0]cmd_word_index=0;reg [31:0]cmd_word_data=0;
 reg [9:0]cmd_tile_x=0,pix_addr=0,pix_raddr=0;reg [8:0]cmd_tile_y=0;
 reg [31:0]pix_color=0;reg [23:0]pix_depth=0;
 wire cmd_ready,busy,done,fault,pix_rvalid;wire [31:0]pix_rcolor;wire [23:0]pix_rdepth;
 wire tex_cfg_valid;wire [22:0]tex_cfg_base,tex_cfg_pitch;wire [10:0]tex_cfg_width,tex_cfg_height;
 wire tex_cfg_repeat_u,tex_cfg_repeat_v,tex_req_valid;
 wire signed [31:0]tex_req_x,tex_req_y;wire [5:0]tex_req_tag;
 wire tex_cfg_ready=1,tex_fault=0,tex_rsp_ready;
 reg tex_rsp_valid=0;reg [15:0]tex_rsp_texel=0;reg [5:0]tex_rsp_tag=0;
 wire tex_req_ready=!tex_rsp_valid||tex_rsp_ready;
 nano_tile_renderer dut(.*);
 reg [31:0]record[0:79];reg [15:0]expected_color[0:1023];reg [23:0]expected_depth[0:1023];
 integer i,p,xp,yp,steps,short_cases=0,pixel_checks=0,texture_requests=0,keyed_pixels=0;
 reg watch_short=0;
 function automatic [15:0]texel(input integer xx,input integer yy);begin texel=16'h4000+8*yy+xx;end endfunction
 function automatic [15:0]packed_rgb(input [23:0]rgb);begin packed_rgb={rgb[7:3],rgb[15:10],rgb[23:19]};end endfunction
 task tick;begin #5;clk=1;#5;clk=0;end endtask
 always @(posedge clk)begin
  if(reset)tex_rsp_valid<=0;
  else if(tex_req_ready)begin
   tex_rsp_valid<=tex_req_valid;
   if(tex_req_valid)begin
    if(tex_req_x<0||tex_req_x>=8||tex_req_y<0||tex_req_y>=8)$fatal(1,"short sprite generated invalid coordinates");
    tex_rsp_texel<=texel(tex_req_x,tex_req_y);tex_rsp_tag<=tex_req_tag;texture_requests=texture_requests+1;
   end
  end
  if(!reset&&watch_short&&busy)begin
   if(dut.state==9||dut.plane_enable!==0||dut.grid_step||dut.coefficient_banks[0].read_coefficient)
    $fatal(1,"short 2D record consumed a stale triangle tail");
  end
 end
 task base_2d(input integer primitive_kind);begin
  for(i=0;i<20;i=i+1)record[i]=0;
  record[0]=32'h00070200|primitive_kind;record[1]=32'h00000200;
  record[2]=24'h112233;record[6]=32;record[7]=32;record[16]=24'h123456;
 end endtask
 task submit(input integer words);begin
  if(!cmd_ready)$fatal(1,"short record submitted while busy");
  for(i=words-1;i>=0;i=i-1)begin cmd_word_we=1;cmd_word_index=i;cmd_word_data=record[i];tick();end
  cmd_word_we=0;cmd_valid=1;tick();cmd_valid=0;steps=0;
  while(!fault&&!done&&steps<1000)begin tick();steps=steps+1;end
  if(fault||!done||busy)$fatal(1,"short record faulted or failed to finish kind%0d",record[0][7:0]);
  tick();
 end endtask
 task inspect;begin
  pix_re=1;
  for(i=0;i<1024;i=i+1)begin
   pix_raddr=i;tick();
   if(!pix_rvalid||pix_rcolor!=={expected_color[(i&1022)+1],expected_color[i&1022]}||pix_rdepth!==expected_depth[i])
    $fatal(1,"short case%0d pixel%0d mismatch color%h depth%h expected%h",short_cases,i,pix_rcolor,pix_rdepth,expected_depth[i]);
   pixel_checks=pixel_checks+1;
  end
  pix_re=0;tick();short_cases=short_cases+1;
 end endtask
 task expect_rect;begin
  for(yp=record[9];yp<record[11];yp=yp+1)for(xp=record[8];xp<record[10];xp=xp+1)begin
   expected_color[32*yp+xp]=packed_rgb(record[2]);expected_depth[32*yp+xp]=record[16];
  end
 end endtask
 initial begin
  reset=1;tick();reset=0;tick();
  for(i=0;i<1024;i=i+1)begin expected_color[i]=16'hface;expected_depth[i]=24'hfedcba;end
  pix_we=3;pix_write_pair=1;pix_color=32'hfaceface;pix_depth=24'hfedcba;
  for(i=0;i<512;i=i+1)begin pix_addr=2*i;tick();end
  pix_we=0;pix_write_pair=0;
  // Leave nonzero coefficients and near-overflow edges in a completed inert
  // triangle. Its final sample consumes no subsequent increment.
  for(i=0;i<80;i=i+1)record[i]=0;
  record[0]=32'h00000200;record[6]=32;record[7]=32;record[10]=1;record[11]=1;
  for(p=0;p<3;p=p+1)begin record[26+4*p]=1;record[28+4*p]=32'h7ffffff7;record[29+4*p]=1;end
  for(p=0;p<7;p=p+1)begin
   record[38+6*p]=32'hffffffff;record[39+6*p]=32'h7fffffff;
   record[40+6*p]=1;record[42+6*p]=1;
  end
  submit(80);watch_short=1;
  base_2d(1);record[8]=2;record[9]=2;record[10]=6;record[11]=4;
  expect_rect();submit(20);inspect();
  base_2d(2);record[2]=24'h445566;record[16]=24'h456789;
  record[17]=1;record[18]=1;record[20]=5;record[21]=5;
  for(xp=1;xp<=5;xp=xp+1)begin expected_color[33*xp]=packed_rgb(record[2]);expected_depth[33*xp]=record[16];end
  submit(22);inspect();
  base_2d(3);record[0]=32'h000f0203;record[1]=32'h00040202;record[3]=texel(2,3);
  record[8]=4;record[9]=1;record[10]=8;record[11]=4;
  record[12]=32'h258000;record[13]=8;record[14]=8;record[15]=16;
  record[17]=1;record[18]=2;record[16]=24'h789abc;
  for(yp=1;yp<4;yp=yp+1)for(xp=4;xp<8;xp=xp+1)begin
   if(texel(xp-3,yp+1)==record[3])keyed_pixels=keyed_pixels+1;
   else begin expected_color[32*yp+xp]=texel(xp-3,yp+1);expected_depth[32*yp+xp]=record[16];end
  end
  submit(20);inspect();
  // Reset keeps the attachment and coefficient RAMs resident. The next short
  // prefix must work without recapturing or zeroing any absent field.
  reset=1;tick();reset=0;tick();
  base_2d(1);record[2]=24'h778899;record[16]=24'h012345;
  record[8]=10;record[9]=2;record[10]=12;record[11]=4;
  expect_rect();submit(20);inspect();
  if(texture_requests!=12||keyed_pixels!=1)$fatal(1,"short keyed sprite did not sample every source or preserve its key");
  $display("PASS short records: cases%0d pixels%0d textures%0d keyed%0d reset1",short_cases,pixel_checks,texture_requests,keyed_pixels);
  $finish;
 end
endmodule
