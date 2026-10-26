// RGB565 nearest-neighbour texture unit. The RAM and address multipliers have
// identical native bindings in synthesis and registered behavioural models in
// simulation. One miss owns one bounded external read; hit initiation interval
// is one clock when the output is ready. No external-memory image is stored in
// this module.
module nano_texture_cache (
    input  wire clk,
    input  wire reset,
    input  wire flush,
    input  wire cfg_valid,
    output wire cfg_ready,
    input  wire [22:0] cfg_base,
    input  wire [10:0] cfg_width,
    input  wire [10:0] cfg_height,
    input  wire [22:0] cfg_pitch,
    input  wire cfg_repeat_u,
    input  wire cfg_repeat_v,
    input  wire req_valid,
    output wire req_ready,
    input  wire signed [31:0] req_x,
    input  wire signed [31:0] req_y,
    input  wire [5:0] req_tag,
    output reg  rsp_valid,
    input  wire rsp_ready,
    output reg  [15:0] rsp_texel,
    output reg  [5:0] rsp_tag,
    output reg  fault,
    output wire mem_cmd_valid,
    input  wire mem_cmd_ready,
    output wire mem_cmd_write,
    output wire [22:0] mem_cmd_addr,
    output wire [4:0] mem_cmd_words,
    output wire mem_w_valid,
    input  wire mem_w_ready,
    output wire [31:0] mem_w_data,
    output wire [3:0] mem_w_strb,
    input  wire mem_r_valid,
    output wire mem_r_ready,
    input  wire [31:0] mem_r_data,
    input  wire mem_r_last,
    input  wire mem_done,
    input  wire mem_error
);
    localparam [2:0] RUN = 3'd0, MISS_COMMAND = 3'd1,
        REFILL = 3'd2, WAIT_DONE = 3'd3;
    reg [2:0] state;
    reg configured;
    reg [22:0] base, pitch;
    reg [10:0] width, height;
    reg repeat_u, repeat_v;
    reg [23:0] extent;
    reg [63:0] line_valid;
    // Tags use a second native RAM, read alongside the linear-address latch.
    // A completed refill forwards its tag until that retained miss advances;
    // this avoids an undefined same-address read/write and a replay bubble.
    reg refill_forward;

    reg [2:0] multiply_valid;
    reg [9:0] multiply_x [0:2];
    reg [5:0] multiply_tag [0:2];
    reg linear_valid;
    reg [21:0] linear_address;
    reg [5:0] linear_tag;
    reg lookup_valid;
    reg [9:0] lookup_address;
    reg [5:0] lookup_tag;
    reg read_pending;
    reg [5:0] read_tag;

    reg [21:0] miss_line;
    reg [5:0] miss_slot;
    reg [11:0] miss_tag;
    reg [3:0] read_words;
    reg [3:0] received_words;
    reg [4:0] refill_texel;
    reg hold_valid;
    reg [31:0] hold_word;
    reg high_half;
    reg done_seen;

    wire output_advance = !rsp_valid || rsp_ready;
    wire [5:0] linear_slot = linear_address[9:4];
    wire [11:0] linear_line_tag = linear_address[21:10];
    wire [15:0] tag_data;
    wire [11:0] observed_tag = refill_forward ? miss_tag : tag_data[11:0];
    wire line_hit = line_valid[linear_slot] && observed_tag == linear_line_tag;
    wire advance = state == RUN && output_advance && (!linear_valid || line_hit);
    wire pipeline_empty = !(|multiply_valid) && !linear_valid && !lookup_valid &&
        !read_pending && !rsp_valid;
    assign cfg_ready = state == RUN && pipeline_empty && !flush;
    assign req_ready = configured && !fault && !reset && !flush && !cfg_valid && advance;
    wire accepting = req_valid && req_ready;

    function [9:0] coordinate;
        input signed [31:0] value;
        input [10:0] size;
        input repeating;
        begin
            if (repeating) coordinate = value[9:0] & (size[9:0] - 10'd1);
            else if (value < 0) coordinate = 10'd0;
            else if (value >= $signed({21'd0,size})) coordinate = size[9:0] - 10'd1;
            else coordinate = value[9:0];
        end
    endfunction
    function [33:0] allocation_extent;
        input [22:0] stride;
        input [10:0] rows;
        begin
            case (rows)
                11'd1: allocation_extent = {11'd0,stride};
                11'd2: allocation_extent = {10'd0,stride,1'd0};
                11'd4: allocation_extent = {9'd0,stride,2'd0};
                11'd8: allocation_extent = {8'd0,stride,3'd0};
                11'd16: allocation_extent = {7'd0,stride,4'd0};
                11'd32: allocation_extent = {6'd0,stride,5'd0};
                11'd64: allocation_extent = {5'd0,stride,6'd0};
                11'd128: allocation_extent = {4'd0,stride,7'd0};
                11'd256: allocation_extent = {3'd0,stride,8'd0};
                11'd512: allocation_extent = {2'd0,stride,9'd0};
                11'd1024: allocation_extent = {1'd0,stride,10'd0};
                default: allocation_extent = 34'h3ffffffff;
            endcase
        end
    endfunction
    wire [33:0] new_extent = allocation_extent(cfg_pitch,cfg_height);
    wire width_pot = cfg_width != 0 && (cfg_width & (cfg_width-11'd1)) == 0;
    wire height_pot = cfg_height != 0 && (cfg_height & (cfg_height-11'd1)) == 0;
    wire descriptor_ok = width_pot && height_pot && cfg_width <= 1024 &&
        cfg_height <= 1024 && cfg_base >= 23'h258000 && cfg_base[1:0] == 0 &&
        cfg_pitch[1:0] == 0 && cfg_pitch >= ({12'd0,cfg_width} << 1) &&
        ({12'd0,cfg_base} + {1'b0,new_extent}) <= 35'd8388608;
    wire [9:0] accepted_x = coordinate(req_x,width,repeat_u);
    wire [9:0] accepted_y = coordinate(req_y,height,repeat_v);
    wire [21:0] pitch_words = pitch[22:1];
    wire signed [35:0] low_product, high_product;
    nano_mult18 low_address_multiplier (
        .clk(clk), .ce(advance), .reset(reset),
        .a($signed({8'd0,accepting ? accepted_y : 10'd0})),
        .b($signed({1'b0,pitch_words[16:0]})), .product(low_product));
    nano_mult18 high_address_multiplier (
        .clk(clk), .ce(advance), .reset(reset),
        .a($signed({8'd0,accepting ? accepted_y : 10'd0})),
        .b($signed({13'd0,pitch_words[21:17]})), .product(high_product));
    // The signed-native operands are nonnegative: Y has ten bits, the low
    // stride limb seventeen and its high limb five. Their exact products need
    // 27 and 15 bits. Trimming provably-zero native outputs avoids creating a
    // fictitious 54-bit address adder solely because DOUT is always 36 bits.
    wire [32:0] address_sum = {6'd0,low_product[26:0]} +
        {1'b0,high_product[14:0],17'd0} + {23'd0,multiply_x[2]};

    wire cache_read_enable = state == RUN && output_advance && lookup_valid;
    wire refill_has_word = state == REFILL && hold_valid;
    wire refill_padding = state == REFILL && !hold_valid && received_words == read_words;
    wire cache_write_enable = (refill_has_word || refill_padding) && refill_texel < 16;
    wire [15:0] cache_write_data = refill_has_word ?
        (high_half ? hold_word[31:16] : hold_word[15:0]) : 16'd0;
    wire [15:0] cache_data;
    nano_sdp16 cache_ram (
        .wclk(clk), .wce(1'b1), .we(cache_write_enable),
        .waddr({miss_slot,refill_texel[3:0]}), .wdata(cache_write_data),
        .rclk(clk), .rce(1'b1), .re(cache_read_enable), .raddr(lookup_address),
        .reset_output(reset), .rdata(cache_data));

    wire [23:0] miss_byte_offset = {1'b0,miss_line,1'b0};
    wire [23:0] remaining_bytes = extent - miss_byte_offset;
    wire [3:0] bounded_read_words = remaining_bytes >= 32 ? 4'd8 : {1'b0,remaining_bytes[4:2]};
    assign mem_cmd_valid = state == MISS_COMMAND && !fault;
    assign mem_cmd_write = 1'b0;
    assign mem_cmd_addr = base + miss_byte_offset[22:0];
    assign mem_cmd_words = {1'b0,bounded_read_words};
    assign mem_w_valid = 1'b0;
    assign mem_w_data = 32'd0;
    assign mem_w_strb = 4'd0;
    assign mem_r_ready = state == REFILL && received_words < read_words &&
        (!hold_valid || high_half);
    wire accepting_word = mem_r_valid && mem_r_ready;
    wire last_cache_write = cache_write_enable && refill_texel == 15;
    wire publish_tag = ((state == REFILL && last_cache_write) || state == WAIT_DONE) &&
        (done_seen || mem_done) && !reset && !flush && !mem_error;
    nano_sdp16 tag_ram (
        .wclk(clk), .wce(1'b1), .we(publish_tag),
        .waddr({4'b0,miss_slot}), .wdata({4'b0,miss_tag}),
        .rclk(clk), .rce(1'b1), .re(advance && multiply_valid[2] && !reset && !flush),
        .raddr({4'b0,address_sum[9:4]}), .reset_output(reset), .rdata(tag_data));
    integer i;
    always @(posedge clk) begin
        if (reset) begin
            state <= RUN; configured <= 0; base <= 0; pitch <= 0;
            width <= 0; height <= 0; repeat_u <= 0; repeat_v <= 0; extent <= 0;
            line_valid <= 0; refill_forward <= 0; multiply_valid <= 0; linear_valid <= 0;
            lookup_valid <= 0; read_pending <= 0; rsp_valid <= 0;
            rsp_texel <= 0; rsp_tag <= 0; fault <= 0;
            linear_address <= 0; linear_tag <= 0; lookup_address <= 0; lookup_tag <= 0;
            read_tag <= 0; miss_line <= 0; miss_slot <= 0; miss_tag <= 0;
            read_words <= 0; received_words <= 0; refill_texel <= 0;
            hold_valid <= 0; hold_word <= 0; high_half <= 0; done_seen <= 0;
            for (i=0;i<3;i=i+1) begin multiply_x[i] <= 0; multiply_tag[i] <= 0; end
        end else begin
            if (rsp_valid && rsp_ready) rsp_valid <= 0;
            if (output_advance && read_pending) begin
                rsp_valid <= 1; rsp_texel <= cache_data; rsp_tag <= read_tag;
                read_pending <= 0;
            end
            if (flush) begin
                if (state != RUN || !pipeline_empty) fault <= 1;
                else line_valid <= 0;
            end
            if (cfg_valid && cfg_ready) begin
                if (!descriptor_ok) fault <= 1;
                else begin
                    if (!configured || base != cfg_base || pitch != cfg_pitch ||
                        width != cfg_width || height != cfg_height)
                        line_valid <= 0;
                    base <= cfg_base; pitch <= cfg_pitch; width <= cfg_width;
                    height <= cfg_height; extent <= new_extent[23:0];
                    repeat_u <= cfg_repeat_u; repeat_v <= cfg_repeat_v;
                    configured <= 1;
                end
            end
            if (state == RUN && output_advance) begin
                if (lookup_valid) begin
                    read_pending <= 1; read_tag <= lookup_tag; lookup_valid <= 0;
                end
                if (linear_valid) begin
                    if (line_hit) begin
                        lookup_address <= linear_address[9:0];
                        lookup_tag <= linear_tag; lookup_valid <= 1;
                    end else begin
                        miss_line <= {linear_address[21:4],4'd0};
                        miss_slot <= linear_slot; miss_tag <= linear_line_tag;
                        line_valid[linear_slot] <= 0;
                        refill_texel <= 0; received_words <= 0;
                        hold_valid <= 0; high_half <= 0; done_seen <= 0;
                        state <= MISS_COMMAND;
                    end
                end
            end
            if (advance) begin
                refill_forward <= 0;
                multiply_valid <= {multiply_valid[1:0],accepting};
                for (i=2;i>0;i=i-1) begin
                    multiply_x[i] <= multiply_x[i-1];
                    multiply_tag[i] <= multiply_tag[i-1];
                end
                multiply_x[0] <= accepted_x; multiply_tag[0] <= req_tag;
                linear_valid <= multiply_valid[2];
                if (multiply_valid[2]) begin
                    if (|address_sum[32:22] || {1'b0,address_sum[21:0],1'b0} >= extent)
                        fault <= 1;
                    linear_address <= address_sum[21:0];
                    linear_tag <= multiply_tag[2];
                end
            end
            if (state == MISS_COMMAND && mem_cmd_valid && mem_cmd_ready) begin
                if (remaining_bytes == 0 || miss_byte_offset >= extent || bounded_read_words == 0)
                    fault <= 1;
                read_words <= bounded_read_words;
                state <= REFILL;
            end
            if (state == REFILL || state == WAIT_DONE) begin
                if (mem_done) begin
                    done_seen <= 1;
                    if (received_words + (accepting_word ? 4'd1 : 4'd0) != read_words)
                        fault <= 1;
                end
            end
            if (state == REFILL) begin
                if (cache_write_enable) refill_texel <= refill_texel + 5'd1;
                if (hold_valid) begin
                    if (!high_half) high_half <= 1;
                    else begin hold_valid <= 0; high_half <= 0; end
                end
                if (accepting_word) begin
                    hold_word <= mem_r_data; hold_valid <= 1; high_half <= 0;
                    received_words <= received_words + 4'd1;
                    if (mem_r_last != (received_words + 4'd1 == read_words)) fault <= 1;
                end
                if (last_cache_write) begin
                    if (done_seen || mem_done) begin
                        refill_forward <= 1; line_valid[miss_slot] <= 1;
                        state <= RUN;
                    end else state <= WAIT_DONE;
                end
            end
            if (state == WAIT_DONE && (done_seen || mem_done)) begin
                refill_forward <= 1; line_valid[miss_slot] <= 1;
                state <= RUN;
            end
            if (mem_error && state != RUN) begin
                // The service's error completes/abandons this transaction.
                // Preserve the sticky error rather than inventing texels.
                fault <= 1; state <= RUN; line_valid <= 0;
                multiply_valid <= 0; linear_valid <= 0; lookup_valid <= 0;
                read_pending <= 0; rsp_valid <= 0; hold_valid <= 0;
            end
        end
    end
    // Reserved write-channel signals remain connected at the public boundary
    // so the texture client has the same bus shape as the other GPU clients.
    wire unused_write_ready = mem_w_ready;
endmodule
