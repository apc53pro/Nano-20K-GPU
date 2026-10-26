`timescale 1ns/1ps
// The rasterizer's reduced-width saturating RNE converters must remain
// equivalent to a full signed-64 input / 49-bit rounded-integer reference.
module nano_tile_quantize_tb;
    nano_tile_renderer dut();
    reg signed [63:0] value;
    reg [63:0] random_bits=64'hd051c4b78a290e63;
    integer i, fraction, bucket, checked=0;
    reg [47:0] integer_part;

    function automatic [23:0] reference_quantize;
        input signed [63:0] sample;
        input [23:0] limit;
        reg [48:0] integer_value;
        begin
            integer_value={1'b0,sample[63:16]}+
                ((sample[15:0]>16'h8000)||
                 ((sample[15:0]==16'h8000)&&sample[16]));
            if(sample<=0) reference_quantize=0;
            else if(integer_value>{25'd0,limit}) reference_quantize=limit;
            else reference_quantize=integer_value[23:0];
        end
    endfunction

    task check_value;
        input signed [63:0] sample;
        reg [23:0] depth, color;
        begin
            depth=reference_quantize(sample,24'hffffff);
            color=reference_quantize(sample,24'd255);
            if(dut.quantize_depth(sample)!==depth)
                $fatal(1,"D24 RNE mismatch value=%h actual=%h expected=%h",
                       sample,dut.quantize_depth(sample),depth);
            if(dut.quantize_color(sample)!==color[7:0])
                $fatal(1,"RGB8 RNE mismatch value=%h actual=%h expected=%h",
                       sample,dut.quantize_color(sample),color[7:0]);
            checked=checked+1;
        end
    endtask

    initial begin
        // Exhaust every discarded fraction around zero, odd/even ties and
        // both representable upper bounds, including the first overflow code.
        for(bucket=0;bucket<10;bucket=bucket+1) begin
            case(bucket)
                0:integer_part=0;
                1:integer_part=1;
                2:integer_part=2;
                3:integer_part=254;
                4:integer_part=255;
                5:integer_part=256;
                6:integer_part=24'hfffffe;
                7:integer_part=24'hffffff;
                8:integer_part=25'h1000000;
                default:integer_part=48'h7fffffffffff;
            endcase
            for(fraction=0;fraction<65536;fraction=fraction+1) begin
                value=$signed({integer_part,fraction[15:0]});
                check_value(value);
                check_value(-value);
            end
        end
        check_value(64'sh8000000000000000);
        check_value(64'sh7fffffffffffffff);
        for(i=0;i<10000;i=i+1) begin
            random_bits=random_bits^(random_bits<<13);
            random_bits=random_bits^(random_bits>>7);
            random_bits=random_bits^(random_bits<<17);
            check_value($signed(random_bits));
        end
        $display("PASS %0d full-width/RNE quantizer vectors",checked);
        $finish;
    end
endmodule
