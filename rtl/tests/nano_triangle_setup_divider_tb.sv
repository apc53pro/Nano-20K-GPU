`timescale 1ns/1ps
// Exercise setup through captured kind-4 records and its public word stream.
// Signed64 division/multiplication supplies an independent arithmetic oracle;
// no internal divider state or implementation-specific preload is inspected.
module nano_triangle_setup_divider_tb;
 reg clk=0,reset=0,capture_we=0,start=0;
 reg [6:0]capture_index=0;reg [31:0]capture_data=0;
 reg [9:0]tile_x=0;reg [8:0]tile_y=0;
 wire vertex_kind,word_we,done,draw,bad;wire [6:0]word_index;wire [31:0]word_data;
 nano_triangle_setup dut(.*);
 reg [31:0]record[0:15],output_words[0:43];
 reg [43:0]seen=0;
 reg signed [63:0]px[0:2],py[0:2],pz[0:2],ea[0:2],eb[0:2],ec[0:2];
 reg signed [63:0]area,nx,ny,gx,gy,seed,sx,sy,wx,wy;
 integer i,e,next_vertex,steps,elapsed,case_id=0,cases=0,reset_cases=0,windings=0;
 integer below_latency,at_latency,zero_latency,reset_offset;
 always @(posedge clk)if(!reset&&word_we)begin
  if(word_index>43||word_index>=20&&word_index<26)
   $fatal(1,"unexpected setup output word%0d",word_index);
  if(seen[word_index])$fatal(1,"duplicate setup output word%0d",word_index);
  seen[word_index]=1;output_words[word_index]=word_data;
 end
 task tick;begin #5;clk=1;#5;clk=0;end endtask
 task reset_control;begin
  start=0;capture_we=0;reset=1;tick();reset=0;seen=0;
  repeat(4)begin tick();if(word_we||done)$fatal(1,"reset left stale output actions");end
 end endtask
 task write_word(input integer index);begin
  capture_index=index;capture_data=record[index];capture_we=1;tick();capture_we=0;
 end endtask
 task load_record;begin
  // Deliberately nonsequential loading exercises every public capture slot.
  for(i=15;i>=0;i=i-1)write_word(i);
  if(!vertex_kind)$fatal(1,"public kind-4 capture failed");
 end endtask
 task make_record(input integer variant,input integer reverse_order);
  reg [31:0]swap_word;
  begin
   for(i=0;i<16;i=i+1)record[i]=0;
   record[0]=32'h00070204;record[2]=24'h57a123;
   tile_x=0;tile_y=0;
   // A one-subpixel right triangle has area 1, NX=Z1-Z0, NY=Z2-Z0.
   // Its vertex at the sample center makes even saturated cases observable
   // through actual record processing rather than a forced divider fixture.
   record[3]=(8<<16)|8;record[4]=(8<<16)|9;record[5]=(9<<16)|8;
   record[9]=(1<<16);record[10]=(1<<16);
   case(variant)
    0:begin record[6]=32'h12345678;record[7]=record[6];record[8]=record[6];end
    1:begin record[6]=17;record[7]=17;record[8]=31;end
    2:begin record[6]=17;record[7]=31;record[8]=17;end
    3:begin record[6]=31;record[7]=17;record[8]=31;end
    4:begin record[6]=31;record[7]=31;record[8]=17;end
    // Strict preload threshold below / equal / above, with Y exactly zero.
    5:record[7]=32'd4194303;
    6:record[7]=32'd4194304;
    7:record[7]=32'd4194305;
    8:begin record[6]=32'd4194304;record[8]=record[6];end
    // Gradient clamp boundary and both signs well beyond it.
    9:record[7]=32'h3fffffff;
    10:record[7]=32'h40000000;
    11:begin record[7]=32'hffffffff;record[8]=32'hffffffff;end
    12:record[6]=32'hffffffff;
    // area8192: NX/area*4096 is exactly (Z1-Z0)/2. Check half-away
    // rounding, below/above half, and negative signed gradient application.
    13,14,15,16:begin
     record[4]=(8<<16)|8200;
     record[6]=variant==16?3:0;
     record[7]=variant==16?2:variant-12;
     record[8]=record[6];
    end
    default:begin
     // Nonzero clipped sample offsets and fractional gradients exercise the
     // final signed seed floor, as well as high unsigned depth operands.
     tile_x=32;tile_y=32;
     record[3]=0;record[4]=10240;record[5]=(7680<<16);
     record[6]=32'hfffff123;record[7]=32'h00012345;record[8]=32'h800001ff;
     record[9]=(41<<16)|40;record[10]=(44<<16)|43;
    end
   endcase
   if(reverse_order)begin
    swap_word=record[4];record[4]=record[5];record[5]=swap_word;
    swap_word=record[7];record[7]=record[8];record[8]=swap_word;
   end
  end
 endtask
 function automatic signed [63:0] gradient(input signed [63:0]n,input signed [63:0]denominator);
  reg signed [63:0]nabs,dabs,q;
  begin
   nabs=n<0?-n:n;dabs=denominator<0?-denominator:denominator;
   // floor(abs(N)*4096/abs(area)+1/2), then the public signed42 clamp.
   q=(nabs*64'sd8192+dabs)/(dabs*64'sd2);
   if(q>64'sd4398046511103)q=64'sd4398046511103;
   gradient=(n<0)!=(denominator<0)?-q:q;
  end
 endfunction
 task compute_expected;begin
  for(e=0;e<3;e=e+1)begin
   px[e]=record[3+e]&32'h3fff;py[e]=(record[3+e]>>16)&32'h1fff;
   pz[e]={32'd0,record[6+e]};
  end
  for(e=0;e<3;e=e+1)begin
   next_vertex=(e+1)%3;
   ea[e]=py[e]-py[next_vertex];eb[e]=px[next_vertex]-px[e];
   ec[e]=-ea[e]*px[e]-eb[e]*py[e];
  end
  area=ea[0]*px[2]+eb[0]*py[2]+ec[0];
  if(area==0)$fatal(1,"test generated a degenerate record");
  nx=pz[0]*ea[1]+pz[1]*ea[2]+pz[2]*ea[0];
  ny=pz[0]*eb[1]+pz[1]*eb[2]+pz[2]*eb[0];
  gx=gradient(nx,area);gy=gradient(ny,area);
  wx=record[9]&32'h7ff;if(wx<tile_x)wx=tile_x;
  wy=record[10]&32'h7ff;if(wy<tile_y)wy=tile_y;
  sx=wx*16+8-px[0];sy=wy*16+8-py[0];
  seed=(pz[0]*256)+((sx*gx+sy*gy+64'sd8)>>>4);
  if(area<0)for(e=0;e<3;e=e+1)begin ea[e]=-ea[e];eb[e]=-eb[e];ec[e]=-ec[e];end
 end endtask
 task launch;begin seen=0;start=1;tick();start=0;end endtask
 task finish_and_compare;begin
  elapsed=0;while(!done&&elapsed<200)begin tick();elapsed=elapsed+1;end
  if(!done||!draw||bad)$fatal(1,"case%0d setup rejected/hung after%0d clocks",case_id,elapsed);
  for(e=0;e<44;e=e+1)if(e<20||e>=26)begin
   if(!seen[e])$fatal(1,"case%0d missing public output word%0d",case_id,e);
  end
  if({output_words[41],output_words[40]}!==gx||{output_words[43],output_words[42]}!==gy||
     {output_words[39],output_words[38]}!==seed)
   $fatal(1,"case%0d gradients/seed mismatch gx%h/%h gy%h/%h seed%h/%h",case_id,
    {output_words[41],output_words[40]},gx,{output_words[43],output_words[42]},gy,
    {output_words[39],output_words[38]},seed);
  for(e=0;e<3;e=e+1)begin
   if(output_words[26+4*e]!==ea[e][31:0]||output_words[27+4*e]!==eb[e][31:0]||
      output_words[28+4*e]!==ec[e][31:0])$fatal(1,"case%0d edge%0d changed during divider work",case_id,e);
  end
  if(output_words[0]!==32'h00070200||output_words[2]!==record[2]||
     output_words[8]!==wx[31:0]||output_words[9]!==wy[31:0])
   $fatal(1,"case%0d header/window output mismatch",case_id);
  cases=cases+1;case_id=case_id+1;tick();
 end endtask
 initial begin
  reset_control();
  for(steps=0;steps<18;steps=steps+1)begin
   make_record(steps,0);load_record();compute_expected();launch();finish_and_compare();
   if(steps==0)zero_latency=elapsed;
   if(steps==5)below_latency=elapsed;
   if(steps==6)at_latency=elapsed;
  end
  // Same ordinary setup/word stream on both sides of a strict threshold:
  // only the obtainable depth numerator changes. A missing fast path would
  // have equal deadlines; truncating the comparison fails exact gradients.
  if(at_latency<below_latency+8||zero_latency>below_latency)
   $fatal(1,"ordinary setup did not save at least eight clocks below threshold: zero%0d below%0d at%0d",zero_latency,below_latency,at_latency);
  for(steps=0;steps<18;steps=steps+1)begin
   make_record(steps,1);load_record();compute_expected();launch();finish_and_compare();windings=windings+1;
  end
  for(reset_offset=0;reset_offset<4;reset_offset=reset_offset+1)begin
   make_record(11,0);load_record();launch();
   repeat(20+reset_offset*25)tick();
   if(done)$fatal(1,"reset test unexpectedly completed before interruption");
   reset_control();
   // Only recapture kind/version after reset; all other record fields remain
   // resident. Immediate reuse must not inherit stale quotient or actions.
   write_word(0);compute_expected();launch();finish_and_compare();reset_cases=reset_cases+1;
  end
  $display("PASS triangle divider: records%0d reverse%0d resets%0d zero_edges%0d below_edges%0d threshold_edges%0d",cases,windings,reset_cases,zero_latency,below_latency,at_latency);
  $finish;
 end
endmodule
