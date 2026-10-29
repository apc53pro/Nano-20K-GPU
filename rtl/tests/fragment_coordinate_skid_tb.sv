`timescale 1ns/1ps
// The full-width standalone contract and bounded tile contract share the same
// stall schedule. Scores are derived from accepted input identities, not DUT
// internal data, and exercise all signed coordinate endpoints.
module nano_fragment_coordinate_skid_case #(parameter COORD_BITS=32)(output reg done=0);
 reg clk=0,reset=1;
 always #5 clk=~clk;
 reg color_write=1,depth_test=0,depth_write=1,key_enable=0,in_valid=0,in_sprite=1;
 reg [1:0] depth_func=0;
 reg [15:0] key_color=0;
 reg [9:0] in_address=0;
 reg [23:0] in_depth=0,in_factor=0;
 reg signed [63:0] in_u=0,in_v=0,in_q=64'd536870912;
 reg signed [31:0] in_source_x=0,in_source_y=0;
 wire [23:0] stored_depth=0;
 wire in_ready,busy,fault,retired,commit_color_write,commit_depth_write;
 wire [9:0] commit_address;
 wire [23:0] commit_depth;
 wire [15:0] commit_color;
 wire tex_req_valid;
 reg tex_req_ready=0,response_gate=0,force_stall=0;
 wire signed [31:0] tex_req_x,tex_req_y;
 wire [5:0] tex_req_tag;
 wire tex_rsp_valid,tex_rsp_ready;
 wire [15:0] tex_rsp_texel;
 wire [5:0] tex_rsp_tag;
 reg [5:0] response_tag[0:31];
 reg [15:0] response_color[0:31];
 reg [4:0] response_head=0,response_tail=0;
 reg [5:0] response_count=0;
 assign tex_rsp_valid=response_count!=0&&response_gate;
 assign tex_rsp_tag=response_tag[response_head];
 assign tex_rsp_texel=response_color[response_head];
 nano_fragment_pipeline #(.COORD_BITS(COORD_BITS)) dut(.*);

 function automatic signed [31:0] coordinate;
  input integer index;
  begin
   case(index%8)
   0:coordinate=COORD_BITS==32?32'sh80000000:-32'sd32768;
   1:coordinate=COORD_BITS==32?32'sh7fffffff:32'sd32767;
   2:coordinate=-1;3:coordinate=0;4:coordinate=1;
   5:coordinate=1024;6:coordinate=-1024;
   default:coordinate=(index*131)%32768;
   endcase
  end
 endfunction
 function automatic [23:0] factor;
  input integer index;
  reg [7:0] r,g,b;
  begin r=255-index;g=index^8'h5a;b=index*3;factor={b,g,r};end
 endfunction
 function automatic [15:0] texel;
  input integer index;
  begin texel=16'h8421^(index*73);end
 endfunction
 function automatic [7:0] modulate;
  input [7:0] channel,multiplier;
  integer product,rounded;
  begin product=channel*multiplier;rounded=product+128;modulate=(rounded+(rounded>>8))>>8;end
 endfunction
 function automatic [15:0] shaded;
  input integer index;
  reg [15:0] t;
  reg [23:0] f;
  reg [7:0] r,g,b;
  begin
   t=texel(index);f=factor(index);
   r=modulate({t[15:11],t[15:13]},f[7:0]);
   g=modulate({t[10:5],t[10:9]},f[15:8]);
   b=modulate({t[4:0],t[4:2]},f[23:16]);
   shaded={r[7:3],g[7:2],b[7:3]};
  end
 endfunction
 integer accepted=0,requested=0,committed=0,cycles=0,target=1024;
 integer simultaneous=0,skid_fills=0,skid_drains=0,held_checks=0;
 integer commit_run=0,max_commit_run=0,accept_run=0,max_accept_run=0;
 integer ready_isolation_checks=0;
 reg [31:0] random_state=32'h93753a2d;
 reg planned_ready,probe_ready,probe_uv_ready;
 reg held=0;
 reg signed [31:0] held_x,held_y;
 reg [5:0] held_tag;
 wire request_fire=tex_req_valid&&tex_req_ready;
 wire response_fire=tex_rsp_valid&&tex_rsp_ready;
 always @(negedge clk) begin
  if(!reset)begin
   in_valid=accepted<target;in_address=accepted;in_depth=24'h100000+accepted;
   in_factor=factor(accepted);in_source_x=coordinate(accepted);in_source_y=coordinate(accepted+3);
   random_state={random_state[30:0],random_state[31]^random_state[21]^random_state[1]^random_state[0]};
   if(force_stall)begin tex_req_ready=0;response_gate=0;end
   else if(cycles<256)begin tex_req_ready=1;response_gate=1;end
   else if(cycles<512)begin tex_req_ready=cycles%2==0;response_gate=cycles%4<2;end
   else if(cycles<1024)begin tex_req_ready=cycles%64>=48;response_gate=cycles%53>=30;end
   else begin tex_req_ready=random_state[3:0]>=4;response_gate=random_state[6:4]!=0;end
   // Change only the downstream ready level between clock edges. Neither
   // upstream input readiness nor the UV pipeline enable may respond to it.
   planned_ready=tex_req_ready;
   #1;probe_ready=in_ready;probe_uv_ready=dut.uv_in_ready;tex_req_ready=~planned_ready;
   #1;
   if(in_ready!==probe_ready||dut.uv_in_ready!==probe_uv_ready)
    $fatal(1,"cache readiness propagated combinationally upstream bits%0d",COORD_BITS);
   tex_req_ready=planned_ready;ready_isolation_checks=ready_isolation_checks+1;
  end else begin in_valid=0;tex_req_ready=0;response_gate=0;end
 end
 always @(posedge clk) begin
  if(reset)begin
   accepted=0;requested=0;committed=0;cycles=0;held=0;
   simultaneous=0;skid_fills=0;skid_drains=0;held_checks=0;ready_isolation_checks=0;
   commit_run=0;max_commit_run=0;accept_run=0;max_accept_run=0;
   response_head<=0;response_tail<=0;response_count<=0;
  end else begin
   cycles=cycles+1;
   if(fault)$fatal(1,"coordinate skid bits%0d pipeline fault",COORD_BITS);
   if(dut.coordinate_skid_valid&&!dut.coordinate_valid)$fatal(1,"orphan skid entry");
   if(in_valid&&in_ready)begin
    accepted=accepted+1;accept_run=accept_run+1;
    if(accept_run>max_accept_run)max_accept_run=accept_run;
   end else accept_run=0;
   if(held)begin
    if(!tex_req_valid||tex_req_x!==held_x||tex_req_y!==held_y||tex_req_tag!==held_tag)
     $fatal(1,"coordinate/tag changed under backpressure bits%0d",COORD_BITS);
    held_checks=held_checks+1;
   end
   held=tex_req_valid&&!tex_req_ready;
   held_x=tex_req_x;held_y=tex_req_y;held_tag=tex_req_tag;
   if(request_fire)begin
    if(requested>=accepted||tex_req_x!==coordinate(requested)||tex_req_y!==coordinate(requested+3)||tex_req_tag!==requested[5:0])
     $fatal(1,"request identity bits%0d index%0d x%h y%h tag%h",COORD_BITS,requested,tex_req_x,tex_req_y,tex_req_tag);
    response_tag[response_tail]<=tex_req_tag;response_color[response_tail]<=texel(requested);
    response_tail<=response_tail+1'b1;requested=requested+1;
   end
   if(response_fire)response_head<=response_head+1'b1;
   case({request_fire,response_fire})
   2'b10:response_count<=response_count+1'b1;
   2'b01:response_count<=response_count-1'b1;
   default:begin end
   endcase
   if(response_count>8)$fatal(1,"texture ownership exceeded eight slots");
   if(retired)begin
    if(!commit_color_write||!commit_depth_write||commit_address!==committed[9:0]||
       commit_depth!==(24'h100000+committed)||commit_color!==shaded(committed))
     $fatal(1,"commit identity/factor bits%0d index%0d color%h expected%h",COORD_BITS,committed,commit_color,shaded(committed));
    committed=committed+1;commit_run=commit_run+1;
    if(commit_run>max_commit_run)max_commit_run=commit_run;
   end else commit_run=0;
   if(dut.capture_coordinate&&request_fire)simultaneous=simultaneous+1;
   if(dut.capture_coordinate&&dut.coordinate_valid&&!request_fire)skid_fills=skid_fills+1;
   if(request_fire&&dut.coordinate_skid_valid)skid_drains=skid_drains+1;
   if(cycles>30000)$fatal(1,"coordinate skid timeout bits%0d",COORD_BITS);
  end
 end
 task restart;
  begin
   @(negedge clk);reset=1;
   repeat(3)@(negedge clk);
   reset=0;
  end
 endtask
 initial begin
  restart();
  wait(committed==1024&&!busy);
  @(negedge clk);
  if(accepted!=1024||requested!=1024||max_commit_run<128||max_accept_run<128||
     simultaneous<100||skid_fills<10||skid_drains!=skid_fills||held_checks<100||ready_isolation_checks<1000)
   $fatal(1,"incomplete stall coverage bits%0d simultaneous%0d fills%0d drains%0d holds%0d warm%0d/%0d",
    COORD_BITS,simultaneous,skid_fills,skid_drains,held_checks,max_accept_run,max_commit_run);
  // Invalidate a full main/skid pair and a younger read handoff, then prove
  // that a fresh stream contains no coordinates, factors or tags from before reset.
  target=64;force_stall=1;restart();
  wait(dut.coordinate_skid_valid);
  restart();force_stall=0;
  wait(committed==64&&!busy);
  @(negedge clk);
  if(accepted!=64||requested!=64)$fatal(1,"reset retained an old coordinate");
  $display("PASS coordinate skid bits%0d:1088 ordered requests/commits, signed endpoints, stalls, factors and reset",COORD_BITS);
  done=1;
 end
endmodule

module nano_fragment_coordinate_skid_tb;
 wire full_done,tile_done;
 nano_fragment_coordinate_skid_case #(.COORD_BITS(32)) full_contract(full_done);
 nano_fragment_coordinate_skid_case #(.COORD_BITS(16)) tile_contract(tile_done);
 initial begin wait(full_done&&tile_done);$finish;end
endmodule
