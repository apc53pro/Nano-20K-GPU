// Experimental lossless upload codec; not part of the live GPU packet ABI.
// Each record expands to 80 little-endian u32 words (320 bytes). Each group
// contains one descriptor byte then payloads for four words, lowest word first.
// There are twenty such groups, so descriptor overhead is twenty bytes total.
// Code 0 emits zero; 1 consumes/sign-extends one byte; 2 consumes/sign-extends
// two bytes; 3 consumes four raw bytes. Payload words retain their original
// order. This preserves every bit, including both halves of i64 coefficients.
//
// start is accepted only with start_ready. reset cancels a partial record.
// done pulses after the eightieth output handshake. Output and source stalls
// are independent; no source byte is consumed while an output is blocked.
module nano_upload_word_decoder (
    input wire clk, input wire reset,
    input wire start, output wire start_ready,
    input wire s_valid, output wire s_ready, input wire [7:0] s_data,
    output wire m_valid, input wire m_ready,
    output wire [31:0] m_data, output wire [6:0] m_index,
    output wire busy, output reg done
);
    localparam [1:0] IDLE=0, HEADER=1, WORD=2;
    reg [1:0] state;
    reg [7:0] descriptor;
    reg [6:0] word_index;
    reg [2:0] byte_count;
    reg [31:0] word_data;
    wire [1:0] code=descriptor[1:0];
    wire [2:0] payload_bytes=code==0 ? 3'd0 : code==1 ? 3'd1 : code==2 ? 3'd2 : 3'd4;
    assign start_ready=state==IDLE && !reset;
    assign busy=state!=IDLE;
    assign m_index=word_index;
    assign m_valid=state==WORD && byte_count==payload_bytes && !reset;
    assign s_ready=!reset && (state==HEADER || (state==WORD && byte_count!=payload_bytes));
    assign m_data=code==0 ? 32'd0 : code==1 ? {{24{word_data[7]}},word_data[7:0]} :
                  code==2 ? {{16{word_data[15]}},word_data[15:0]} : word_data;
    always @(posedge clk) begin
        if(reset) begin
            state<=IDLE; descriptor<=0; word_index<=0;
            byte_count<=0; word_data<=0; done<=0;
        end else begin
            done<=0;
            case(state)
            IDLE: if(start) begin
                state<=HEADER; word_index<=0; byte_count<=0;
            end
            HEADER: if(s_valid) begin
                descriptor<=s_data; state<=WORD;
            end
            WORD: begin
                if(s_valid && s_ready) begin
                    case(byte_count)
                    0: word_data[7:0]<=s_data;
                    1: word_data[15:8]<=s_data;
                    2: word_data[23:16]<=s_data;
                    3: word_data[31:24]<=s_data;
                    default: begin end
                    endcase
                    byte_count<=byte_count+1'b1;
                end
                if(m_valid && m_ready) begin
                    descriptor<={2'b00,descriptor[7:2]}; byte_count<=0;
                    if(word_index==79) begin state<=IDLE; done<=1; end
                    else begin
                        word_index<=word_index+1'b1;
                        if(word_index[1:0]==3) state<=HEADER;
                    end
                end
            end
            default: state<=IDLE;
            endcase
        end
    end
endmodule
