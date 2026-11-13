// Tang Nano 20K hardware3921, GW2AR-LV18QN88C8/I7. No command player.
module tang20k_top (
    input wire clk27, input wire board_reset,
    input wire gpu_hclk, input wire gpu_reset_n, input wire gpu_abort,
    input wire gpu_ale_n, input wire gpu_wr_n, input wire gpu_rd_n,
    inout wire [7:0] gpu_d, output wire gpu_wready, output wire gpu_rvalid,
    output wire [3:0] hdmi_p, output wire [3:0] hdmi_n,
    output wire O_sdram_clk, output reg O_sdram_cke=0,
    output reg O_sdram_cs_n=1, output reg O_sdram_cas_n=1,
    output reg O_sdram_ras_n=1, output reg O_sdram_wen_n=1,
    inout wire [31:0] IO_sdram_dq,
    output reg [10:0] O_sdram_addr=0, output reg [1:0] O_sdram_ba=0,
    output reg [3:0] O_sdram_dqm=15
);
    reg [7:0] poweron=0;
    always @(posedge clk27) begin
        if(board_reset) poweron<=0;
        else if(!(&poweron)) poweron<=poweron+1'b1;
    end
    wire pll_reset=board_reset || !(&poweron);
    wire fast_clk, serial_clk, core_clk, pixel_clk, locked;
    // 27/3*28=252MHz, VCO504MHz. Serial126, core50.4, pixel25.2.
    rPLL #(.FCLKIN("27"),.IDIV_SEL(2),.FBDIV_SEL(27),.ODIV_SEL(2),
        .DYN_SDIV_SEL(2),.DEVICE("GW2AR-18C")) pll (
        .CLKIN(clk27),.CLKFB(1'b0),.RESET(pll_reset),.RESET_P(1'b0),
        .FBDSEL(6'b0),.IDSEL(6'b0),.ODSEL(6'b0),.PSDA(4'b0),
        .DUTYDA(4'b0),.FDLY(4'b0),.CLKOUT(fast_clk),.CLKOUTD(serial_clk),
        .CLKOUTP(),.CLKOUTD3(),.LOCK(locked));
    CLKDIV #(.DIV_MODE("5"),.GSREN("false")) core_div (
        .HCLKIN(fast_clk),.RESETN(locked),.CALIB(1'b1),.CLKOUT(core_clk));
    CLKDIV #(.DIV_MODE("5"),.GSREN("false")) pixel_div (
        .HCLKIN(serial_clk),.RESETN(locked),.CALIB(1'b1),.CLKOUT(pixel_clk));
    wire reset_request=!locked || pll_reset || !gpu_reset_n;
    (* async_reg="true" *) reg [1:0] core_reset_pipe=3;
    always @(posedge core_clk or posedge reset_request)
        if(reset_request) core_reset_pipe<=3;
        else core_reset_pipe<={core_reset_pipe[0],1'b0};
    wire reset=core_reset_pipe[1];
    (* async_reg="true" *) reg [1:0] host_reset_pipe=3,pixel_reset_pipe=3;
    always @(posedge gpu_hclk or posedge reset)
        if(reset) host_reset_pipe<=3;
        else host_reset_pipe<={host_reset_pipe[0],1'b0};
    always @(posedge pixel_clk or posedge reset_request)
        if(reset_request) pixel_reset_pipe<=3;
        else pixel_reset_pipe<={pixel_reset_pipe[0],1'b0};
    wire host_reset=host_reset_pipe[1];
    // One event per physical pixel. The toggle crosses as a single bit;
    // detection advances scanout shortly AFTER a pixel edge, leaving at least
    // one core period before that pixel is captured on the next pixel edge.
    reg pixel_toggle=0;
    always @(posedge pixel_clk or posedge pixel_reset_pipe[1])
        if(pixel_reset_pipe[1]) pixel_toggle<=0;
        else pixel_toggle<=!pixel_toggle;
    (* async_reg="true" *) reg pixel_meta=0,pixel_sync=0;
    reg pixel_previous=0;
    always @(posedge core_clk or posedge reset)
        if(reset) begin pixel_meta<=0;pixel_sync<=0;pixel_previous<=0;end
        else begin pixel_meta<=pixel_toggle;pixel_sync<=pixel_meta;pixel_previous<=pixel_sync;end
    wire pixel_advance=pixel_sync ^ pixel_previous;
    wire [7:0] rd_data;
    wire native_ready_base, native_valid;
    reg [6:0] address=0;
    // address==0, registered with the address so its decode is off the
    // half-cycle WREADY pad path. With RD_N high no read is requested, so
    // this equals the core's host_wr_ready.
    reg address_zero=1;
    assign gpu_wready=!host_reset && gpu_rd_n && (gpu_ale_n ? address_zero && native_ready_base : !gpu_d[7]);
    always @(posedge gpu_hclk or posedge host_reset)
        if(host_reset) begin address<=0; address_zero<=1; end
        else if(!gpu_ale_n && !gpu_wr_n && gpu_rd_n && gpu_wready) begin
            address<=gpu_d[6:0]; address_zero<=gpu_d[6:0]==0;
        end
    wire reading=!gpu_rd_n && gpu_wr_n && gpu_ale_n && !host_reset;
    assign gpu_d=reading ? rd_data : 8'bz;
    assign gpu_rvalid=reading && native_valid;
    wire [31:0] dq_out;
    wire dq_oe, unused_sd_clk, sd_cke,sd_cs,sd_cas,sd_ras,sd_we;
    wire [10:0] sd_addr;
    wire [1:0] sd_ba;
    wire [3:0] sd_mask;
    reg [31:0] dq_registered=0;
    reg dq_enable=0;
    // The logical controller presents commands BEFORE a core edge. Register
    // them there and let the chip sample the following falling core edge.
    // Do not reset this pipeline on a GPU reset: accepted WRITE tails drain.
    always @(posedge core_clk) begin
        O_sdram_cke<=sd_cke;O_sdram_cs_n<=sd_cs;O_sdram_cas_n<=sd_cas;
        O_sdram_ras_n<=sd_ras;O_sdram_wen_n<=sd_we;
        O_sdram_addr<=sd_addr;O_sdram_ba<=sd_ba;O_sdram_dqm<=sd_mask;
        dq_registered<=dq_out;dq_enable<=dq_oe;
    end
    assign IO_sdram_dq=dq_enable ? dq_registered : 32'bz;
    wire [15:0] rgb;
    wire hs,vs,de,pixel_ce;
    nano_gpu_core #(.POWERUP_CYCLES(10080),.REFRESH_INTERVAL(787),
        .CORE_HZ(50400000),.PIXEL_DIVIDER(2),.HOST_DIVIDER_METADATA(0),.EXTERNAL_PIXEL_TICK(1)) core (
        .clk(core_clk),.reset(reset),.host_clk(gpu_hclk),.abort_request(gpu_abort),
        .pixel_advance(pixel_advance),
        .host_wr_valid(!gpu_wr_n && gpu_ale_n),.host_wr_data(gpu_d),
        .host_wr_ready(),.host_wr_ready_base(native_ready_base),.host_rd_req(!gpu_rd_n && gpu_ale_n),
        .host_reg_addr(address),.host_rd_data(rd_data),.host_rd_valid(native_valid),
        .sd_dq_in(IO_sdram_dq),.sd_dq_out(dq_out),.sd_dq_oe(dq_oe),
        .sd_clk(unused_sd_clk),.sd_cke(sd_cke),.sd_cs_n(sd_cs),
        .sd_ras_n(sd_ras),.sd_cas_n(sd_cas),.sd_we_n(sd_we),
        .sd_ba(sd_ba),.sd_addr(sd_addr),.sd_dqm(sd_mask),
        .video_rgb565(rgb),.video_hsync(hs),.video_vsync(vs),.video_de(de),
        .video_x(),.video_y(),.video_pixel_ce(pixel_ce),.busy(),.ready(),.fault(),.fault_code());
    ODDR sd_clock (
        .CLK(core_clk),.D0(1'b0),.D1(1'b1),.TX(1'b0),.Q0(O_sdram_clk),.Q1());
    nano_tmds video (.pixel_clk(pixel_clk),.serial_clk(serial_clk),.reset(pixel_reset_pipe[1]),
        .rgb565(rgb),.hsync(hs),.vsync(vs),.de(de),.tmds_p(hdmi_p),.tmds_n(hdmi_n));
endmodule
