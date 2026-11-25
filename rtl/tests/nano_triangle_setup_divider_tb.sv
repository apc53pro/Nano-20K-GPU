`timescale 1ns/1ps
// Exercise setup through captured kind-4 records and its public word stream.
// Signed64 arithmetic supplies an independent public output oracle. A separate
// internal transition check counts pair clocks and verifies the terminal bit.
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
 integer pair_clocks=0,pair_resets=0,tail_resets=0,traced_divisions=0;
 reg [3:0] digit_coverage=0,terminal_coverage=0;
 integer min_pair_clocks=22,max_pair_clocks=0;
 // Per-edge division uses mathematical quotient and remainder. A saturated
 // result is already outside signed42; only its schedule and public clamp
 // are relevant to the output.
 always @(posedge clk) begin : check_divide_cadence
  reg [5:0] position;
  reg [42:0] old_quotient,want_quotient;
  reg [41:0] old_numerator,want_numerator;
  reg [28:0] old_remainder;
  reg [31:0] brought_down,want_remainder,want_digit;
  if(reset)pair_clocks=0;
  else if(dut.div_busy)begin
   position=dut.div_count;
   if(position>=1&&position<=43)begin
    old_quotient=dut.quotient;old_numerator=dut.numerator;old_remainder=dut.remainder;
    if(position==43)begin
     brought_down=2*{3'd0,dut.remainder}+{31'd0,old_numerator[41]};
     want_numerator=old_numerator;
    end else begin
     brought_down=4*{3'd0,dut.remainder}+{30'd0,old_numerator[41:40]};
     want_numerator=old_numerator<<2;
     pair_clocks=pair_clocks+1;
    end
    if(!dut.saturate)begin
     want_digit=brought_down/{3'd0,dut.area};
     want_remainder=brought_down%{3'd0,dut.area};
     if(position==43)begin
      if(want_digit>1)$fatal(1,"terminal quotient digit exceeded one bit");
      want_quotient={old_quotient[41:0],want_digit[0]};
      want_remainder={3'd0,old_remainder};
      terminal_coverage[{dut.step>=36?dut.sign_y:dut.sign_x,want_digit[0]}]=1;
     end else begin
      if(want_digit>3)$fatal(1,"paired quotient digit exceeded two bits");
      want_quotient={old_quotient[40:0],want_digit[1:0]};
      digit_coverage[want_digit[1:0]]=1;
     end
     #1;
     if(dut.quotient!==want_quotient||dut.remainder!==want_remainder[28:0])
      $fatal(1,"case%0d exact quotient or remainder changed at bit%0d",case_id,position);
    end else #1;
    if(dut.div_count!==position+(position==43?6'd1:6'd2)||dut.numerator!==want_numerator)
     $fatal(1,"case%0d unexpected divider cadence at bit%0d",case_id,position);
   end else if(position==44)begin
    if(pair_clocks!=17&&pair_clocks!=21)
     $fatal(1,"case%0d completed with %0d paired clocks",case_id,pair_clocks);
    if(pair_clocks<min_pair_clocks)min_pair_clocks=pair_clocks;
    if(pair_clocks>max_pair_clocks)max_pair_clocks=pair_clocks;
    pair_clocks=0;traced_divisions=traced_divisions+1;
   end
  end
 end
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
    18,19,20,21:begin
     // Denominators 8191 and 8193 straddle exact half. Exercise just
     // above and below the rounding threshold in either result sign.
     record[4]=(8<<16)|(variant[0]?8201:8199);
     record[6]=variant>=20;
     record[7]=variant<20;
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
  for(steps=0;steps<22;steps=steps+1)begin
   make_record(steps,0);load_record();compute_expected();launch();finish_and_compare();
   if(steps==0)zero_latency=elapsed;
   if(steps==5)below_latency=elapsed;
   if(steps==6)at_latency=elapsed;
  end
  // Same ordinary setup/word stream on both sides of a strict threshold:
  // only the obtainable depth numerator changes. A missing fast path would
  // have equal deadlines; truncating the comparison fails exact gradients.
  if(at_latency!=below_latency+5||zero_latency>below_latency)
   $fatal(1,"radix-four high prefix condition did not save five clocks: zero%0d below%0d at%0d",zero_latency,below_latency,at_latency);
  for(steps=0;steps<22;steps=steps+1)begin
   make_record(steps,1);load_record();compute_expected();launch();finish_and_compare();windings=windings+1;
  end
  for(reset_offset=0;reset_offset<5;reset_offset=reset_offset+1)begin
   make_record(11,0);load_record();launch();
   if(reset_offset==0)repeat(20)tick();
   else begin
    elapsed=0;
    // Reset before either a nonzero paired step or the final rounding bit
    // on both gradients, independent of the surrounding setup latency.
    while(!done&&elapsed<110&&!(dut.div_busy&&
          dut.step==(reset_offset<=2?6'd32:6'd37)&&
          dut.div_count==(reset_offset[0]?6'd11:6'd43)))begin
     tick();elapsed=elapsed+1;
    end
    if(elapsed==110)$fatal(1,"reset test never reached divider checkpoint%0d",reset_offset);
    if(reset_offset[0])pair_resets=pair_resets+1;
    else tail_resets=tail_resets+1;
   end
   if(done)$fatal(1,"reset test unexpectedly completed before interruption");
   reset_control();
   // Only recapture kind/version after reset; all other record fields remain
   // resident. Immediate reuse must not inherit stale quotient or actions.
   write_word(0);compute_expected();launch();finish_and_compare();reset_cases=reset_cases+1;
  end
  if(digit_coverage!==4'hf||terminal_coverage!==4'hf||pair_resets!=2||tail_resets!=2||
     min_pair_clocks!=17||max_pair_clocks!=21)
   $fatal(1,"missing radix-four coverage digits=%h terminal=%h pairs=%0d..%0d",
       digit_coverage,terminal_coverage,min_pair_clocks,max_pair_clocks);
  $display("PASS triangle divider: records%0d reverse%0d resets%0d zero_edges%0d below_edges%0d threshold_edges%0d",cases,windings,reset_cases,zero_latency,below_latency,at_latency);
  $display("PASS radix-four cadence: divisions%0d digits%h terminal%h min_pairs%0d max_pairs%0d pair_resets%0d tail_resets%0d",
      traced_divisions,digit_coverage,terminal_coverage,min_pair_clocks,max_pair_clocks,pair_resets,tail_resets);
  $finish;
 end
endmodule
