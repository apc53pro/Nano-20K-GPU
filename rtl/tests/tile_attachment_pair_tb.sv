`timescale 1ns/1ps
// Exercise the actual renderer attachment ports across the complete tile.
// Expected pixels are maintained independently of native word/bank layout.
module nano_tile_attachment_pair_tb;
 reg clk=0,reset=0,cmd_word_we=0,cmd_valid=0,pix_re=0,tex_flush=0;
 reg [1:0]pix_we=0;reg pix_write_pair=0;
 reg [6:0]cmd_word_index=0;reg [31:0]cmd_word_data=0;
 reg [9:0]cmd_tile_x=0,pix_addr=0,pix_raddr=0;reg [8:0]cmd_tile_y=0;
 reg [31:0]pix_color=0;reg [23:0]pix_depth=0;
 wire cmd_ready,busy,done,fault,pix_rvalid;wire [31:0]pix_rcolor;wire [23:0]pix_rdepth;
 wire tex_cfg_valid;wire [22:0]tex_cfg_base,tex_cfg_pitch;wire [10:0]tex_cfg_width,tex_cfg_height;
 wire tex_cfg_repeat_u,tex_cfg_repeat_v,tex_req_valid;
 wire signed [31:0]tex_req_x,tex_req_y;wire [5:0]tex_req_tag;
 wire tex_cfg_ready=1,tex_req_ready=1,tex_rsp_valid=0,tex_fault=0;
 wire [15:0]tex_rsp_texel=0;wire [5:0]tex_rsp_tag=0;wire tex_rsp_ready;
 nano_tile_renderer dut(.*);
 reg [15:0]expected_color[0:1023];reg [23:0]expected_depth[0:1023];
 reg [31:0]held_color;reg [23:0]held_depth;
 integer i,pair_index,read_checks=0,held_edges=0,clearback_pairs=0;
 localparam [31:0]CLEAR_COLOR=32'h24681357;
 localparam [23:0]CLEAR_DEPTH=24'ha57931;
 task tick;begin #5;clk=1;#5;clk=0;end endtask
 task check_read(input integer pixel_index);begin
  if(!pix_rvalid||pix_rcolor!=={expected_color[(pixel_index&1022)+1],expected_color[pixel_index&1022]}||
     pix_rdepth!==expected_depth[pixel_index])
   $fatal(1,"paired attachment pixel%0d color%h expected%h/%h depth%h expected%h",pixel_index,pix_rcolor,
    expected_color[(pixel_index&1022)+1],expected_color[pixel_index&1022],pix_rdepth,expected_depth[pixel_index]);
  read_checks=read_checks+1;
 end endtask
 task inspect_tile;begin
  pix_we=0;pix_write_pair=0;pix_re=1;
  for(i=0;i<1024;i=i+1)begin pix_raddr=i;tick();check_read(i);end
  pix_re=0;tick();
 end endtask
 initial begin
  reset=1;tick();reset=0;tick();
  if(!cmd_ready||busy||fault)$fatal(1,"attachment fixture not idle after reset");
  // Every pair, including row boundaries and the tile's last word. An odd
  // address selects the same physical pair; the caller normally uses even.
  for(pair_index=0;pair_index<512;pair_index=pair_index+1)begin
   expected_color[2*pair_index]=16'h1300^pair_index;
   expected_color[2*pair_index+1]=16'ha700^(pair_index*3);
   expected_depth[2*pair_index]=24'h678900^pair_index;
   expected_depth[2*pair_index+1]=expected_depth[2*pair_index];
   pix_we=3;pix_write_pair=1;pix_addr=2*pair_index+(pair_index&1);
   pix_color={expected_color[2*pair_index+1],expected_color[2*pair_index]};
   pix_depth=expected_depth[2*pair_index];tick();
  end
  inspect_tile();
  // Single-color writes take the low half for either address parity and
  // preserve the neighboring RGB565 pixel and both depth values.
  pix_write_pair=0;pix_we=1;
  for(i=0;i<1024;i=i+1)begin
   expected_color[i]=16'h5a00^(i*17);
   pix_addr=i;pix_color={16'hdead,expected_color[i]};pix_depth=24'hbadbad;tick();
  end
  // Depth-only writes exercise both native parity banks without touching
  // the paired color word; each pixel receives a distinct unsigned D24.
  pix_we=2;
  for(i=0;i<1024;i=i+1)begin
   expected_depth[i]=24'h800000^(i*7919);
   pix_addr=i;pix_color=32'hdeadbeef;pix_depth=expected_depth[i];tick();
  end
  inspect_tile();
  // An accepted odd read must hold its depth lane when the live address
  // changes while no read is accepted.
  pix_re=1;pix_raddr=1023;tick();check_read(1023);
  held_color=pix_rcolor;held_depth=pix_rdepth;pix_re=0;
  repeat(16)begin
   pix_raddr=~pix_raddr;tick();held_edges=held_edges+1;
   if(pix_rvalid||pix_rcolor!==held_color||pix_rdepth!==held_depth)
    $fatal(1,"unaccepted address changed held attachment output");
  end
  reset=1;tick();reset=0;tick();
  if(!cmd_ready||busy||fault)$fatal(1,"reset did not restore attachment access");
  // Model the real store pipeline: read a pair, then clear that pair while
  // reading the next word. Distinct physical addresses avoid undefined
  // cross-port read/write collisions, including across row boundaries.
  pix_re=1;pix_raddr=0;tick();check_read(0);
  for(pair_index=1;pair_index<512;pair_index=pair_index+1)begin
   pix_we=3;pix_write_pair=1;pix_addr=2*(pair_index-1);
   pix_color=CLEAR_COLOR;pix_depth=CLEAR_DEPTH;
   pix_raddr=2*pair_index+(pair_index&1);tick();check_read(2*pair_index+(pair_index&1));
   expected_color[2*(pair_index-1)]=CLEAR_COLOR[15:0];
   expected_color[2*(pair_index-1)+1]=CLEAR_COLOR[31:16];
   expected_depth[2*(pair_index-1)]=CLEAR_DEPTH;expected_depth[2*(pair_index-1)+1]=CLEAR_DEPTH;
   clearback_pairs=clearback_pairs+1;
  end
  pix_re=0;pix_we=3;pix_write_pair=1;pix_addr=1022;pix_color=CLEAR_COLOR;pix_depth=CLEAR_DEPTH;tick();
  expected_color[1022]=CLEAR_COLOR[15:0];expected_color[1023]=CLEAR_COLOR[31:16];
  expected_depth[1022]=CLEAR_DEPTH;expected_depth[1023]=CLEAR_DEPTH;clearback_pairs=clearback_pairs+1;
  inspect_tile();
  if(fault||busy||!cmd_ready||clearback_pairs!=512)$fatal(1,"attachment stream failed to remain idle");
  $display("PASS paired attachments: pairs512 single_color1024 single_depth1024 reads%0d clearback%0d held%0d",read_checks,clearback_pairs,held_edges);
  $finish;
 end
endmodule

// A partial write still collides with a read of its native word. The runner
// requires the primitive's collision diagnostic, rather than this sentinel.
module nano_sdp32_masked_collision_tb;
 reg clk=0,we=0,re=0;wire [31:0]rdata;
 nano_sdp32_masked dut(.wclk(clk),.wce(1'b1),.we(we),.wstrb(4'b0001),
  .waddr(9'd511),.wdata(32'h11223344),.rclk(clk),.rce(1'b1),.re(re),
  .raddr(9'd511),.reset_output(1'b0),.rdata(rdata));
 initial begin
  #5;clk=1;#5;clk=0;we=1;re=1;#5;clk=1;#5;
  $fatal(1,"masked RAM collision was not rejected");
 end
endmodule
