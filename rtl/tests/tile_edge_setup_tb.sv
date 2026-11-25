`timescale 1ns/1ps
// Check the edge equations directly in signed64, independent of the DSP's
// launch/capture schedule. Single-pixel windows isolate setup from row steps.
module nano_tile_edge_setup_tb;
 reg clk=0,reset=0,cmd_word_we=0,cmd_valid=0,pix_re=0,tex_flush=0;
 reg [1:0] pix_we=0;reg pix_write_pair=0;
 reg [6:0]cmd_word_index=0;reg[31:0]cmd_word_data=0;reg[9:0]cmd_tile_x=0;reg[8:0]cmd_tile_y=0;
 wire cmd_ready,busy,done,fault;reg[9:0]pix_addr=0,pix_raddr=0;reg[31:0]pix_color=0;reg[23:0]pix_depth=0;
 wire pix_rvalid;wire[31:0]pix_rcolor;wire[23:0]pix_rdepth;
 reg read_odd=0;
 always @(posedge clk) if(pix_re&&cmd_ready&&!reset)read_odd<=pix_raddr[0];
 wire[15:0]read_pixel_color=read_odd?pix_rcolor[31:16]:pix_rcolor[15:0];
 wire tex_cfg_valid;wire[22:0]tex_cfg_base,tex_cfg_pitch;wire[10:0]tex_cfg_width,tex_cfg_height;
 wire tex_cfg_repeat_u,tex_cfg_repeat_v;wire tex_req_valid;wire signed[31:0]tex_req_x,tex_req_y;wire[5:0]tex_req_tag;
 wire tex_cfg_ready=1,tex_req_ready=1,tex_rsp_valid=0;wire[15:0]tex_rsp_texel=0;wire[5:0]tex_rsp_tag=0;
 wire tex_rsp_ready,tex_fault=0;
 nano_tile_renderer dut(.*);

 reg [31:0] record[0:79];
 reg signed [63:0] expected[0:2];
 reg signed [63:0] a,b,c,center_x,center_y;
 integer i,e,k,phase,sign_case,steps,setup_edges,max_setup_edges=0;
 integer equation_cases=0,reset_cases=0,overflow_cases=0,reuse_cases=0;
 integer start_x,start_y,sample_address,write_count=0;
 reg watch_writes=0;
 task tick;begin #5;clk=1;#5;clk=0;end endtask
 always @(posedge clk) if(!reset&&watch_writes)begin
  if(dut.color_we||dut.depth_we)write_count=write_count+1;
  if(dut.state!=8&&dut.state!=17&&(dut.color_we||dut.depth_we))
   $fatal(1,"edge setup wrote attachments before raster state%0d",dut.state);
  if(tex_cfg_valid||tex_req_valid)$fatal(1,"untextured edge setup requested texture data");
 end
 task reset_control;begin
  watch_writes=0;
  cmd_valid=0;cmd_word_we=0;pix_we=0;pix_re=0;
  reset=1;tick();reset=0;tick();
  if(fault||busy||!cmd_ready)$fatal(1,"reset did not restore idle control");
 end endtask
 task write_record_word(input integer index);begin
  cmd_word_index=index;cmd_word_data=record[index];cmd_word_we=1;tick();cmd_word_we=0;
 end endtask
 task make_record(input integer variant);begin
  for(i=0;i<80;i=i+1)record[i]=0;
  // Distinct clipping sources; the start must be the intersection, not the
  // immutable attribute anchor or either raw/tile origin alone.
  cmd_tile_x=(variant%2)?608:32;cmd_tile_y=(variant%2)?448:64;
  start_x=cmd_tile_x+3+(variant%13);start_y=cmd_tile_y+2+(variant%11);
  record[0]=32'h00010100;record[2]=24'h112233;
  record[4]=start_x;record[5]=start_y;record[6]=640;record[7]=480;
  record[8]=cmd_tile_x-2;record[9]=cmd_tile_y-1;
  record[10]=start_x+1;record[11]=start_y+1;
  for(e=0;e<3;e=e+1)begin
   case((variant+e)%6)
    0:begin a=131071;b=-131072;end
    1:begin a=-131072;b=131071;end
    2:begin a=9173+variant*7;b=-3211-e*11;end
    3:begin a=-17311-e*13;b=7199+variant*3;end
    4:begin a=1;b=-1;end
    default:begin a=-1;b=1;end
   endcase
   c=(e+1)*100003-variant*17017;
   record[26+4*e]=a[31:0];record[27+4*e]=b[31:0];record[28+4*e]=c[31:0];
   record[29+4*e]=1;
  end
 end endtask
 task load_record;begin
  if(!cmd_ready)$fatal(1,"record loaded while renderer unavailable");
  for(i=79;i>=0;i=i-1)write_record_word(i);
 end endtask
 task launch;begin
  write_count=0;watch_writes=1;cmd_valid=1;tick();cmd_valid=0;steps=0;
  while(dut.state!=3&&!fault&&!done&&steps<20)begin tick();steps=steps+1;end
  if(dut.state!=3||fault||done)$fatal(1,"triangle did not enter edge setup");
 end endtask
 task expected_edges;begin
  center_x=start_x;center_x=center_x*16+8;
  center_y=start_y;center_y=center_y*16+8;
  for(e=0;e<3;e=e+1)begin
   a=$signed(record[26+4*e]);b=$signed(record[27+4*e]);c=$signed(record[28+4*e]);
   expected[e]=a*center_x+b*center_y+c;
  end
 end endtask
 task check_equations;begin
  expected_edges();setup_edges=0;
  while(dut.state!=8&&!fault&&!done&&setup_edges<12)begin tick();setup_edges=setup_edges+1;end
  if(fault||done||dut.state!=8||setup_edges>9)
   $fatal(1,"edge setup failed nine-edge budget: edges%0d state%0d fault%0d",setup_edges,dut.state,fault);
  if(setup_edges>max_setup_edges)max_setup_edges=setup_edges;
  if(write_count!=0)$fatal(1,"setup committed a sample before edge results settled");
  for(e=0;e<3;e=e+1)begin
   if(expected[e] < -64'sd2147483648 || expected[e] > 64'sd2147483647)
    $fatal(1,"invalid nonoverflow test equation");
   if($signed(dut.edge_row[e])!==expected[e]||$signed(dut.edge_current[e])!==expected[e])
    $fatal(1,"edge%0d signed64 equation mismatch expected%0d row%0d current%0d",e,expected[e],$signed(dut.edge_row[e]),$signed(dut.edge_current[e]));
  end
  equation_cases=equation_cases+1;
  steps=0;while(!done&&!fault&&steps<10)begin tick();steps=steps+1;end
  if(fault||!done)$fatal(1,"single-sample triangle did not retire");
  tick();watch_writes=0;
 end endtask
 task sentinel;begin
  watch_writes=0;
  sample_address=(start_y-cmd_tile_y)*32+(start_x-cmd_tile_x);
  pix_we=3;pix_addr=sample_address;pix_color=16'hbeef;pix_depth=24'h654321;tick();pix_we=0;
 end endtask
 task inspect_sentinel;begin
  pix_re=1;pix_raddr=sample_address;tick();pix_re=0;
  if(!pix_rvalid||read_pixel_color!==16'hbeef||pix_rdepth!==24'h654321)
   $fatal(1,"interrupted or overflowing edge setup modified attachments");
 end endtask
 initial begin
  reset_control();
  // No reset between these draws: changed signed coefficients and C values
  // must replace all three earlier results in the same physical multiplier.
  for(k=0;k<24;k=k+1)begin make_record(k);load_record();launch();check_equations();end
  // Reuse a retained record immediately, with only its C words rewritten.
  // This also catches a capture-index mux accidentally following live launches.
  for(k=0;k<3;k=k+1)begin
   for(e=0;e<3;e=e+1)begin record[28+4*e]=$signed(record[28+4*e])+1009*(e+1);write_record_word(28+4*e);end
   launch();check_equations();reuse_cases=reuse_cases+1;
  end
  // Assert reset at every edge of the setup interval. Header RAM/registers
  // survive, so launch the retained record again immediately after reset.
  for(phase=0;phase<9;phase=phase+1)begin
   reset_control();make_record(phase+30);load_record();sentinel();launch();
   repeat(phase)tick();
   if(dut.state!=3||fault||write_count!=0)$fatal(1,"reset checkpoint left setup prematurely");
   reset_control();inspect_sentinel();
   launch();check_equations();reset_cases=reset_cases+1;
  end
  // Every edge's signed32 range check must retain both overflow signs, even
  // when the first bad edge is the last result emerging from the pipeline.
  for(k=0;k<3;k=k+1)for(sign_case=0;sign_case<2;sign_case=sign_case+1)begin
   reset_control();make_record(51);
   for(e=0;e<3;e=e+1)begin record[26+4*e]=0;record[27+4*e]=0;record[28+4*e]=1;end
   record[26+4*k]=sign_case?-32'sd131072:32'sd131071;
   record[27+4*k]=sign_case?-32'sd131072:32'sd131071;
   record[28+4*k]=sign_case?32'h80000000:32'h7fffffff;
   expected_edges();
   if(expected[k]>=-64'sd2147483648&&expected[k]<=64'sd2147483647)$fatal(1,"overflow vector did not exceed signed32");
   load_record();sentinel();launch();steps=0;
   while(!fault&&!done&&steps<12)begin tick();steps=steps+1;end
   if(!fault||done||busy||write_count!=0||steps>9)$fatal(1,"edge%0d overflow sign%0d not rejected before writes",k,sign_case);
   repeat(3)tick();
   if(!fault||write_count!=0)$fatal(1,"setup fault did not remain sticky");
   reset_control();inspect_sentinel();overflow_cases=overflow_cases+1;
  end
  $display("PASS edge setup: equations%0d resets%0d overflows%0d reuse%0d max_edges%0d",equation_cases,reset_cases,overflow_cases,reuse_cases,max_setup_edges);
  $finish;
 end
endmodule
