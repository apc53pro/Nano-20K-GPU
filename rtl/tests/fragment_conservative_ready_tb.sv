`timescale 1ns/1ps
// Source/retirement scoreboard uses accepted input identities and a separate
// in-order texture responder. Forced closure targets ready's control contract;
// ordinary warm/mixed streams exercise the complete datapath without forces.
module nano_fragment_conservative_ready_case #(parameter COORD_BITS=32)(output reg done=0);
 reg clk=0,reset=1,color_write=1,depth_test=1,depth_write=1,key_enable=0,in_valid=0,in_sprite=0;
 reg[1:0]depth_func=0;reg[15:0]key_color=16'h0423;
 reg[9:0]in_address=0;reg[23:0]in_depth=123,in_factor=24'hffffff,stored_depth=500;
 reg signed[63:0]in_u=0,in_v=0,in_q=64'd536870912;
 reg signed[31:0]in_source_x=0,in_source_y=0;
 wire in_ready,busy,fault,retired,commit_color_write,commit_depth_write;
 wire[9:0]commit_address;wire[23:0]commit_depth;wire[15:0]commit_color;
 wire tex_req_valid;reg tex_req_ready=1,response_gate=1;
 wire signed[31:0]tex_req_x,tex_req_y;wire[5:0]tex_req_tag;
 wire tex_rsp_valid,tex_rsp_ready;wire[15:0]tex_rsp_texel;wire[5:0]tex_rsp_tag;
 reg[5:0]rsp_tag[0:31];reg[15:0]rsp_color[0:31];
 reg[4:0]rsp_head=0,rsp_tail=0;reg[5:0]rsp_count=0;
 assign tex_rsp_valid=rsp_count!=0&&response_gate;
 assign tex_rsp_tag=rsp_tag[rsp_head];assign tex_rsp_texel=rsp_color[rsp_head];
 nano_fragment_pipeline #(.COORD_BITS(COORD_BITS))dut(.*);
 reg[9:0]expected_address[0:255];reg[23:0]expected_depth[0:255];
 reg expected_write[0:255],expected_texture[0:255],requested_once[0:255];reg signed[31:0]expected_x[0:255],expected_y[0:255];
 integer request_order[0:255];integer expected_requests=0;
 integer accepted=0,requested=0,committed=0,cycles=0;
 integer accept_run=0,retire_run=0,max_accept=0,max_retire=0;
 integer cases=0,ready_implications=0,ready_isolation=0,blocked_holds=0,mixed_order_checks=0;
 integer identity,search_index;
 wire request_fire=tex_req_valid&&tex_req_ready;
 wire response_fire=tex_rsp_valid&&tex_rsp_ready;
 function automatic signed[31:0]coord_x(input integer index);begin coord_x=index-64;end endfunction
 function automatic signed[31:0]coord_y(input integer index);begin coord_y=(index*7)%127-63;end endfunction
 function automatic[15:0]texel(input integer index);begin texel=16'h0421+index;end endfunction
 always@(posedge clk)begin
  if(reset)begin
   accepted=0;requested=0;committed=0;cycles=0;expected_requests=0;
   accept_run=0;retire_run=0;max_accept=0;max_retire=0;
   rsp_count<=0;rsp_head<=0;rsp_tail<=0;
  end else begin
   cycles=cycles+1;
   if(fault)$fatal(1,"conservative ready fault bits%0d case%0d",COORD_BITS,cases);
   // Acceptance may reuse an occupied depth register only when that old
   // sample really leaves it on this edge. This checks the transition rather
   // than repeating the proposed in_ready expression.
   if(dut.depth_valid&&in_ready)begin
    if(!dut.depth_consumed)$fatal(1,"ready overwrites blocked depth sample bits%0d",COORD_BITS);
    ready_implications=ready_implications+1;
   end
   if(in_valid&&in_ready)begin
    if(accepted>=256)$fatal(1,"scoreboard overflow");
    expected_address[accepted]=in_address;expected_depth[accepted]=in_depth;
    expected_x[accepted]=in_sprite?in_source_x:coord_x(accepted);
    expected_y[accepted]=in_sprite?in_source_y:coord_y(accepted);
    expected_write[accepted]=(in_depth<stored_depth)&&!(key_enable&&texel(accepted)==key_color);
    expected_texture[accepted]=in_depth<stored_depth;requested_once[accepted]=0;
    if(in_depth<stored_depth)begin request_order[expected_requests]=accepted;expected_requests=expected_requests+1;end
    accepted=accepted+1;accept_run=accept_run+1;if(accept_run>max_accept)max_accept=accept_run;
   end else accept_run=0;
   if(request_fire)begin
    if(requested>=expected_requests)$fatal(1,"unexpected texture request bits%0d",COORD_BITS);
    identity=-1;
    for(search_index=0;search_index<accepted;search_index=search_index+1)
     if(expected_texture[search_index]&&!requested_once[search_index]&&tex_req_tag===search_index[5:0])begin
      if(identity!=-1)$fatal(1,"ambiguous live texture tag");identity=search_index;
     end
    if(identity<0)$fatal(1,"texture request did not match an accepted context");
    requested_once[identity]=1;
    if(tex_req_x!==expected_x[identity]||tex_req_y!==expected_y[identity]||tex_req_tag!==identity[5:0])
     $fatal(1,"texture identity bits%0d case%0d request%0d identity%0d x%0d y%0d tag%0d expected%0d/%0d",COORD_BITS,cases,requested,identity,tex_req_x,tex_req_y,tex_req_tag,expected_x[identity],expected_y[identity]);
    rsp_tag[rsp_tail]<=tex_req_tag;rsp_color[rsp_tail]<=texel(identity);rsp_tail<=rsp_tail+1'b1;
    requested=requested+1;
   end
   if(response_fire)rsp_head<=rsp_head+1'b1;
   case({request_fire,response_fire})
    2'b10:rsp_count<=rsp_count+1'b1;2'b01:rsp_count<=rsp_count-1'b1;default:begin end
   endcase
   if(rsp_count>8)$fatal(1,"texture response ownership overflow");
   if(retired)begin
    if(committed>=accepted||commit_address!==expected_address[committed]||commit_depth!==expected_depth[committed]||
       commit_color_write!==expected_write[committed]||commit_depth_write!==expected_write[committed]||
       (commit_color_write&&commit_color!==texel(committed)))
     $fatal(1,"retirement bits%0d ordinal%0d addr%0d depth%0d writes%0b/%0b expected%0b",COORD_BITS,committed,commit_address,commit_depth,commit_color_write,commit_depth_write,expected_write[committed]);
    committed=committed+1;retire_run=retire_run+1;if(retire_run>max_retire)max_retire=retire_run;
   end else retire_run=0;
   if(cycles>5000)$fatal(1,"conservative-ready timeout bits%0d",COORD_BITS);
  end
 end
 task tick;begin #4;clk=1;#1;clk=0;#1;end endtask
 task restart;
  begin in_valid=0;reset=1;stored_depth=500;key_enable=0;tex_req_ready=1;response_gate=1;
   repeat(3)tick();reset=0;tick();end
 endtask
 task drive(input integer index,input bit sprite,input bit reject_depth);
  begin
   in_valid=1;in_sprite=sprite;in_address=index;in_depth=(reject_depth?900:123)+(index%17);
   in_source_x=coord_x(index);in_source_y=coord_y(index);
   in_u=$signed(coord_x(index));in_u=in_u<<<19;
   in_v=$signed(coord_y(index));in_v=in_v<<<19;
  end
 endtask
 task drain;
  begin in_valid=0;tex_req_ready=1;response_gate=1;
   while(busy)tick();tick();
   if(committed!=accepted||requested!=expected_requests||rsp_count!=0)$fatal(1,"incomplete drain bits%0d",COORD_BITS);
  end
 endtask
 integer mode,j;
 reg[222+2*COORD_BITS:0]held_depth;
 reg sampled_ready;
 initial begin
  // Warm passing and rejecting streams retain a one-sample-per-clock cadence
  // while their downstream path is available, for both primitive kinds.
  for(mode=0;mode<4;mode=mode+1)begin
   restart();while(accepted<128)begin drive(accepted,mode[0],mode[1]);tick();end
   drain();if(accepted!=128||max_accept!=128||max_retire!=128)$fatal(1,"lost warm II1 bits%0d mode%0d accepts%0d commits%0d",COORD_BITS,mode,max_accept,max_retire);
   cases=cases+1;
  end
  for(mode=0;mode<2;mode=mode+1)begin
   restart();
   // Force a consistent closed triangle pipeline (advance, not merely its
   // in_ready output), or the sprite's coordinate handoff space.
   if(mode==0)force dut.perspective.advance=1'b0;
   else force dut.read_space=1'b0;
   drive(0,mode[0],0);tick();
   if(accepted!=1||!dut.depth_valid||in_ready)$fatal(1,"closed passing stage advertised acceptance");
   held_depth={dut.depth_sprite,dut.depth_slot,dut.depth_value,dut.depth_u,dut.depth_v,dut.depth_q,dut.depth_x,dut.depth_y};
   drive(1,mode[0],0);
   for(j=0;j<5;j=j+1)begin
    // No clock occurs while probing the opposite compare result. Readiness
    // must remain isolated from that result under the same downstream state.
    sampled_ready=in_ready;stored_depth=0;#1;
    if(in_ready!==sampled_ready||in_ready)$fatal(1,"depth comparator propagated into blocked input readiness bits%0d kind%0d",COORD_BITS,mode);
    stored_depth=500;#1;ready_isolation=ready_isolation+1;tick();
    if(accepted!=1||{dut.depth_sprite,dut.depth_slot,dut.depth_value,dut.depth_u,dut.depth_v,dut.depth_q,dut.depth_x,dut.depth_y}!==held_depth)
     $fatal(1,"blocked depth payload overwritten bits%0d",COORD_BITS);
    blocked_holds=blocked_holds+1;
   end
   in_valid=0;
   if(mode==0)release dut.perspective.advance;else release dut.read_space;
   drain();cases=cases+1;
   restart();
   if(mode==0)force dut.perspective.advance=1'b0;else force dut.read_space=1'b0;
   drive(0,mode[0],1);tick();drive(1,mode[0],1);#1;
   if(in_ready||!dut.depth_consumed)$fatal(1,"blocked reject did not obey conservative acceptance contract bits%0d",COORD_BITS);
   tick();if(accepted!=1)$fatal(1,"closed reject accepted younger sample on same edge");
   in_valid=0;if(mode==0)release dut.perspective.advance;else release dut.read_space;
   drain();if(requested!=0||committed!=1)$fatal(1,"depth reject generated texture or duplicate retirement");cases=cases+1;
  end
  // Hold the oldest passing sample in the texture responder while younger
  // depth rejects become ready. They must not retire ahead of that sample.
  restart();key_enable=1;
  while(accepted<128)begin
   response_gate=cycles>=81&&(cycles%5!=0);tex_req_ready=cycles<81||(cycles%7!=0);
   drive(accepted,accepted[0],accepted%3==1);
   tick();if(cycles<=81)begin if(committed!=0)$fatal(1,"younger reject retired before blocked oldest sample");mixed_order_checks=mixed_order_checks+1;end
  end
  drain();cases=cases+1;
  if(ready_implications<200||ready_isolation!=10||blocked_holds!=10||mixed_order_checks!=80)
   $fatal(1,"missing conservative-ready coverage bits%0d checks%0d/%0d/%0d/%0d",COORD_BITS,ready_implications,ready_isolation,blocked_holds,mixed_order_checks);
  $display("PASS conservative ready bits%0d:%0d cases, warm pass/reject II1, blocked hold/compare isolation, ordered mixed rejects; checks%0d/%0d/%0d/%0d",COORD_BITS,cases,ready_implications,ready_isolation,blocked_holds,mixed_order_checks);
  done=1;
 end
endmodule
module nano_fragment_conservative_ready_tb;
 wire full_done,tile_done;
 nano_fragment_conservative_ready_case #(.COORD_BITS(32))full_case(full_done);
 nano_fragment_conservative_ready_case #(.COORD_BITS(16))tile_case(tile_done);
 initial begin wait(full_done&&tile_done);$finish;end
endmodule
