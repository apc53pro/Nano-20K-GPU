// Independent 640x480 RGB565 scanout. All pixels arrive through memory client
// zero and two native 1024x16 BSRAM banks. No framebuffer exists in this module.
// The banks form one 1024-word FIFO: bank 0 holds each word's low (even) pixel
// and bank 1 its high (odd) pixel. A response word is accepted every edge, so a
// refill never holds the shared SDRAM service while pixels trickle out.
module nano_scanout (
    input wire clk, input wire reset, input wire pixel_ce,
    input wire memory_ready, input wire front_id,
    output reg [15:0] pixel, output reg hsync, output reg vsync, output reg de,
    output reg [9:0] x, output reg [8:0] y,
    output reg vblank_pulse, output reg [31:0] underflows,
    output wire mem_cmd_valid, input wire mem_cmd_ready,
    output wire mem_cmd_write, output wire [22:0] mem_cmd_addr,
    output wire [4:0] mem_cmd_words,
    output wire mem_w_valid, input wire mem_w_ready,
    output wire [31:0] mem_w_data, output wire [3:0] mem_w_strb,
    input wire mem_r_valid, output wire mem_r_ready,
    input wire [31:0] mem_r_data, input wire mem_r_last,
    input wire mem_done, input wire mem_error
);
    localparam [18:0] FRAME_PIXELS = 19'd307200;
    reg [9:0] horizontal;
    reg [9:0] vertical;
    reg seen_front, previous_memory_ready;
    reg streaming, frame_ready, frame_black;
    reg [18:0] next_fetch;
    reg request_valid, request_discard;
    reg [22:0] request_address;
    reg outstanding, discard_response;
    reg [4:0] received_words;
    reg [9:0] read_pointer, write_pointer;
    reg [10:0] stored; // words, 0..1024
    reg read_pending;
    // Current pixel and the second pixel of the same word.
    reg output_valid, spare_valid;
    reg [15:0] output_word, spare_word;
    wire [15:0] fifo0_data, fifo1_data;

    wire active_pixel = horizontal < 10'd640 && vertical < 10'd480;
    wire enter_blank = pixel_ce && horizontal == 10'd799 && vertical == 10'd479;
    wire enter_frame = pixel_ce && horizontal == 10'd799 && vertical == 10'd524;
    wire front_changed = front_id != seen_front;
    wire disabled = previous_memory_ready && !memory_ready;
    wire consume_pixel = pixel_ce && active_pixel && frame_ready && !frame_black && memory_ready;
    wire underflow_now = consume_pixel && !output_valid;
    wire base_flush = enter_blank || front_changed || disabled || underflow_now || mem_error;
    wire bad_response = mem_r_valid && mem_r_ready && !discard_response && streaming &&
        (mem_r_last != (received_words == 5'd15));
    wire flush_stream = base_flush || bad_response;
    wire fifo_room = stored != 11'd1024;
    wire fifo_push = mem_r_valid && mem_r_ready && !discard_response && streaming && !flush_stream;
    // A word supplies two pixels. Read the next one when the current pixel is
    // being consumed (or absent) and its partner has already been shown.
    wire fifo_read = !read_pending && !spare_valid && (!output_valid || consume_pixel) &&
        stored != 0 && streaming && !flush_stream;

    nano_sdp16 fifo0 (
        .wclk(clk), .wce(1'b1), .we(fifo_push),
        .waddr(write_pointer), .wdata(mem_r_data[15:0]),
        .rclk(clk), .rce(1'b1), .re(fifo_read),
        .raddr(read_pointer), .reset_output(reset), .rdata(fifo0_data)
    );
    nano_sdp16 fifo1 (
        .wclk(clk), .wce(1'b1), .we(fifo_push),
        .waddr(write_pointer), .wdata(mem_r_data[31:16]),
        .rclk(clk), .rce(1'b1), .re(fifo_read),
        .raddr(read_pointer), .reset_output(reset), .rdata(fifo1_data)
    );

    // A command already offered to the service remains stable until accepted,
    // including if a front swap happens meanwhile. Its response is then
    // discarded before the new generation starts fetching.
    assign mem_cmd_valid = request_valid;
    assign mem_cmd_write = 1'b0;
    assign mem_cmd_addr = request_address;
    assign mem_cmd_words = 5'd16;
    assign mem_w_valid = 1'b0;
    assign mem_w_data = 32'b0;
    assign mem_w_strb = 4'b0;
    assign mem_r_ready = outstanding && (discard_response || !streaming || base_flush || fifo_room);

    always @(posedge clk) begin
        if (reset) begin
            pixel <= 0; hsync <= 1; vsync <= 1; de <= 0; x <= 0; y <= 0;
            horizontal <= 0; vertical <= 0; vblank_pulse <= 0; underflows <= 0;
            seen_front <= 0; previous_memory_ready <= 0;
            streaming <= 0; frame_ready <= 0; frame_black <= 0;
            next_fetch <= 0; outstanding <= 0; discard_response <= 0; received_words <= 0;
            request_valid <= 0; request_discard <= 0; request_address <= 0;
            read_pointer <= 0; write_pointer <= 0; stored <= 0;
            read_pending <= 0; output_valid <= 0; output_word <= 0;
            spare_valid <= 0; spare_word <= 0;
        end else begin
            vblank_pulse <= 0;
            seen_front <= front_id;
            previous_memory_ready <= memory_ready;

            // Offer the next refill before the service becomes idle. Waiting
            // for mem_done to clear outstanding leaves a request bubble in
            // which a slow foreground WRITE can win every arbitration turn,
            // defeating the service's bounded scanout priority.
            // Reserve 16 words for the active transaction and 16 for this
            // queued one. The command remains stable until accepted; no extra
            // memory port is added.
            if (!request_valid && streaming && memory_ready &&
                    next_fetch < FRAME_PIXELS && stored <= 11'd992 && !flush_stream) begin
                request_valid <= 1;
                request_discard <= 0;
                request_address <= (front_id ? 23'h096000 : 23'b0) + {3'b0,next_fetch,1'b0};
            end
            // Completion can coincide with acceptance of the queued refill.
            // The newly accepted owner must remain outstanding in that case.
            if (mem_done || mem_error) begin
                outstanding <= 0;
                discard_response <= 0;
            end
            if (mem_cmd_valid && mem_cmd_ready) begin
                request_valid <= 0;
                outstanding <= 1;
                discard_response <= request_discard || flush_stream || !streaming;
                received_words <= 0;
                if (!request_discard && !flush_stream) next_fetch <= next_fetch + 19'd32;
            end
            if (fifo_push) write_pointer <= write_pointer + 10'd1;
            if (mem_r_valid && mem_r_ready) received_words <= received_words + 5'd1;
            if (consume_pixel && output_valid) begin
                if (spare_valid) begin output_word <= spare_word; spare_valid <= 0; end
                else output_valid <= 0;
            end
            if (read_pending) begin
                output_word <= fifo0_data; spare_word <= fifo1_data;
                output_valid <= 1; spare_valid <= 1;
                read_pending <= 0;
            end
            if (fifo_read) begin
                read_pending <= 1;
                read_pointer <= read_pointer + 10'd1;
            end
            case ({fifo_push,fifo_read})
                2'b10: stored <= stored + 11'd1;
                2'b01: stored <= stored - 11'd1;
                default: begin end
            endcase

            if (pixel_ce) begin
                x <= horizontal;
                y <= vertical[8:0];
                de <= active_pixel;
                hsync <= !(horizontal >= 10'd656 && horizontal < 10'd752);
                vsync <= !(vertical >= 10'd490 && vertical < 10'd492);
                pixel <= (consume_pixel && output_valid) ? output_word : 16'b0;
                if (horizontal == 10'd799) begin
                    horizontal <= 0;
                    if (vertical == 10'd524) vertical <= 0;
                    else vertical <= vertical + 10'd1;
                end else horizontal <= horizontal + 10'd1;
            end
            if (enter_frame) frame_ready <= streaming && memory_ready;

            if (flush_stream) begin
                // Already accepted SDRAM work still retires. Old words are
                // discarded until its completion; new requests use the new
                // front and restart from pixel zero during vertical blank.
                read_pointer <= 0; write_pointer <= 0; stored <= 0;
                read_pending <= 0; output_valid <= 0; spare_valid <= 0;
                next_fetch <= 0;
                if (request_valid && !mem_cmd_ready) request_discard <= 1;
                discard_response <= (request_valid && mem_cmd_ready) ||
                    (outstanding && !mem_done && !mem_error);
            end
            if (enter_blank) begin
                vblank_pulse <= 1;
                streaming <= memory_ready;
                frame_black <= 0;
            end
            if (front_changed) begin
                if (vertical >= 10'd480) streaming <= memory_ready;
                else if (frame_ready) begin streaming <= 0; frame_black <= 1; end
            end
            if (disabled) begin streaming <= 0; frame_ready <= 0; end
            if (underflow_now || mem_error || bad_response) begin
                streaming <= 0;
                frame_black <= 1;
                if (!frame_black) underflows <= underflows + 32'd1;
            end
        end
    end
endmodule
