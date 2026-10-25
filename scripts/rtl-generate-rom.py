#!/usr/bin/env python3
"""Generate the four explicitly configured native seed-ROM instances."""
import argparse
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    lines = ["// Generated from the checked reciprocal_seed_*.hex files.",
             "module nano_uv_seed_rom(input clk,input reset,input enable,input [7:0] address,",
             "output [15:0] base0,base1,base2,gradient);", "`ifdef SYNTHESIS"]
    for bank, output in enumerate(("base0", "base1", "base2", "gradient")):
        path = ROOT / "rtl/generated" / f"reciprocal_seed_{bank}.hex"
        words = [int(value, 16) for value in path.read_text().split()]
        if len(words) != 256 or any(not 0 <= word <= 65535 for word in words):
            raise ValueError(f"invalid seed image: {path}")
        words += [0] * (1024 - len(words))
        lines += [f"wire [31:0] data{bank};", "SDPB #(",
                  '.READ_MODE(1\'b0),.BIT_WIDTH_0(16),.BIT_WIDTH_1(16),',
                  '.BLK_SEL_0(3\'b0),.BLK_SEL_1(3\'b0),.RESET_MODE("SYNC"),']
        for chunk in range(64):
            value = sum(words[16 * chunk + i] << (16 * i) for i in range(16))
            comma = "," if chunk != 63 else ""
            lines.append(f".INIT_RAM_{chunk:02X}(256'h{value:064x}){comma}")
        lines += [f") bank{bank}(.CLKA(clk),.CLKB(clk),.CEA(1'b0),.CEB(enable|reset),",
                  ".RESETA(1'b0),.RESETB(reset),.OCE(1'b0),",
                  ".ADA(14'b0),.ADB({2'b0,address,4'b0}),.DI(32'b0),",
                  f".BLKSELA(3'b0),.BLKSELB(3'b0),.DO(data{bank}));",
                  f"assign {output}=data{bank}[15:0];"]
    lines += ["`else"]
    for bank, output in enumerate(("base0", "base1", "base2", "gradient")):
        lines += [f"reg [15:0] image{bank}[0:255], output{bank};",
                  f'initial $readmemh("rtl/generated/reciprocal_seed_{bank}.hex",image{bank});',
                  f"always @(posedge clk) if(reset) output{bank}<=0; else if(enable) output{bank}<=image{bank}[address];",
                  f"assign {output}=output{bank};"]
    lines += ["`endif", "endmodule", ""]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(lines))


if __name__ == "__main__":
    main()
