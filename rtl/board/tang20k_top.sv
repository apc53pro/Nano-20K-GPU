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
    // 27/3*28=252MHz, VCO504MHz. Serial126, pixel25.2; core divided below.
    rPLL #(.FCLKIN("27"),.IDIV_SEL(2),.FBDIV_SEL(27),.ODIV_SEL(2),
        .DYN_SDIV_SEL(2),.DEVICE("GW2AR-18C")) pll (
        .CLKIN(clk27),.CLKFB(1'b0),.RESET(pll_reset),.RESET_P(1'b0),
        .FBDSEL(6'b0),.IDSEL(6'b0),.ODSEL(6'b0),.PSDA(4'b0),
        .DUTYDA(4'b0),.FDLY(4'b0),.CLKOUT(fast_clk),.CLKOUTD(serial_clk),
        .CLKOUTP(),.CLKOUTD3(),.LOCK(locked));
    CLKDIV #(.DIV_MODE("5"),.GSREN("false")) pixel_div (
        .HCLKIN(serial_clk),.RESETN(locked),.CALIB(1'b1),.CLKOUT(pixel_clk));
    // Fixed slow bus-test image: 25.2 / 4 = 6.3 MHz. This is a real clock
    // divider, not a clock enable or a selectable compatibility mode.
    reg [1:0] core_phase=0;
    always @(posedge pixel_clk or negedge locked)
        if(!locked) core_phase<=0;
        else core_phase<=core_phase+1'b1;
    BUFG core_buffer (.I(core_phase[1]),.O(core_clk));
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
    // A very slow GPU cannot feed the existing 25.2 MHz scanout handoff.
    // Keep an independent black 640x480/60 raster for monitor lock only.
    // No GPU-rendered pixels cross into this diagnostic display path.
    reg [9:0] video_column=0,video_row=0;
    always @(posedge pixel_clk or posedge pixel_reset_pipe[1]) begin
        if(pixel_reset_pipe[1]) begin video_column<=0;video_row<=0;end
        else if(video_column==10'd799) begin
            video_column<=0;
            video_row<=video_row==10'd524 ? 10'd0 : video_row+1'b1;
        end else video_column<=video_column+1'b1;
    end
    wire video_hs=!(video_column>=10'd656 && video_column<10'd752);
    wire video_vs=!(video_row>=10'd490 && video_row<10'd492);
    wire video_de=video_column<10'd640 && video_row<10'd480;
    wire [7:0] rd_data;
    wire native_ready, native_valid;
    reg [6:0] address=0;
    assign gpu_wready=!host_reset && gpu_rd_n && (gpu_ale_n ? native_ready : !gpu_d[7]);
    always @(posedge gpu_hclk or posedge host_reset)
        if(host_reset) address<=0;
        else if(!gpu_ale_n && !gpu_wr_n && gpu_rd_n && gpu_wready) address<=gpu_d[6:0];
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
    nano_gpu_core #(.POWERUP_CYCLES(1260),.REFRESH_INTERVAL(98),
        .CORE_HZ(6300000),.PIXEL_DIVIDER(2),.HOST_DIVIDER_METADATA(0),
        .SDRAM_T_RP(1),.SDRAM_T_RCD(1),.SDRAM_T_RFC(1),.SDRAM_T_MRD(2),
        .SDRAM_T_WR(1),.SDRAM_T_RAS(1),.SDRAM_T_RC(2),
        .SDRAM_READ_TO_WRITE(1),.SDRAM_WRITE_TO_READ(1),.SDRAM_CAS_LATENCY(3)) core (
        .clk(core_clk),.reset(reset),.host_clk(gpu_hclk),.abort_request(gpu_abort),
        .pixel_advance(1'b0),
        .host_wr_valid(!gpu_wr_n && gpu_ale_n),.host_wr_data(gpu_d),
        .host_wr_ready(native_ready),.host_rd_req(!gpu_rd_n && gpu_ale_n),
        .host_reg_addr(address),.host_rd_data(rd_data),.host_rd_valid(native_valid),
        .sd_dq_in(IO_sdram_dq),.sd_dq_out(dq_out),.sd_dq_oe(dq_oe),
        .sd_clk(unused_sd_clk),.sd_cke(sd_cke),.sd_cs_n(sd_cs),
        .sd_ras_n(sd_ras),.sd_cas_n(sd_cas),.sd_we_n(sd_we),
        .sd_ba(sd_ba),.sd_addr(sd_addr),.sd_dqm(sd_mask),
        .video_rgb565(),.video_hsync(),.video_vsync(),.video_de(),
        .video_x(),.video_y(),.video_pixel_ce(),.busy(),.ready(),.fault(),.fault_code());
    ODDR sd_clock (
        .CLK(core_clk),.D0(1'b0),.D1(1'b1),.TX(1'b0),.Q0(O_sdram_clk),.Q1());
    nano_tmds video (.pixel_clk(pixel_clk),.serial_clk(serial_clk),.reset(pixel_reset_pipe[1]),
        .rgb565(16'b0),.hsync(video_hs),.vsync(video_vs),.de(video_de),.tmds_p(hdmi_p),.tmds_n(hdmi_n));
endmodule
