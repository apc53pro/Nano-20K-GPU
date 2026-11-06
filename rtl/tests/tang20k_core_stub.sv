// Board-wrapper unit-test model only. Excluded from every synthesis source list.
module nano_gpu_core #(
    parameter integer POWERUP_CYCLES = 10080,
    parameter integer REFRESH_INTERVAL = 1575,
    parameter integer CORE_HZ = 100800000,
    parameter integer HOST_DIVIDER_METADATA = 4,
    parameter integer PIXEL_DIVIDER = 4,
    parameter integer EXTERNAL_PIXEL_TICK = 0,
    parameter integer SDRAM_T_RP=3, SDRAM_T_RCD=3, SDRAM_T_RFC=9,
    parameter integer SDRAM_T_MRD=3, SDRAM_T_WR=3, SDRAM_T_RAS=6,
    parameter integer SDRAM_T_RC=9, SDRAM_READ_TO_WRITE=2,
    parameter integer SDRAM_WRITE_TO_READ=2, SDRAM_CAS_LATENCY=3
)(
    input wire clk, input wire reset, input wire host_clk, input wire abort_request,
    input wire pixel_advance,
    input wire host_wr_valid, input wire [7:0] host_wr_data,
    output wire host_wr_ready,
    input wire host_rd_req, input wire [6:0] host_reg_addr,
    output reg [7:0] host_rd_data, output reg host_rd_valid,
    input wire [31:0] sd_dq_in,
    output wire sd_clk, output wire sd_cke, output wire sd_cs_n,
    output wire sd_ras_n, output wire sd_cas_n, output wire sd_we_n,
    output wire [1:0] sd_ba, output wire [10:0] sd_addr,
    output wire [31:0] sd_dq_out, output wire sd_dq_oe, output wire [3:0] sd_dqm,
    output wire [15:0] video_rgb565, output wire video_hsync,
    output wire video_vsync, output wire video_de,
    output wire [9:0] video_x, output wire [8:0] video_y,
    output reg video_pixel_ce,
    output wire busy, output wire ready, output wire fault,
    output reg [7:0] fault_code
);
    integer writes=0;
    reg [7:0] last_byte=0;
    reg [15:0] stamp=0;
    assign host_wr_ready=!reset && host_reg_addr==0 && !host_rd_req;
    always @(posedge host_clk or posedge reset) begin
        if(reset) begin host_rd_data<=0;host_rd_valid<=0;writes<=0;end
        else begin
            host_rd_valid<=host_rd_req;
            if(host_rd_req) host_rd_data<=8'ha0 ^ {1'b0,host_reg_addr};
            if(host_wr_valid && host_wr_ready) begin writes<=writes+1;last_byte<=host_wr_data;end
        end
    end
    always @(posedge clk) if(reset) stamp<=0;else stamp<=stamp+1;
    assign sd_clk=clk;
    assign sd_cke=!reset;assign sd_cs_n=reset;assign sd_ras_n=1;assign sd_cas_n=1;assign sd_we_n=1;
    assign sd_addr=stamp[10:0];assign sd_ba=stamp[12:11];assign sd_dqm=0;
    assign sd_dq_out={stamp,stamp};assign sd_dq_oe=stamp[0];
    reg [15:0] pixel_stamp=0;
    always @(posedge clk) if(reset) pixel_stamp<=0;else if(pixel_advance) pixel_stamp<=pixel_stamp+1'b1;
    assign video_rgb565=pixel_stamp;assign video_hsync=1;assign video_vsync=1;assign video_de=0;
    assign video_x=0;assign video_y=0;assign busy=0;assign ready=!reset;assign fault=0;
    always @* begin video_pixel_ce=stamp[0];fault_code=0;end
endmodule
