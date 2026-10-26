// Serial qualification harness for nano_gpu_core, not a board top. All core
// inputs remain externally controllable, and every core output is observed.
// No host, SDRAM, framebuffer or renderer functionality is replaced here.
//
// Shift 50 input bits MSB first with stimulus_shift=1. host_clk is an external
// independent clock. With capture_outputs=1, capture all 116 output bits; then
// set it low for 116 core clocks to feed that snapshot through the serial CRC.
// A snapshot register prevents simultaneous output bits from cancelling in a
// parallel XOR reduction. The CRC and its serially visible MSB retain the full
// output cone without requiring the logical core's 169 package connections.
module nano_gpu_probe (
    input wire clk, input wire reset, input wire host_clk,
    input wire stimulus_shift, input wire serial_data, input wire capture_outputs,
    output wire serial_result, output wire core_ready, output wire core_fault
);
    reg [49:0] stimulus;
    reg [115:0] observed;
    reg [31:0] checksum;
    wire host_wr_ready, host_rd_valid;
    wire [7:0] host_rd_data;
    wire sd_clk, sd_cke, sd_cs_n, sd_ras_n, sd_cas_n, sd_we_n;
    wire [1:0] sd_ba;
    wire [10:0] sd_addr;
    wire [31:0] sd_dq_out;
    wire sd_dq_oe;
    wire [3:0] sd_dqm;
    wire [15:0] video_rgb565;
    wire video_hsync, video_vsync, video_de, video_pixel_ce;
    wire [9:0] video_x;
    wire [8:0] video_y;
    wire busy;
    wire [7:0] fault_code;
    wire [115:0] core_outputs = {
        host_wr_ready, host_rd_data, host_rd_valid,
        sd_clk, sd_cke, sd_cs_n, sd_ras_n, sd_cas_n, sd_we_n,
        sd_ba, sd_addr, sd_dq_out, sd_dq_oe, sd_dqm,
        video_rgb565, video_hsync, video_vsync, video_de,
        video_x, video_y, video_pixel_ce,
        busy, core_ready, core_fault, fault_code
    };
    wire checksum_feedback = checksum[31] ^ observed[115];

    nano_gpu_core core (
        .clk(clk), .reset(reset), .host_clk(host_clk),
        .abort_request(stimulus[49]),
        .host_wr_valid(stimulus[48]), .host_wr_data(stimulus[47:40]),
        .host_wr_ready(host_wr_ready), .host_rd_req(stimulus[39]),
        .host_reg_addr(stimulus[38:32]), .host_rd_data(host_rd_data),
        .host_rd_valid(host_rd_valid), .sd_dq_in(stimulus[31:0]),
        .sd_clk(sd_clk), .sd_cke(sd_cke), .sd_cs_n(sd_cs_n),
        .sd_ras_n(sd_ras_n), .sd_cas_n(sd_cas_n), .sd_we_n(sd_we_n),
        .sd_ba(sd_ba), .sd_addr(sd_addr), .sd_dq_out(sd_dq_out),
        .sd_dq_oe(sd_dq_oe), .sd_dqm(sd_dqm),
        .video_rgb565(video_rgb565), .video_hsync(video_hsync),
        .video_vsync(video_vsync), .video_de(video_de),
        .video_x(video_x), .video_y(video_y), .video_pixel_ce(video_pixel_ce),
        .busy(busy), .ready(core_ready), .fault(core_fault), .fault_code(fault_code)
    );
    always @(posedge clk) begin
        if (reset) begin
            stimulus <= 0;
            observed <= 0;
            checksum <= 0;
        end else begin
            if (stimulus_shift) stimulus <= {stimulus[48:0],serial_data};
            if (capture_outputs) observed <= core_outputs;
            else begin
                observed <= {observed[114:0],1'b0};
                checksum <= {checksum[30:0],1'b0} ^
                    (checksum_feedback ? 32'h04c11db7 : 32'b0);
            end
        end
    end
    assign serial_result = checksum[31];
endmodule
