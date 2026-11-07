`timescale 1ns/1ps
// Compare the compact header against full raw 32-bit validation, without
// reconstructing expected fields from the compact representation. Mutations
// cover every record word, arbitrary write order and clearing old sentinels.
module nano_tile_record_compact_tb;
 reg clk=0,reset=0,cmd_word_we=0,cmd_valid=0;
 reg [6:0]cmd_word_index=0;reg[31:0]cmd_word_data=0;
 nano_tile_renderer dut(.clk(clk),.reset(reset),.cmd_word_we(cmd_word_we),
  .cmd_word_index(cmd_word_index),.cmd_word_data(cmd_word_data),.cmd_valid(cmd_valid),
  .cmd_tile_x(10'd0),.cmd_tile_y(9'd0),.pix_we(2'b00),.pix_addr(10'd0),
  .pix_color(16'd0),.pix_depth(24'd0),.pix_re(1'b0),.pix_raddr(10'd0),
  .tex_flush(1'b0),.tex_cfg_ready(1'b1),.tex_req_ready(1'b1),.tex_rsp_valid(1'b0),
  .tex_rsp_texel(16'd0),.tex_rsp_tag(6'd0),.tex_fault(1'b0));
 reg [31:0] raw[0:79],base[0:79];
 reg [63:0] rng=64'hb52d89a364017cef;
 reg [31:0] sample;
 integer variant,kind,w,n,i,checkpoint,checked=0,setup_checked=0;

 function automatic signed [31:0] minimum;
  input signed [31:0] a,b;
  begin minimum=a<b?a:b;end
 endfunction
 function automatic signed [31:0] maximum;
  input signed [31:0] a,b;
  begin maximum=a>b?a:b;end
 endfunction
 function automatic raw_bad;
  reg bad,need_texture;
  reg signed[32:0] width,height;
  reg signed[33:0] end_x,end_y;
  integer j;
  begin
   need_texture=raw[0][7:0]==3||(raw[0][7:0]==0&&raw[1][7:0]>=2&&(raw[0][16]||raw[0][19]));
   width=$signed({raw[10][31],raw[10]})-$signed({raw[8][31],raw[8]});
   height=$signed({raw[11][31],raw[11]})-$signed({raw[9][31],raw[9]});
   end_x=$signed({2'd0,raw[17]})+$signed({width[32],width});
   end_y=$signed({2'd0,raw[18]})+$signed({height[32],height});
   bad=raw[0][15:8]!=1||raw[0][7:0]>3||raw[0][31:20]!=0||
    raw[1][7:0]>4||raw[1][15:8]>2||raw[1][31:19]!=0||
    raw[2][31:24]!=0||raw[3][31:16]!=0||raw[16][31:24]!=0||
    raw[19]!=0||(!raw[0][19]&&raw[3]!=0)||(need_texture&&!raw[1][18]);
   if(raw[1][18])bad=bad||raw[12][31:23]!=0||raw[15][31:23]!=0||
    raw[13][31:11]!=0||raw[14][31:11]!=0;
   else bad=bad||raw[12]!=0||raw[13]!=0||raw[14]!=0||raw[15]!=0;
   case(raw[0][7:0])
   0:bad=bad||raw[16]!=0||raw[17]!=0||raw[18]!=0||raw[29]>1||raw[33]>1||raw[37]>1||
    raw[26][31:17]!={15{raw[26][17]}}||raw[27][31:17]!={15{raw[27][17]}}||
    raw[30][31:17]!={15{raw[30][17]}}||raw[31][31:17]!={15{raw[31][17]}}||
    raw[34][31:17]!={15{raw[34][17]}}||raw[35][31:17]!={15{raw[35][17]}}||
    $signed(raw[8])<0||$signed(raw[9])<0||raw[10]>640||raw[11]>480;
   1:begin
    bad=bad||raw[17]!=0||raw[18]!=0||raw[1][7:0]!=0||raw[0][19];
    for(j=20;j<80;j=j+1)bad=bad||raw[j]!=0;
   end
   2:begin
    bad=bad||raw[8]!=0||raw[9]!=0||raw[10]!=0||raw[11]!=0||raw[1][7:0]!=0||raw[0][19]||
     raw[17]>=640||raw[20]>=640||raw[18]>=480||raw[21]>=480;
    for(j=22;j<80;j=j+1)bad=bad||raw[j]!=0;
   end
   3:begin
    bad=bad||(raw[1][7:0]!=0&&raw[1][7:0]!=2)||raw[17][31]||raw[18][31];
    if(width>0&&height>0)bad=bad||end_x>$signed({2'd0,raw[13]})||end_y>$signed({2'd0,raw[14]});
    for(j=20;j<80;j=j+1)bad=bad||raw[j]!=0;
   end
   default:begin end
   endcase
   raw_bad=bad;
  end
 endfunction
 task tick;begin #1;clk=1;#1;clk=0;end endtask
 task write_word;
  input integer index;
  input [31:0] value;
  begin
   raw[index]=value;cmd_word_index=index;cmd_word_data=value;cmd_word_we=1;tick();cmd_word_we=0;
  end
 endtask
 task check_record;
  reg bad,empty;
  reg signed[31:0] bx0,by0,bx1,by1,wx0,wy0,wx1,wy1;
  begin
   bad=raw_bad();
   if(dut.record_bad!==bad)$fatal(1,"compact validation variant%0d word%0d value%h rawbad%b compactbad%b",
      variant,w,raw[w],bad,dut.record_bad);
   if(!bad)begin
    if(raw[0][7:0]==2)begin
     bx0=minimum($signed(raw[17]),$signed(raw[20]));by0=minimum($signed(raw[18]),$signed(raw[21]));
     bx1=maximum($signed(raw[17]),$signed(raw[20]))+1;by1=maximum($signed(raw[18]),$signed(raw[21]))+1;
    end else begin bx0=$signed(raw[8]);by0=$signed(raw[9]);bx1=$signed(raw[10]);by1=$signed(raw[11]);end
    wx0=maximum(maximum(bx0,$signed(raw[4])),0);wy0=maximum(maximum(by0,$signed(raw[5])),0);
    wx1=minimum(minimum(bx1,$signed(raw[6])),32);wy1=minimum(minimum(by1,$signed(raw[7])),32);
    empty=(wx0>=wx1)||(wy0>=wy1);
    if(empty!==((dut.window_x0>=dut.window_x1)||(dut.window_y0>=dut.window_y1)))
     $fatal(1,"compact clipping changed empty interval variant%0d word%0d",variant,w);
    if(!empty&&(wx0!=$signed(dut.window_x0)||wy0!=$signed(dut.window_y0)||
       wx1!=$signed(dut.window_x1)||wy1!=$signed(dut.window_y1)))
     $fatal(1,"compact clipping changed nonempty interval variant%0d word%0d",variant,w);
   end
   // Launch a deterministic subset through the actual registered setup.
   // Header words remain immutable until reset, so this also tests arbitrary
   // write order, high-bit sentinels and rewritten values in the new stages.
   if((checked%97)==0)begin
    cmd_valid=1;tick();cmd_valid=0;
    for(checkpoint=0;checkpoint<3;checkpoint=checkpoint+1)begin
     tick();
     if(dut.color_we||dut.depth_we||dut.tex_cfg_valid)
      $fatal(1,"staged validation exposed a side effect before acceptance");
    end
    if(dut.fault!==bad||(bad&&dut.state!=18)||(!bad&&dut.state!=12))
     $fatal(1,"staged raw32 validation variant%0d word%0d value%h rawbad%b fault%b state%0d",
       variant,w,raw[w],bad,dut.fault,dut.state);
    if(!bad)begin
     if(empty!==((dut.x0>=dut.x1)||(dut.y0>=dut.y1)))
      $fatal(1,"staged clipping changed empty raw32 interval");
     if(!empty&&(wx0!=$signed(dut.x0)||wy0!=$signed(dut.y0)||
        wx1!=$signed(dut.x1)||wy1!=$signed(dut.y1)))
      $fatal(1,"staged clipping changed nonempty raw32 interval");
    end
    setup_checked=setup_checked+1;
    reset=1;tick();reset=0;tick();
   end
   checked=checked+1;
  end
 endtask
 function automatic[31:0] directed;
  input integer index;
  begin
   case(index)
   0:directed=0;1:directed=1;2:directed=2;3:directed=3;4:directed=4;5:directed=5;
   6:directed=479;7:directed=480;8:directed=481;9:directed=639;10:directed=640;11:directed=641;
   12:directed=1023;13:directed=1024;14:directed=2047;15:directed=2048;
   16:directed=32'h1ffff;17:directed=32'h20000;18:directed=32'hffff0000;
   19:directed=32'h80000000;20:directed=32'h7fffffff;21:directed=32'hffffffff;
   22:directed=32'h00070100;23:directed=32'h00040002;24:directed=32'h01000100;
   default:directed=32'h00000100;
   endcase
  end
 endfunction
 initial begin
  reset=1;tick();reset=0;tick();
  for(variant=0;variant<8;variant=variant+1)begin
   kind=(variant<4)?variant:3;
   for(i=0;i<80;i=i+1)base[i]=0;
   base[0]=32'h70100|kind;base[6]=640;base[7]=480;
   if(kind==2)begin base[20]=31;base[21]=31;end
   else begin base[10]=4;base[11]=4;end
   if(kind==0)begin base[28]=1;base[32]=1;base[36]=1;end
   if(kind==3)begin base[1]=32'h40002;base[12]=32'h258000;base[13]=8;base[14]=8;base[15]=16;end
   if(variant==4)base[10]=0;
   if(variant==5)base[10]=32'hffffffff;
   if(variant==6)base[11]=0;
   if(variant==7)begin base[10]=32'hffffffff;base[11]=32'hffffffff;end
   // Reverse loading must produce the same complete record.
   for(i=79;i>=0;i=i-1)write_word(i,base[i]);
   for(w=0;w<80;w=w+1)begin
    for(n=0;n<112;n=n+1)begin
     rng=rng^(rng<<13);rng=rng^(rng>>7);rng=rng^(rng<<17);
     sample=(n<26)?directed(n):rng[31:0];
     if(n>=96&&w==0)sample=32'h100|((rng[19:16]&15)<<16)|(rng[1:0]);
     if(n>=96&&w==1)sample=((rng[18:16]&7)<<16)|((rng[9:8]%3)<<8)|(rng[2:0]%5);
     if(n>=96&&(w==26||w==27||w==30||w==31||w==34||w==35))sample={{14{rng[17]}},rng[17:0]};
     write_word(w,sample);check_record();
     write_word(w,base[w]);check_record();
    end
   end
  end
  $display("PASS %0d staged raw32 clipping/validation checkpoints",setup_checked);
  $display("PASS %0d raw32/compact record validation and clipping vectors",checked);$finish;
 end
endmodule
