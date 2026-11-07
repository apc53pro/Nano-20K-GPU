`timescale 1ns/1ps
// Functional primitive models verify divider ratios and wrapper sequencing,
// not PLL analog behavior, pad delay or silicon timing.
module rPLL #(parameter FCLKIN="27",IDIV_SEL=2,FBDIV_SEL=27,ODIV_SEL=2,
 DYN_SDIV_SEL=2,DEVICE="GW2AR-18C")(
 input CLKIN,CLKFB,RESET,RESET_P,input [5:0] FBDSEL,IDSEL,ODSEL,
 input [3:0] PSDA,DUTYDA,FDLY,
 output reg CLKOUT=0,output reg CLKOUTD=0,output CLKOUTP,CLKOUTD3,output reg LOCK=0);
 always #1.984 CLKOUT=~CLKOUT;
 integer settling=0;
 always @(posedge CLKIN) if(RESET) begin settling<=0;LOCK<=0;end
 else if(settling==4) LOCK<=1;else settling<=settling+1;
 always @(posedge CLKOUT or posedge RESET) if(RESET) CLKOUTD<=0;else CLKOUTD<=~CLKOUTD;
 assign CLKOUTP=0;assign CLKOUTD3=0;
endmodule
module CLKDIV #(parameter DIV_MODE="5",GSREN="false")(
 input HCLKIN,RESETN,CALIB,output reg CLKOUT=0);
 integer phase=0;
 always @(posedge HCLKIN or negedge HCLKIN or negedge RESETN)
 if(!RESETN) begin phase<=0;CLKOUT<=0;end
 else if(phase==9) begin phase<=0;CLKOUT<=0;end
 else begin phase<=phase+1;if(phase==4) CLKOUT<=1;end
endmodule
module ODDR(input CLK,D0,D1,TX,output reg Q0=0,output Q1);
 always @(posedge CLK or negedge CLK) Q0<=CLK ? D0 : D1;
 assign Q1=TX;
endmodule
module tang20k_board_tb;
 reg clk27=0;always #18.5185 clk27=~clk27;
 reg board_reset=0,gpu_hclk=0;always #20 gpu_hclk=~gpu_hclk;
 reg gpu_reset_n=0,gpu_abort=0,gpu_ale_n=1,gpu_wr_n=1,gpu_rd_n=1;
 tri [7:0] gpu_d;reg cpu_drive=0;reg [7:0] cpu_data=0;
 assign gpu_d=cpu_drive ? cpu_data : 8'bz;
 wire gpu_wready,gpu_rvalid;
 wire [3:0] hdmi_p,hdmi_n;wire O_sdram_clk,O_sdram_cke,O_sdram_cs_n,
 O_sdram_cas_n,O_sdram_ras_n,O_sdram_wen_n;
 tri [31:0] IO_sdram_dq;wire [10:0] O_sdram_addr;wire [1:0] O_sdram_ba;wire [3:0] O_sdram_dqm;
 tang20k_top dut(.*);
 integer checks=0,cores=0,serials=0;
 real last_core=0;
 reg [10:0] expected_addr;
 always @(posedge dut.core_clk) begin
  cores=cores+1;last_core=$realtime;expected_addr=dut.sd_addr;
  #0.1;
  if(O_sdram_addr!==expected_addr) $fatal(1,"SDRAM output register not capturing controller word");
  checks=checks+1;
 end
 always @(posedge dut.serial_clk) serials=serials+1;
 always @(posedge O_sdram_clk) if(!dut.reset) begin
  if($realtime-last_core<9 || $realtime-last_core>11) $fatal(1,"SDRAM forwarded phase mismatch");
  checks=checks+1;
 end
 real last_rgb_change=0;
 integer pixel_samples=0;
 reg [15:0] previous_rgb=0;
 always @(dut.rgb) if(!dut.reset) last_rgb_change=$realtime;
 always @(posedge dut.pixel_clk) begin
  if(dut.reset || dut.pixel_reset_pipe[1]) pixel_samples=0;
  else begin
   if(pixel_samples>5) begin
    if($realtime-last_rgb_change<17) $fatal(1,"pixel payload lacks safe capture interval");
    if(dut.rgb!==previous_rgb+16'd1) $fatal(1,"physical pixel handoff lost or duplicated a pixel");
    checks=checks+2;
   end
   previous_rgb=dut.rgb;pixel_samples=pixel_samples+1;
  end
 end
 task address_write(input [6:0] a);
 begin
  @(negedge gpu_hclk);gpu_ale_n=0;gpu_wr_n=0;cpu_drive=1;cpu_data={1'b0,a};
  #1;if(!gpu_wready) $fatal(1,"address latch blocked by native bank ready");
  @(posedge gpu_hclk);#1;
  if(dut.address!==a || dut.core.writes!=0) $fatal(1,"address byte reached packet path");
  @(negedge gpu_hclk);gpu_wr_n=1;gpu_ale_n=1;cpu_drive=0;#1;
  checks=checks+1;
 end endtask
 integer before_core,before_serial,release_phase;
 initial begin
  wait(dut.locked);repeat(4) @(posedge dut.core_clk);
  if(gpu_wready || gpu_rvalid || gpu_d!==8'bz) $fatal(1,"bus active during reset");
  gpu_reset_n=1;repeat(5) @(posedge gpu_hclk);#1;
  @(posedge dut.pixel_clk);#0.2;before_core=cores;before_serial=serials;
  repeat(20) @(posedge dut.pixel_clk);#0.2;
  if(cores-before_core!=40 || serials-before_serial!=100) $fatal(1,"clock ratios incorrect");
  address_write(37);
  if(gpu_wready) $fatal(1,"nonzero bank incorrectly accepts packet writes");
  @(negedge gpu_hclk);gpu_rd_n=0;
  @(posedge gpu_hclk);#1;
  if(!gpu_rvalid || gpu_d!==(8'ha0^8'd37)) $fatal(1,"registered read not visible on shared bus");
  @(negedge gpu_hclk);gpu_rd_n=1;#1;
  if(gpu_rvalid || gpu_d!==8'bz) $fatal(1,"read bus not released");
  address_write(0);
  @(negedge gpu_hclk);gpu_wr_n=0;cpu_drive=1;cpu_data=8'h9d;
  @(posedge gpu_hclk);#1;
  if(dut.core.writes!=1 || dut.core.last_byte!=8'h9d) $fatal(1,"packet byte not forwarded");
  @(negedge gpu_hclk);gpu_rd_n=0;cpu_data=8'h35;#1;
  if(gpu_d!==8'h35 || gpu_rvalid) $fatal(1,"FPGA drove bus during simultaneous strobes");
  @(negedge gpu_hclk);gpu_wr_n=1;gpu_rd_n=1;cpu_drive=0;
  gpu_reset_n=0;#1;
  if(gpu_wready || gpu_rvalid || gpu_d!==8'bz) $fatal(1,"asynchronous reset did not isolate bus");
  gpu_reset_n=1;repeat(5) @(posedge gpu_hclk);#1;
  if(dut.address!=0) $fatal(1,"address did not reset to command bank");
  for(release_phase=0;release_phase<8;release_phase=release_phase+1) begin
   gpu_reset_n=0;#(23+release_phase*3);gpu_reset_n=1;
   repeat(15) @(posedge dut.pixel_clk);
  end
  $display("PASS board wrapper: %0d clock/SDRAM pad/bus checks",checks);$finish;
 end
 initial begin #100000;$fatal(1,"board wrapper timeout");end
endmodule
