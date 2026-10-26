// Independent 640x480 RGB565 scanout. All pixels arrive through memory client
// zero and two native 1024x16 BSRAM banks. No framebuffer exists in this module.
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
    reg [31:0] response_word;
    reg response_valid, response_lane;
    reg [10:0] read_pointer, write_pointer;
    reg [11:0] stored;
    reg read_pending, read_bank;
    reg output_valid;
    reg [15:0] output_word;
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
    wire [12:0] fifo_level = {1'b0,stored} + {12'b0,read_pending} + {12'b0,output_valid};
    wire fifo_push = response_valid && !discard_response && streaming &&
        fifo_level < 13'd2048 && !flush_stream;
    wire fifo_read = !read_pending && (!output_valid || consume_pixel) && stored != 0 &&
        streaming && !flush_stream;
    wire [15:0] fifo_write_data = response_lane ? response_word[31:16] : response_word[15:0];

    nano_sdp16 fifo0 (
        .wclk(clk), .wce(1'b1), .we(fifo_push && !write_pointer[10]),
        .waddr(write_pointer[9:0]), .wdata(fifo_write_data),
        .rclk(clk), .rce(1'b1), .re(fifo_read && !read_pointer[10]),
        .raddr(read_pointer[9:0]), .reset_output(reset), .rdata(fifo0_data)
    );
    nano_sdp16 fifo1 (
        .wclk(clk), .wce(1'b1), .we(fifo_push && write_pointer[10]),
        .waddr(write_pointer[9:0]), .wdata(fifo_write_data),
        .rclk(clk), .rce(1'b1), .re(fifo_read && read_pointer[10]),
        .raddr(read_pointer[9:0]), .reset_output(reset), .rdata(fifo1_data)
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
    assign mem_r_ready = outstanding && (discard_response || !streaming || base_flush ||
        !response_valid || (response_lane && fifo_level < 13'd2048 && !base_flush));

    always @(posedge clk) begin
        if (reset) begin
            pixel <= 0; hsync <= 1; vsync <= 1; de <= 0; x <= 0; y <= 0;
            horizontal <= 0; vertical <= 0; vblank_pulse <= 0; underflows <= 0;
            seen_front <= 0; previous_memory_ready <= 0;
            streaming <= 0; frame_ready <= 0; frame_black <= 0;
            next_fetch <= 0; outstanding <= 0; discard_response <= 0; received_words <= 0;
            request_valid <= 0; request_discard <= 0; request_address <= 0;
            response_word <= 0; response_valid <= 0; response_lane <= 0;
            read_pointer <= 0; write_pointer <= 0; stored <= 0;
            read_pending <= 0; read_bank <= 0; output_valid <= 0; output_word <= 0;
        end else begin
            vblank_pulse <= 0;
            seen_front <= front_id;
            previous_memory_ready <= memory_ready;

            // Offer the next refill before the service becomes idle. Waiting
            // for mem_done to clear outstanding leaves a request bubble in
            // which a slow foreground WRITE can win every arbitration turn,
            // defeating the service's bounded scanout priority.
            // Reserve 32 pixels for the active transaction, two for a retained
            // response word, and 32 for this queued transaction. The command
            // remains stable until accepted; no extra memory port is added.
            if (!request_valid && streaming && memory_ready &&
                    next_fetch < FRAME_PIXELS && fifo_level <= 13'd1982 && !flush_stream) begin
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
            if (fifo_push) begin
                write_pointer <= write_pointer + 11'd1;
                response_lane <= !response_lane;
                if (response_lane) response_valid <= 0;
            end
            if (mem_r_valid && mem_r_ready) begin
                received_words <= received_words + 5'd1;
                if (!discard_response && streaming && !flush_stream) begin
                    response_word <= mem_r_data;
                    response_valid <= 1;
                    response_lane <= 0;
                end
            end
            if (consume_pixel && output_valid) output_valid <= 0;
            if (read_pending) begin
                output_word <= read_bank ? fifo1_data : fifo0_data;
                output_valid <= 1;
                read_pending <= 0;
            end
            if (fifo_read) begin
                read_bank <= read_pointer[10];
                read_pending <= 1;
                read_pointer <= read_pointer + 11'd1;
            end
            case ({fifo_push,fifo_read})
                2'b10: stored <= stored + 12'd1;
                2'b01: stored <= stored - 12'd1;
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
                read_pending <= 0; output_valid <= 0;
                response_valid <= 0; response_lane <= 0;
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
