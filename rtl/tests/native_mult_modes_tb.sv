`timescale 1ns/1ps

// The sign-control pins are static native configurations. Exercise all four
// modes against a deliberately wider mathematical product, including operand
// bit 17, enabled-edge latency, CE holds and synchronous reset priority.
module native_mult_modes_tb;
    reg clk=0;
    always #5 clk=~clk;
    reg reset=1,ce=0;
    reg [17:0] a=0,b=0;
    wire [35:0] actual [0:3];
    reg [35:0] expected [0:3][0:2];
    reg [17:0] corners [0:8];
    reg [31:0] random_state=32'h269ed3a5;
    integer mode,stage,checks=0,cycle,i,j;
    genvar configuration;
    generate for(configuration=0;configuration<4;configuration=configuration+1) begin: modes
        nano_mult18 #(.SIGNED_A((configuration&2)!=0),.SIGNED_B((configuration&1)!=0)) dut(
            .clk(clk),.ce(ce),.reset(reset),.a(a),.b(b),.product(actual[configuration]));
    end endgenerate

    function automatic [35:0] mathematical_product;
        input integer selected_mode;
        input [17:0] left,right;
        reg signed [18:0] signed_left,signed_right;
        reg signed [37:0] full_product;
        begin
            signed_left={(selected_mode&2)!=0?left[17]:1'b0,left};
            signed_right={(selected_mode&1)!=0?right[17]:1'b0,right};
            full_product=signed_left*signed_right;
            mathematical_product=full_product[35:0];
        end
    endfunction
    function automatic [31:0] next_random;
        input [31:0] value;
        reg [31:0] shifted;
        begin
            shifted=value^(value<<13);
            shifted=shifted^(shifted>>17);
            next_random=shifted^(shifted<<5);
        end
    endfunction

    always @(posedge clk) begin
        for(mode=0;mode<4;mode=mode+1) begin
            if(reset) begin
                for(stage=0;stage<3;stage=stage+1)expected[mode][stage]=0;
            end else if(ce) begin
                expected[mode][2]=expected[mode][1];
                expected[mode][1]=expected[mode][0];
                expected[mode][0]=mathematical_product(mode,a,b);
            end
        end
        #1;
        for(mode=0;mode<4;mode=mode+1) begin
            if(actual[mode]!==expected[mode][2])
                $fatal(1,"mode %0d: got %h expected %h, CE=%0b reset=%0b",
                    mode,actual[mode],expected[mode][2],ce,reset);
            checks=checks+1;
        end
    end

    initial begin
        corners[0]=0;corners[1]=1;corners[2]=2;
        corners[3]=18'h1ffff;corners[4]=18'h20000;
        corners[5]=18'h3fffe;corners[6]=18'h3ffff;
        corners[7]=18'h15555;corners[8]=18'h2aaaa;
        repeat(2)@(negedge clk);
        reset=0;ce=1;
        for(i=0;i<9;i=i+1)for(j=0;j<9;j=j+1)begin
            @(negedge clk);a=corners[i];b=corners[j];
        end
        for(cycle=0;cycle<20000;cycle=cycle+1)begin
            @(negedge clk);
            random_state=next_random(random_state);a=random_state[17:0];
            random_state=next_random(random_state);b=random_state[17:0];
            ce=(cycle%7!=2)&&(cycle%13!=4);
            reset=(cycle%1237==0);
            // Force reset while CE is low as well as through active traffic.
            if(reset)ce=0;
        end
        @(negedge clk);reset=0;ce=1;a=0;b=0;
        repeat(4)@(negedge clk);
        if(checks<80000)$fatal(1,"too few multiplier checks");
        $display("PASS native multiplier: all four sign modes, %0d output checks, latency/CE/reset",checks);
        $finish;
    end
endmodule
