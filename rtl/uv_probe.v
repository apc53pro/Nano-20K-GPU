// Physical qualification harness for the arithmetic core, not a board top.
// Serial stimulus and an observable rolling checksum retain all datapath bits
// without requiring hundreds of package pins for the parallel core interface.
module nano_uv_probe(input clk,input reset,input ce,input serial_data,
    input in_valid,input out_ready,output out_valid,output serial_result,
    output out_error);
    reg [197:0] stimulus;
    reg [31:0] checksum;
    wire in_ready;
    wire signed [31:0] result_u,result_v;
    wire [5:0] result_tag;
    wire overflow,invalid_q;
    nano_uv_pipeline core(clk,reset,ce,in_valid,in_ready,
        stimulus[63:0],stimulus[127:64],stimulus[191:128],stimulus[197:192],
        out_valid,out_ready,result_u,result_v,result_tag,overflow,invalid_q);
    always @(posedge clk) begin
        if(reset)begin stimulus<=0;checksum<=0;end
        else if(ce)begin
            if(in_ready)stimulus<={stimulus[196:0],serial_data};
            if(out_valid && out_ready)
                checksum<={checksum[30:0],checksum[31]} ^ result_u ^ result_v ^
                    {24'b0,result_tag,overflow,invalid_q};
        end
    end
    assign serial_result=checksum[31];
    assign out_error=overflow|invalid_q;
endmodule
