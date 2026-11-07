// Diagnostic Tang Nano 20K image, separate from the GPU programming image.
// It reproduces only tang20k_top's host-bus pad behavior: the same pins,
// address-latch condition, WREADY rule and read-enable condition. A read
// returns the full byte the FPGA sampled at its last accepted address write,
// so the RP can print exactly which address the FPGA latched.
// There is no GPU core, SDRAM traffic, PLL or video output in this image.
module tang20k_bus_echo (
    input wire gpu_hclk, input wire gpu_reset_n, input wire gpu_abort,
    input wire gpu_ale_n, input wire gpu_wr_n, input wire gpu_rd_n,
    inout wire [7:0] gpu_d, output wire gpu_wready, output wire gpu_rvalid
);
    wire reset = !gpu_reset_n;
    reg [7:0] latched = 0;
    reg valid = 0;
    // Same expressions as tang20k_top: address phase WREADY is !gpu_d[7];
    // data writes are always accepted because no packet is consumed here.
    assign gpu_wready = !reset && gpu_rd_n && (gpu_ale_n ? 1'b1 : !gpu_d[7]);
    wire reading = !gpu_rd_n && gpu_wr_n && gpu_ale_n && !reset;
    always @(posedge gpu_hclk or posedge reset)
        if (reset) begin latched <= 0; valid <= 0; end
        else begin
            if (!gpu_ale_n && !gpu_wr_n && gpu_rd_n && gpu_wready) latched <= gpu_d;
            // Like a capability read: valid on the first rising edge of a read.
            valid <= !gpu_rd_n && gpu_ale_n;
        end
    assign gpu_d = reading ? latched : 8'bz;
    assign gpu_rvalid = reading && valid;
    wire unused_abort = gpu_abort;
endmodule
