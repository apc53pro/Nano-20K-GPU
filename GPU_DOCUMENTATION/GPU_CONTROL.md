# GPU control: CPU interface, commands and examples

This is the CPU programmer's guide for the **implemented RTL**, protocol
**N20K-SYS-0.2**. Read [GPU_ARCHITECTURE.md](GPU_ARCHITECTURE.md) for the rendering
pipeline and CPU/GPU division of work. These two files are the primary GPU
documentation and must be updated with architecture or control changes in the
same change and pushed with those changes to GitHub.

The byte protocol below is implemented and remains provisional. The
**Core2350B/Tang Nano 20K wiring allocation
uses the `N20K-PIN-0.1` Tang pin profile with the 2026-11-06 RP-side remap below**.
The bidirectional pad wrapper and exact package constraints are implemented
in `rtl/board/`. The MPC-2350 firmware uses a PIO transport for a monochrome
terminal and a native GPU service. Physical electrical margins remain
**unqualified**.
The examples implement generic packet/driver logic; their transport callbacks
must be connected to the eventual board interface or a simulator adapter.

Version **0.2 breaks the previous 0.1 byte ABI**. `WRITE_HEAP` now uses one
lossless encoded representation for every resource; no uncompressed upload
mode or legacy packet decoder remains. Request/reply version bytes and the
capability version are 0.2. Update host software together with the GPU.
Primitive records remain 320 bytes after decoding, and rendering precision,
state, buffer ownership and completion rules are unchanged. The earlier
[timing-optimization regression](../docs/benchmarks/rtl-timing-optimized.json)
describes the historical 0.1 implementation, not the new upload path.

**Hardware compatibility policy:** old host software or packet formats must
never require additional GPU hardware resources. Interface changes may break
the ABI; update the current host software, tests and both guides together
instead of retaining a resource-consuming legacy path. Compatibility with no
additional hardware cost is allowed. Current-format validation remains
required, and this policy changes none of the packet formats documented here.

Control and coordinate-buffer timing changes preserve prepared-field widths,
numeric overflow behavior, reply ordering and completion checks. Drivers still
honor backpressure and wait for replies/status; they do not adjust commands for
the buffer's internal coordinate widths or infer completion from clock counts.

An accepted SDRAM transaction now has one internal validation/startup clock
before transfer scheduling. This also delays an invalid transaction's internal
error indication by one edge. CPU packet acceptance and fence/reply ordering
are unchanged; drivers do not insert a matching delay between commands.

## 1. What the CPU sends

The CPU sends an ordered stream of packets. It uploads textures and prepared
draw records into GPU SDRAM, submits passes, waits for fences, and presents a
completed back buffer. It does not write pixel, triangle or sprite position
registers to directly manipulate persistent display objects.

Each primitive record includes its own render state. Pass descriptors, tile
tables and primitive lists also reside in GPU SDRAM. A command FIFO acceptance
is not completion: use `FENCE` or the corresponding reply/status token before
reusing active resources. Every address below is an offset into **GPU SDRAM**,
not an RP2350 pointer.

## 2. Logical parallel-bus interface

The RTL host interface is source-synchronous. The **RP2350 owns `host_clk`**;
the GPU core clock is a separate domain connected through native asynchronous
FIFOs. Simulation selects approximately 25.2 MHz host clock and 100.8 MHz core
clock. These are nominal profiles, not achieved board timings or a requirement
that every physical host clock have a fixed phase relative to the core.
The board image uses **50.4 MHz core/SDRAM**, **25.2 MHz pixels** and
**126 MHz DDR serialization**, all from the onboard 27 MHz oscillator through
one PLL and two CLKDIV5 blocks. Each physical pixel clock edge produces one
synchronized scanout event in the core, and the core's rendered RGB565 drives
HDMI at 640×480/60. The capability bank reports **50,400,000 core Hz**,
host-divider metadata 0 (independent host clock) and pixel divider 2. Default
logical-core simulation retains 100,800,000 / 4 / 4. Drivers read capabilities
rather than assuming one profile. The image contains one fixed compile-time
configuration, with no runtime clock command or hardware compatibility mode.

The PIO firmware defaults to **250 kHz HCLK during active bursts**. It idles
the clock between byte operations, so the sustained transfer rate depends on
handshakes and FIFO service. The user reported a link failure at 25 MHz on
their wired setup, and later a working 21 MHz HCLK (local firmware setting,
2026-11-07). External bus/SDRAM margins remain unqualified.
The image is built with Gowin EDA. The reasons and
the gates that could not be applied are recorded in `AGENTS.md`. Its
[complete-board report](../docs/hardware/tang20k-board.json) verifies every
package pin and meets the core/pixel/host clocks, with Gowin Fmax estimates of
56.46 / 174.90 / 80.01 MHz against 50.4 / 25.2 / 25.2 MHz. The only
negative-slack paths are reset-release paths, listed in the report. From
2026-11-06 to 2026-11-07, a 6.3 MHz bus-test image with a constant-black
raster replaced this profile during bus diagnosis; it is historical.

| Signal | Direction at GPU | Current logical meaning |
| --- | --- | --- |
| `host_clk` | Input | Host transfer clock; active rising edge. |
| `host_wr_valid` | Input | A write byte is offered. |
| `host_wr_data[7:0]` | Input | Offered byte. |
| `host_wr_ready` | Output | Offered write will be accepted at the rising edge. |
| `host_rd_req` | Input | Request a response/register byte. |
| `host_reg_addr[6:0]` | Input | Logical stream or read-only bank address. |
| `host_rd_data[7:0]` | Output | Registered returned byte. |
| `host_rd_valid` | Output | Returned byte is valid after the host edge. |
| `abort_request` | Input to core | Out-of-band soft recovery request; internally synchronized. |
| `reset` | Input to core | Hard reset and initialization. |

The separate read/write data ports and seven address bits describe the current
RTL boundary. The physical wiring reservation below shares eight data wires
and assigns an address-latch control. `tang20k_top.sv` bridges
the two interfaces. The logical port table is not itself a physical pin map.

### Fixed wiring layout: Core2350B to Tang Nano 20K

**Tang profile `N20K-PIN-0.1`, RP wiring revision 2026-11-06:
16 signal wires plus common ground, all at 3.3 V.**
This allocation is for the **Waveshare Core2350B with 16 MiB flash and 8 MiB
PSRAM**, confirmed by the user, and **Tang Nano 20K hardware 3921**, referenced
to Sipeed schematic revision 1.30. The board top and CST implement the Tang
endpoints; the RP-side remap changes no FPGA package pin or constraint.
The MPC-2350 firmware implements this RP mapping with PIO. Use the full-board
programming image. Physical hardware qualification remains pending.

The uploaded `resource_development.7z` confirms HSYNC on GP16, VSYNC on GP17
and eight RGB outputs on GP18–GP25. Reuse **all ten VGA GPIOs**, with **GP35–GP40**
as the six additional signals. The user reports GP10–GP15 unavailable and
supplied this replacement mapping in `updated_wiring.txt`: GP15 → GP38,
GP14 → GP37, GP13 → GP36, GP12 → GP35, GP10 → GP39 and GP11 → GP40.
PIO2 uses GPIO base 16, which maps GP16–GP47 into the state machine's window.
Side-set drives HCLK/WR_N on GP16/GP17. The SET pin group drives ALE_N/RD_N on
GP37/GP38. The data group starts at GP18 and occupies eight pins. WREADY and
RVALID remain inputs on GP39/GP40.

- [Printable A4 wiring and both board-header layouts](../docs/images/core2350b-tang-nano20k-wiring.pdf)
- [Point-to-point layout](../docs/images/core2350b-tang-nano20k-wiring.svg),
  [Tang header locator](../docs/images/tang-nano20k-header-map.svg) and
  [Core2350B header locator](../docs/images/core2350b-header-map.svg)
- [Pin allocation, resistor values and source fingerprints](../docs/hardware/rp2350-tang20k-pinout.json)

`P4.12` means **contact 12 of Core2350B connector P4**, which carries GP39. Likewise,
`J6.8` is Tang connector J6 contact 8; its FPGA package pin is 27. These are
manufacturer connector positions, not Raspberry Pi Pico/Pico 2 header numbers.

| Signal | RP GPIO | Core2350B contact | Tang contact | FPGA QN88 pin | Direction | Previous VGA use |
| --- | ---: | --- | --- | ---: | --- | --- |
| WREADY | GP39 | P4.12 | J6.1 | 73 | Tang → RP | — |
| RVALID | GP40 | P4.11 | J6.2 | 74 | Tang → RP | — |
| ABORT | GP35 | P4.8 | J5.17 | 72 | RP → Tang | — |
| GPU_RESET_N | GP36 | P4.7 | J5.18 | 71 | RP → Tang | — |
| ALE_N | GP37 | P4.10 | J6.15 | 17 | RP → Tang | — |
| RD_N | GP38 | P4.9 | J6.7 | 16 | RP → Tang | — |
| HCLK | GP16 | P3.4 | J6.5 | 77 | RP → Tang | HSYNC |
| WR_N | GP17 | P3.3 | J6.6 | 15 | RP → Tang | VSYNC |
| D0 | GP18 | P3.6 | J6.8 | 27 | Bidirectional | RGB bit 0 |
| D1 | GP19 | P3.5 | J6.9 | 28 | Bidirectional | RGB bit 1 |
| D2 | GP20 | P3.8 | J6.12 | 29 | Bidirectional | RGB bit 2 |
| D3 | GP21 | P3.7 | J6.13 | 30 | Bidirectional | RGB bit 3 |
| D4 | GP22 | P3.10 | J6.14 | 31 | Bidirectional | RGB bit 4 |
| D5 | GP23 | P3.9 | J5.5 | 42 | Bidirectional | RGB bit 5 |
| D6 | GP24 | P3.12 | J5.6 | 41 | Bidirectional | RGB bit 6 |
| D7 | GP25 | P3.11 | J5.10 | 48 | Bidirectional | RGB bit 7 |

Connect Core **P1.1 GND → Tang J5.2 GND**, and **P3.1 GND → Tang J6.20 GND**.
Also bond **Tang J5.15 GND** to that same ground; it may branch from P3.1.
Keep a ground return alongside the clock and data bundle. The layout does not
join the boards' 3.3 V or 5 V outputs. Power each board through its normal power
connection. Tang **J6.19 3.3 V** is the local rail for the recommended pull-ups
below; it does not connect to the RP board's 3.3 V output.

#### Header orientation

View **both boards from the component side**, not the solder side:

- **Tang:** USB-C at the top, HDMI at the bottom. J6 is the left 1×20 header;
  J5 is the right 1×20 header. Each runs from contact 1 at the USB end to
  contact 20 at the HDMI end. **J5.1 is 5 V**, not ground.
- **Core2350B:** the USB FPC connector is at the top. P1 is left, P2 top, P3
  bottom and P4 right. On P1, the outer column is odd-numbered top to bottom
  and the inner column even-numbered. On P4, the inner column is odd-numbered
  top to bottom and the outer column even-numbered; P4.1 is ground.
  On P3, the outer/bottom row is odd-numbered
  left to right and the inner/top row even-numbered. P1.1 and P3.1 are ground.

The printable locator pages mark every P3/P4 and J5/J6 contact, including
unused positions, and identify the separate P1.1 ground connection. GP10–GP15
and their old contacts are no longer GPU signal endpoints. Verify that the
received Tang board matches the referenced
3921 schematic and orientation before following the layout.

#### Resistors, idle levels and board connections

**Current user-reported assembly:** one **100 Ω series resistor on each of
the 16 signal wires**, including one on each bidirectional data wire. The end
placement has not been reported. This replaces the earlier proposed two-resistor
data arrangement. The user has now fitted **separate 4.7 kΩ pull-downs from
RP GPIO39/WREADY and GPIO40/RVALID to common GND**. Capability reads remain
all zero. Other external-bias fitment has not been separately verified.
Series resistance does not establish an idle LOW or HIGH level. These are
prototype values; signal levels and allowable clock rate remain unqualified.

The **recommended external 4.7 kΩ bias** is listed below. Only the two
handshake pull-downs are confirmed fitted; the other entries are design
recommendations/defaults, not claims of installed parts:

| Signals | Pull | Power-on/default meaning |
| --- | --- | --- |
| WR_N, RD_N, ALE_N | To Tang J6.19, 3.3 V | HIGH; inactive |
| HCLK, ABORT | To common GND | LOW; stopped clock and no abort |
| GPU_RESET_N | To common GND | LOW; GPU logic held in reset until RP releases it |
| WREADY, RVALID | To common GND | LOW while FPGA outputs are not configured |
| D0–D7 | No external data bias specified | Drivers release outside transfers; idle voltage is undefined, including possible A2 E9 effects |

High impedance describes the output drivers, not a guaranteed wire voltage:
the FPGA can bias data pins with weak pull-ups during configuration, and A2
RP2350 inputs have the E9 leakage described below. The recommended defaults
apply after external bias is fitted and supplies exist. The confirmed series
parts count is **16 × 100 Ω**, plus the **2 × 4.7 kΩ** handshake pull-downs.
The complete recommended control-bias count is **8 ×
4.7 kΩ** (three pull-ups and five pull-downs), separate from the series parts.

The user confirms **A2 silicon**. RP2350-E9 can source leakage into an input
in the undefined voltage region and hold a weakly driven pad near 2.2 V.
The datasheet recommends a low-impedance driver or external pull-down of
8.2 kΩ or less where needed. A series 100 Ω resistor is not a pull-down;
a functioning push-pull FPGA output should overcome this leakage during a
valid read. The all-zero/all-one capability responses do not establish E9
as the cause. [RP2350 datasheet, RP2350-E9](https://datasheets.raspberrypi.com/rp2350/rp2350-datasheet.pdf)

The first E9 test added **4.7 kΩ from RP GPIO39/WREADY to common GND and
4.7 kΩ from RP GPIO40/RVALID to common GND**, retaining the existing 100 Ω
series resistors. Both are now user-confirmed fitted, and capability reads
still return all zero. This does not establish E9 as the root cause. A held,
known read and pad-voltage measurement are needed to distinguish a weak/high-
impedance line from incorrect active readback. Changing to `digitalRead()` does not
remove the hardware leakage; Arduino-Pico applies no forced universal E9
workaround. [Arduino-Pico E9 notes](https://arduino-pico.readthedocs.io/en/stable/rp2350.html)

The three active-low controls deliberately use Tang **LED0/1/2** pins 15/16/17.
Each has an existing **3.3 V → orange LED → 510 Ω → signal** branch. LOW lights
the corresponding LED and adds sink current to the RP output; HIGH turns it
off. The driver must select **at least 8 mA GPIO drive strength** for
these three outputs; the initial firmware selects **12 mA**. Ignoring LED forward
voltage gives a conservative nominal bound of **7.18 mA per asserted line**
including the recommended 4.7 kΩ pull-up and ignoring the series resistance.
The LED branch does not replace a real pull-up. Verify the FPGA-side logic
levels with the current 100 Ω series parts. Configure these FPGA pins strictly
as inputs.

**Leave the Tang RGB LCD FPC connector disconnected.** Data pins and HCLK also
route to that connector. This allocation avoids HDMI CEC/HPD on FPGA pins 25/26,
DDC on 52/53, the LCD backlight enable on 49, and BL616-driven signals. It keeps
the Tang flash, SDRAM, SD slot, audio and programming connections available.
FPGA pins 41/42 are **bank 4**, despite the bank-5 color in Sipeed's pinout
picture; the Gowin package table and Sipeed power schematic establish their
bank and 3.3 V supply. The selected banks 1/3/4/5/6 are all 3.3 V on this board.
HCLK uses **77/GCLKT_1**, a suitable single-ended global-clock input.

Preserve RP **GP0** (startup indicator), **GP2–GP5** (SD SPI), **GP6–GP8**
(I2S audio), **GP27/GP28** (PS/2) and **GP47**
(PSRAM chip select). GP1/GP9/GP26/GP29 are not allocated by this wiring plan.
GP10–GP15 are unavailable for this bus, rather than spare GPU connections.
**GP39 is now WREADY, not a separate LED output.** The Core schematic connects
GP39 → R10 470 Ω → red LED1 → GND. A HIGH WREADY therefore lights the LED and
loads the Tang output. This is a branch to ground, not a series resistor in
the GP39 connection; continuity along that connection can still read nearly
zero ohms. Keep RP GP39 configured as an input and disable any
independent onboard-LED output code. Include this additional source-current
load and its effect on WREADY levels/edges in board validation; no electrical
margin or tested host clock is established by this documentation update.
The existing VGA resistor DAC and sync circuitry must be disconnected from
GP16–GP25; connect at the GPIO-side nodes, not the VGA connector's combined
analog R/G/B pins.

Assemble with power off. The previous VGA firmware and the Tang's factory
image do not implement this bus or guarantee its pin directions/reset rules.
The factory image can drive LED/control and LCD-clock pins as well as data.
Use a removable signal connector or jumpers and keep **all 16 signal links
isolated during powered operation until compatible firmware and an FPGA
board image are available**. Series resistors do not make conflicting
push-pull outputs safe. Do not drive an unpowered board through its GPIOs.
All signal voltages are 3.3 V; no 5 V signal connects to this interface.

#### Implemented physical pad wrapper

These roles are implemented by `rtl/board/tang20k_top.sv`. The packet bytes
below are the same ones used by the RP2350 PIO transport.

HCLK is generated by the RP. WR_N offers an address or packet byte; WREADY is
its pre-rising-edge acceptance indication. With ALE_N LOW, a write latches
D[6:0] as the native register-bank address, with D7 zero, without adding a
packet byte. With ALE_N HIGH, a write appends a packet byte at selected address
0. Address-latch cycles need their own WREADY acceptance independent of native
`host_wr_ready`: native readiness is LOW at nonzero bank addresses and cannot
otherwise permit selecting bank 0 again. The selected address resets to 0 and
stays latched until another address write or reset.

RD_N selects read ownership of D[7:0]. The RP releases all eight data outputs
before asserting RD_N; only the FPGA drives during a read. RVALID marks the
registered returned byte. WR_N and RD_N must never be LOW together; ALE_N is
HIGH for reads. Insert an idle turnaround phase with both strobes HIGH and
both drivers released before reversing direction. An asserted read can consume
a reply byte on each valid host edge, so single-byte reads must end before a
second acceptance. Exact setup/hold and turnaround durations remain future
board-timing work, not values established by this layout.

ABORT is an active-HIGH soft-recovery request to the existing core semantics;
GPU_RESET_N is an active-LOW **GPU logic reset**, not Core RUN, Tang RECONFIG_N
or a configuration/programming button. GPU_RESET_N, PLL unlock and the board reset path assert the core reset;
release is synchronized in core, host and pixel domains. The wrapper releases
the data bus and suppresses handshakes during reset. GPU_RESET_N is pulled
LOW, so an unconnected board remains in GPU reset until an external host or
isolated bring-up jumper drives it HIGH. Both clocks must continue for
soft recovery. Reply/status polling uses the existing logical banks; no IRQ,
separate direction signal or seven extra address wires are reserved.

#### Pinout evidence

The user's `updated_wiring.txt` supplies the six revised RP GPIO-to-Tang
package-pin connections. The Core manufacturer schematic verifies the P4
contacts in the table and the GP39 LED load. The dated RP-side revision does
not change the Tang `N20K-PIN-0.1` allocation, packet ABI or FPGA image inputs.

The source archive is retained as reference metadata and hashes in the pin
allocation JSON; its firmware files are not copied into this repository.
Pin definitions are at `globals.h:69–70`, `resource_development.ino:233` and
`rgb.pio.h:66,75–84`. The peripheral audit also checked `SDmgr.cpp`,
`audio_runtime.cpp` and the PS/2 assignments.

Manufacturer sources: [Core2350B schematic](https://files.waveshare.com/wiki/Core2350B0/Core2350B.pdf),
[Core component orientation](https://www.waveshare.com/img/devkit/Core2350B0/Core2350B0-details-intro.jpg),
[Tang schematic rev 1.30](https://dl.sipeed.com/fileList/TANG/Nano_20K/2_Schematic/Tang_Nano_20K_3921_Schematics.pdf)
(PDF pages 2, 3, 5 and 6),
[Tang datasheet v1.3](https://dl.sipeed.com/fileList/TANG/Nano_20K/1_Datasheet/Sipeed%20Tang%20nano%2020K%20Datasheet%20V1.3-en_US.pdf)
(printed page 5), [Gowin UG115](https://cdn.gowinsemi.com.cn/UG115E.pdf)
(QN88 pins/banks and GCLKT/GCLKC distinction),
[Gowin UG290](https://cdn.gowinsemi.com.cn/UG290E.pdf)
(Table A-3, GPIO boot states), and
[RP2350 datasheet](https://datasheets.raspberrypi.com/rp2350/rp2350-datasheet.pdf)
(Table 1683, output drive and current limits).

### MPC-2350 GPU driver

`gpu_bus.pio` defines the byte transport, `gpu_display.cpp` controls it, and
`nano_gpu.cpp` serializes N20K-SYS-0.2 packets and issues terminal commands.
`font.cpp` supplies the bitmap font for the **40×30 black-on-white terminal**.
The FPGA copies each 16×16 glyph and scans out the front buffer over HDMI.
See the [MPC-2350 firmware](https://github.com/apc53pro/MPC-2350/tree/6b4ebda7e0bffb24ea81aead5970b014f4146e2c).

The CPU submits each byte operation through the TX FIFO of one PIO2 state
machine and waits for its completion in the RX FIFO. PIO controls the clock,
strobes and D[7:0] direction. It samples WREADY before the write edge. While
WREADY is LOW, it advances HCLK so the host domain can progress. The FPGA
accepts the write only when WREADY is HIGH. For reads,
it releases D[7:0] before asserting RD_N, samples RVALID and data after the
rising edge, and releases RD_N before another rising edge. PIO returns the
pre-edge RVALID sample and the read-edge count for diagnostics. The CPU
handles packet encoding, deadlines and reset, and uses GPIO for startup and
offline static measurements. RP control outputs use 12 mA drive.
Read retries use seven PIO cycles high and five low. Write retries use six
cycles per phase. Edit only `kHostClockHz` in `gpu_display.cpp` and rebuild
the firmware to change the burst rate. The driver rounds the target period
up to whole RP system-clock ticks and then to a supported PIO divider.
The 250 kHz default gives a 4 µs period at the firmware's 375 MHz system
clock. `GPU_STATUS` prints the requested rate as `hclk_target_hz=`.
Manual reset and offline diagnostic phases round up to whole microseconds
using the same setting.
The setting describes HCLK bursts. Electrical timing at the pads still
needs measurement.

Startup asserts GPU_RESET_N and keeps the data bus released for **1000 ms**
before any host clock/transfer, allowing for FPGA flash configuration. This is
a minimum allowance, not a configuration-DONE detector. It then clocks
the host domain during reset and release, and checks the immutable sideband
capability prefix `0 2 8 20` before interpreting status. An unconfigured FPGA
reads all `FF` or times out. While the prefix is wrong, the driver releases the
bus, waits 100 ms and repeats the reset pulse and prefix check. It stops at the
first match or **10 s** after initialization began; without a PIO state machine
it stops at once. A cold power-up that configures after the allowance therefore
still connects (host-mock tested; on 2026-11-08 the user reported the first
boot after a full power cycle connecting on the board). The firmware
starts this sequence only after its final system-clock change, because the HCLK
divider is derived from `clk_sys`. It then polls coherent startup
status, checks the full 0.2 capabilities, and uploads a **64 KiB
256×128 RGB565 font texture at heap `0x258000`**, and fences. The CPU expands
immutable font bits into one 512-byte upload row at a time; it allocates no
screen pixel buffer. The first frame uses 1200 heap-image `COPY_RECT` commands.
The client tracks which characters each colour buffer shows. For a changed
frame it picks the cheapest way to bring the back buffer up to date. Either it
draws only the glyphs that differ from what that buffer already holds, or it
first sends one `COPY_RECT` of the front, shifted up by whole 16-pixel rows for
a scroll, and then draws the rest. A copy counts as 16 glyphs when the options
are compared. The terminal also uploads a 6×9 RGB565 pointer at `0x268000` and
copies each opaque row span to the back buffer. It restores glyphs covered by
the previous pointer, including coverage inherited from a shifted front
copy. It then fences, presents the back and waits for the matching
vertical-blank reply before reusing buffer ownership. No-change frames send
no packets. The terminal uses the copy engine and scanout.

Each physical byte transfer has a **250 ms timeout**; startup polling also has
a finite count. A reply exists only after every earlier command retires, so its
first byte can lag far behind one byte timeout. Before reading a reply, the
client polls sideband register 1 until bit 4 (reply available) is set, within a
**10 s** CPU-clock deadline. It stops at once on bit 3 (core fault) or bit 7
(transport fault), because the RTL emits no reply after a fault. The errors are
`reply wait timeout` and `GPU fault while awaiting reply`. `GPU_STATUS` reports
`reply_wait_ms=last/max`. `GPU_REGS` decodes two live status snapshots 500 ms
apart (flags, epoch, completed, fence and present tokens, cycle counter, fault
codes) and runs automatically after a failed frame. Invalid replies, mismatched epoch/sequence/token, or transfer
failure latch the client offline. For normal initialization, packet and probe
transfers, the board glue releases data, drives
WR_N/RD_N/ALE_N HIGH and **keeps GPU_RESET_N HIGH**. A host error must not
hold the video pipeline in reset; the earlier firmware's LOW-on-error path
also stopped HDMI video and has been corrected. Only explicit startup,
`GPU_RESET` or the offline `GPU_LATCHTEST` diagnostic asserts GPU_RESET_N LOW
before releasing it HIGH again.
It does not retry a partial packet or use soft ABORT in this first
version. USB Serial at 115200 accepts `GPU_STATUS` and `GPU_RESET`; the latter
hard-resets the GPU, starts a fresh sequence/epoch session, reuploads the font
and redraws the terminal. Remove any Tang-only reset-release jumper before
connecting the RP reset output. Terminal messages are mirrored to USB Serial.
`GPU_STATUS` also prints the observed RESET_N/WREADY/RVALID GPIO levels and
the saved startup flags, core fault code and transport fault code (`GPU BOOT`).
Those fault fields are the last startup snapshot, not a live status refresh.
If the capability prefix fails before status polling, `GPU BOOT` remains
zero-initialized and unread; those zeros do not mean the GPU reported no fault.
An all-`FF` snapshot is rejected as `all-ones startup readback`; its numeric
fault fields must not be interpreted as a decoded GPU error. `GPU ADDRESS`
shows the last address byte driven and sensed at the RP data pads. Equal
values check the local output level, not inter-board continuity.
For an offline link, `GPU_PROBE` reads immutable capability bytes 64..79 through
the sideband path without submitting/retrying packets. Its expected hexadecimal
prefix is `0 2 8 20` (version 0.2, 8-bit bus, 32-pixel tiles). All-ones responses
point to invalid readback and require checking data/control wiring and bus drive.
An all-zero or otherwise incorrect capability prefix reports `invalid capability
readback` rather than being treated as an SDRAM initialization timeout: these
bytes are constants independent of initialization.
The probe also runs automatically once after a failed startup so its output is
available even with a locally disabled serial command loop.
Sixteen printed probe bytes mean the RP sampled WREADY and RVALID HIGH during
the transactions. The diagnostic's later idle WREADY=0/RVALID=0 values do not
mean those inputs stayed LOW during the probe.

The offline-only `GPU_SWAPTEST` runs after a completed sixteen-byte capability
sweep in `GPU_PROBE` (including its automatic run after failed startup), and
can also be requested over USB Serial. A capability-transfer timeout skips this
automatic test. It first reads sideband register `0x01` to refresh the status
snapshot, then reads requested addresses **`0x02`, `0x21`, `0x23`**. It prints
`GPU SWAPTEST: <three HEX bytes>; flags=0x<raw flags>; edges=<a b c d>; pre=<a b c d>`
without decoding or classifying the result. It submits no packet and refuses
while the client is online or a read is held; use `GPU_RELEASE` before running
it after `GPU_HOLD`. Completion or a timeout releases the bus; a timeout
identifies the register.

After a fresh hard reset, epoch is 1. Status bytes 33 and 35 are upper bytes
of the fault code, which `nano_gpu_core` always reports as `{24'd0, fault_code}`:

| Wiring hypothesis | Expected `GPU SWAPTEST` bytes | Reason |
| --- | --- | --- |
| Correct D5/D6 order | `1 0 0` | Status byte 2 is epoch's low byte, 1; status bytes 33 and 35 are zero. |
| Pure D5/D6 interchange in both address and return data | `1 2 40` | `0x02` is unchanged; `0x21` selects `0x41` (`0x02`); `0x23` selects `0x43` (`0x20`), returned as `0x40`. |

The latter pattern strongly supports that interchange under these assumptions;
it is not a unique fault diagnosis. `1 0 0` does not verify every bus line.
Under an interchange, the raw flags also arrive with bits 5 and 6 exchanged.

`edges` gives, in decimal for registers `0x01 0x02 0x21 0x23`, the HCLK rising
edges after RD_N went LOW until the RP sampled RVALID HIGH. `pre` is RVALID
sampled once with RD_N LOW and HCLK LOW, before the first such edge.

| Handshake observation | Interpretation |
| --- | --- |
| `edges=4 1 1 1; pre=0 0 0 0` | Genuine handshake. A register-`0x01` request toggles a snapshot request that the core clock acknowledges; the result returns after the fourth HCLK edge in an Icarus simulation of `nano_host_link`. Capability reads and later status reads answer on the first edge. |
| Register-`0x01` count `1` | RVALID did not come from the snapshot handshake (for example, RVALID following HCLK), or the FPGA latched another address such as a capability register. |
| Any `pre` value `1` | RVALID was HIGH before any read edge requested data. |

`tang20k_top` drives `gpu_d` and `gpu_rvalid` under the same `reading`
condition, so genuine handshake counts with zero data point at the data wires
or their Tang endpoints. These are diagnostic expectations, not measured
electrical margins.

**Reported 2026-11-06, before edge counting existed:** `GPU CAPS` showed sixteen
zero bytes and `GPU SWAPTEST: 0 0 0; flags=0x0`, with no timeouts. This matches
neither table row. Register `0x02` uses neither D5 nor D6, so a pure interchange
still returns `1`. Every byte read was zero, including capability bytes `0x02`,
`0x08` and `0x20`. A D5/D6 interchange alone cannot explain this result. In
the startup prefix check, register 64's expected zero can pass; register **65**
is the first mismatch when it returns zero instead of `0x02`.

**Reported 2026-11-06 with edge counting**, after an RP-only reset with the
FPGA already configured: `edges=1 1 1 1; pre=0 0 0 0`, bytes `0 0 0`, and a
`GPU_HOLD` idle check of `0/0`. During a confirmed hold (`held=1`, RVALID=1,
WREADY=0 at the RP), the user measured at the Tang:

| Point | Measured | Expected for register `0x41` |
| --- | --- | --- |
| 3V3 | 3.3 V | 3.3 V |
| Pin 74 RVALID / pin 73 WREADY | 3.3 V / 0 V | HIGH / LOW |
| Pin 16 RD_N | 0.23 V | LOW (pin 16 also sinks onboard LED1 current through the series resistor) |
| Data pins 27, 28, 29, 30, 31, 42, 41, 48 | all 0 V | pin 28 (D1) HIGH, others LOW |
| RP GPIO19 (D1) | 0.05 V | HIGH |

The GPU therefore answered and drove `0x00` where register `0x41` holds
`0x02`. Together with the one-edge register-`0x01` answer, this indicates that
the FPGA latched a different address, in the immediately answered capability
range. That is an inference, not an isolated cause. The `build/tang20k-slow`
image and the flashed `bitstreams/tangnano20k/nano-gpu.fs` have the same SHA-256
(`ede2657f…`), and its routed ports are on the constrained package pins.
A cold power-up first printed all `FF` with idle WREADY/RVALID HIGH and
`pre=1 1 1 1`, because configuration had not finished within the 1000 ms
allowance. That is a separate startup-timing observation.

During an address write, the RP accepts once WREADY is HIGH, and
`tang20k_top` drives WREADY as `!gpu_d[7]`. Acceptance therefore cannot reveal
a missed or incorrect latch. Two further offline host diagnostics isolate the
address path without an FPGA change:

- **`GPU_LATCHTEST`** pulses reset exactly like startup (32 HCLK edges with
  GPU_RESET_N LOW, release, 64 idle edges), without the configuration wait or
  client startup. It then reads capability registers **`0x41 0x42 0x43 0x41`**,
  never `0x40` first, and prints
  `GPU LATCHTEST: <4 HEX bytes>; edges=<4 decimal>; expected=2 8 20 2`.
  `2 8 20 2` means that these address writes reached the latch. `2 2 2 2`
  matches latching only the first address after reset. `0 0 0 0` matches the
  FPGA seeing other addresses, for example a fixed `0x40`. These signatures
  come from the host model and are not unique diagnoses.
- **`GPU_DRIVE <hex>`** (one or two hexadecimal digits) releases the bus,
  drives the byte on GPIO18..25 and holds an **address phase without its
  rising edge**: ALE_N and WR_N LOW, RD_N HIGH, HCLK LOW, reset HIGH. No edge
  means nothing is latched. With RD_N HIGH, the `reading` condition is false,
  so the FPGA leaves `gpu_d` released. Measure the Tang data pins, WR_N pin 15
  and ALE_N pin 17 (both LOW), RD_N pin 16 (HIGH), HCLK pin 77 (LOW) and WREADY
  pin 73. WREADY reports the inverse of the D7 level that the FPGA sees. `55`
  then `2A` exercise each address bit HIGH and LOW. `GPU BUS` reports
  `drive=1`. `GPU_RELEASE` raises the strobes before releasing data, still
  without a rising edge; `GPU_RESET` also ends the hold.

Both diagnostics submit no packet and refuse while the client is online.
`GPU_PROBE`, `GPU_SWAPTEST` and `GPU_LATCHTEST` refuse while a `GPU_HOLD` or
`GPU_DRIVE` hold is active.

#### Address-path isolation, 2026-11-07

User-reported results on the received board, in order:

| Test | Image | Result | Conclusion |
| --- | --- | --- | --- |
| `GPU_LATCHTEST` | open-flow `nano-gpu.fs` | `0 0 0 0; edges=4 1 1 1` | First read used the status path: the FPGA latched a status-range address, not `0x41` |
| `GPU_DRIVE 55` / `2A` at Tang pins 27, 28, 29, 30, 31, 42, 41, 48 | open-flow | Every bit 3.3 V or 0 V as driven; WREADY pin 73 3.25 V | Each RP→Tang data bit arrives, HIGH and LOW, at its constrained pin; no contention |
| Gate-level simulation of the synthesized netlist, firmware timing | open-flow synthesis | Prefix `00 02 08 20`, `GPU_LATCHTEST` `2 8 20 2`, register 1 after 4 edges | RTL and synthesis are correct |
| Bus-echo diagnostic image (`experiments/bus-echo`) | open flow, same pin sites | Probe `40`…`4F`, swap test `2 21 23`, `GPU_LATCHTEST` `41 42 43 41`, held `0x41` with pins 27/41 HIGH | Pins, FPGA inputs, address latch rule and FPGA→RP HIGH/LOW levels work |
| Unchanged `tang20k_top` RTL built by Gowin EDA (6.3 MHz profile at the time) | Gowin V1.9.8.11 Education | `READY`; `GPU_LATCHTEST` `2 8 20 2`; caps `0 2 8 20 0 0 80 0 0 80 25 0 0 4 0 0`; swap test `1 0 0; flags=0x1; edges=4 1 1 1` | The full design works when built by the vendor flow |

The fault is therefore after synthesis in the open flow, in nextpnr packing or
placement, or in Apicula bitstream generation, for this full design. It is not
yet isolated to one stage; the routed netlist's latch logic reads correctly.
A gate-level simulation of the decoded bitstream was attempted. It cannot settle
the question, because `gowin_unpack` output leaves the global clock spines
unconnected, so even the working echo bitstream fails there.

That A/B build used an unverified third-party copy of Gowin EDA, from a
mega.nz download with no Authenticode signature. Board images are now built
with it by `make tang20k`, under the `AGENTS.md` exception. The image was
programmed with the signed official Gowin Programmer V1.9.12.04
(`programmer_cli -r 9`, external flash, verified).
After `READY`, the first terminal frame completed (present 1332). The second
frame then failed with `read timeout` while the client awaited its fence
(sequence 1336). `GPU_REGS` showed `busy`, `queue-full`, `completed=1332`, no
fault codes and the cycle counter advancing about 6.3 million counts per second.
A later dump showed `completed=1336`, `fence=1336` and a reply available. The
second frame's 640×480 front-to-back `COPY_RECT` therefore finished, but it
took far longer than the old 250 ms reply-byte wait at the 6.3 MHz bus-test
clock. That was at least about 0.8 s, against roughly 218 ms (1.37 M cycles)
predicted by the C++ copy model with that image's SDRAM parameters. The RTL
then moved every pixel with a separate one-word read and a separate masked
write, which the model did not reproduce. The status-polled reply wait above
replaced that wait. Copies now use SDRAM bursts, and the terminal no longer
copies the whole screen per frame. Gowin's installer bound the Tang debugger's FTDI
interfaces to FTDI's driver, so openFPGALoader reported `device not found` until
a WinUSB-type driver is restored. Gowin Programmer uses the FTDI driver
directly; its read-back verify after an SRAM load fails because Gowin
bitstreams enable read-back security (status `0x6020`).

The mapping is D5 **GP23 / Core P3.9 → Tang J5.5 / FPGA pin 42**, D6
**GP24 / Core P3.12 → Tang J5.6 / FPGA pin 41**. PIO uses the contiguous
data group starting at GPIO18. A matching address echo checks the local RP pads and
cannot verify that these two wires reach their intended Tang endpoints. This
host diagnostic changes no FPGA RTL, constraints, clock or programming image.

For stable measurements, the offline-only `GPU_HOLD` command first releases
the bus with WR_N/RD_N/ALE_N HIGH and RP data pins as inputs. Before selecting
an address, it samples RVALID after one HCLK phase (2 us) with HCLK LOW and
after one phase with HCLK HIGH. `GPU IDLE` prints `RVALID_LOWCLK` and `RVALID_HIGHCLK`; both should
be **0** because RD_N is inactive. A clock-linked RVALID miswire is one possible
cause of an incorrect idle result, not an established diagnosis. The check
adds one idle rising edge, submits no packet and continues to the held read
even if idle RVALID is wrong.

It freshly selects register **65 / `0x41`**, whose immutable expected
byte is **`0x02`**. It releases all RP data outputs, asserts RD_N LOW with
WR_N/ALE_N HIGH, and clocks until RVALID is sampled HIGH or the 250 ms read
timeout expires. It then **keeps HCLK HIGH and RD_N LOW**, including when the
data is zero or RVALID timed out. An address-write/WREADY timeout instead
releases the bus. `GPU HOLD` reports the expected byte and result; `GPU BUS`
reports the held flag and RP-side HCLK/WR_N/RD_N/ALE_N/ABORT levels. These are
RP pad readings, not measurements at the Tang end of the wires.

`GPU_STATUS` refreshes the held data without adding clock edges. `GPU_PROBE`,
`GPU_SWAPTEST` and `GPU_LATCHTEST` refuse while a read is held. Use `GPU_RELEASE` to end the
read, release data
and return to inactive strobes with reset HIGH, or `GPU_RESET` to clear the
hold and perform the normal hard-initialization sequence. The
hold sends no packet, retries no partial packet and leaves the client offline;
it is not a rendering mode or an FPGA implementation change.

With `GPU_HOLD` active, measure against common GND at these nodes. The table
describes a correctly operating held read, not the user's measured voltages:

| Signal | Tang measurement point | RP measurement point | Expected level |
| --- | --- | --- | --- |
| D1 | J6.9 / FPGA pin 28 | P3.5 / GPIO19 | HIGH, approximately 3.3 V |
| D0, D2–D7 | Existing data-pin table above | GPIO18, GPIO20–GPIO25 | LOW |
| GPU_RESET_N | J5.18 / FPGA pin 71 | P4.7 / GPIO36 | HIGH |
| HCLK | J6.5 / FPGA pin 77 | P3.4 / GPIO16 | HIGH |
| WR_N | J6.6 / FPGA pin 15 | P3.3 / GPIO17 | HIGH |
| ALE_N | J6.15 / FPGA pin 17 | P4.10 / GPIO37 | HIGH |
| RD_N | J6.7 / FPGA pin 16 | P4.9 / GPIO38 | LOW |
| ABORT | J5.17 / FPGA pin 72 | P4.8 / GPIO35 | LOW |
| WREADY | J6.1 / FPGA pin 73 | P4.12 / GPIO39 | LOW during the held read |
| RVALID | J6.2 / FPGA pin 74 | P4.11 / GPIO40 | HIGH after a valid response |

Start with D1 on **both sides of its existing 100 Ω series resistor**, then
check the Tang-side control levels. Keep the two fitted handshake pull-downs.
The held measurement avoids averaging brief read pulses on a multimeter.
The earlier measurements above apply to the open-flow diagnostic session.

`GPU_RESET` repeats the one-second allowance and the 10 s prefix retry. If the
Tang is powered or reprogrammed later than that, while the RP is already
running, wait for configuration to finish and issue `GPU_RESET` or restart the
RP before expecting a connection.
At Tang J5.18 / FPGA pin 71, approximately 3.3 V releases GPU reset; 0 V asserts
it. A `GPU startup fault` message alone does not distinguish a real core fault
from incorrect bus readback; inspect the numeric snapshot and physical levels.

The firmware host regression uses the clocked **C++ software oracle** and reads
every pixel back through packets to check redraw, idle behavior, edits across
swaps, scrolling and failure latching. Its nine PS/2 host checks also pass.
Historical host tests compiled the GPIO board glue with stubbed Arduino
GPIO/time APIs to check all 256 write/read byte patterns, direction changes,
reset polarity, failure release and saved fault diagnostics across
write timeout, read timeout, startup fault and malformed reply. A simulated
800 ms configuration delay checks the one-second startup guard. A simulated
cold power-up checks the prefix retry: the pins read HIGH until configuration
finishes at 2.5 s, and an FPGA that never configures fails after 10 s with
reset released. These checks
cover the software mock. On 2026-11-07 the user reported the GPIO-driven terminal
running on HDMI with the Gowin-built 50.4 MHz image. The earlier all-zero
capability reads on the configured image were traced to the open-source FPGA
flow, as recorded in [Address-path isolation](#address-path-isolation-2026-11-07).
The new PIO transport still needs its first connected-board test. Build
settings and first-test procedure are in the firmware README.

**Host stack limit (firmware, 2026-11-07).** With the burst-copy image and the
2 us bus, the GPU kept presenting frames, but the PS/2 keyboard stopped
shortly after startup and Core 0 sometimes stopped answering serial commands.
The cause was in the RP2350 firmware, not the GPU or the bus. Arduino-Pico
gives each core a fixed 4 KiB stack bank, not main RAM. Core 0's bank
(SCRATCH_Y, `0x20081000`–`0x20082000`) sits directly above Core 1's
(SCRATCH_X, `0x20080000`–`0x20081000`), with no guard between them. The
client's frame code kept about 3.6 KiB of local arrays, plus a 1 KiB upload
packet, so Core 0 overflowed into Core 1's stack and halted the core that
serves the keyboard. A standalone sketch confirmed that Core 1 stopped
exactly when the client code first ran. The client now keeps these buffers in
the `NanoGpu` object (largest stack frame 208 bytes), and the user then
reported typing and stability fixed. Host code on any core should keep large
buffers in static storage, objects or the heap, never in local arrays.

### NARM GPU interface

Firmware API 1.19 advertises `NARM_CAP_GPU` (bit 6) and exposes
`NativeGpuApiV1` version 1.1 through interface `NARM_INTERFACE_GPU` (`0x0500`)
and the context's `gpu_api` pointer. Only the running, focused application can
submit GPU operations on Core 0. The terminal and NARM share a single firmware
client, with one sequence, epoch and front/back buffer state. The terminal
rebuilds any glyph cache invalidated by native drawing when it resumes.

`submit(context, command, args, arg_words)` takes an array of 32-bit ARM
argument words. The adapter validates the argument count, applicable CPU
pointers and GPU address ranges before the firmware serializes wire bytes.
Here `out` and `source` are native CPU pointers, and addresses of GPU records
are byte offsets in the FPGA SDRAM. The table uses `NARM_GPU_` names. NARMCC's
`narm.tch` declares a named `narm_gpu_*` built-in for each row. Its
`narm_gpu_copy_rect` built-in takes a pointer to a typed 14-word argument
block, which firmware serializes to the GPU.

| Command | Argument words in order | Result or constraints |
| --- | --- | --- |
| `GET_CAPS` (`0x0001`) | `out` | Writes the 64-byte capability reply payload. |
| `GET_STATUS` (`0x0002`) | `out` | Writes the 64-byte status reply payload. |
| `WRITE_HEAP` (`0x0003`) | `destination, source, byte_count` | Encodes and uploads 1–956 bytes within `0x280000`–`0x7fffff`. |
| `SUBMIT_PASS` (`0x0004`) | `descriptor` | Submits a 4-byte-aligned 32-byte pass descriptor in the user heap. |
| `COPY_RECT` (`0x0005`) | The 14 words in [section 8](#8-copy_rect-payload-56-bytes) | Copies to the current back buffer. Heap source images use the user heap. |
| `FENCE` (`0x0006`) | `token` | Waits for the matching reply after earlier GPU work retires. |
| `PRESENT` (`0x0007`) | `target, token` | Presents the current back buffer and waits for the matching vertical-blank reply. |
| `READ_MEMORY` (`0x0008`) | `address, byte_count, out` | Reads 1–512 bytes within GPU SDRAM. |
| `BACK_TARGET` (`0x0100`) | None | Returns the writable physical target, 0 or 1. |
| `ENCODE_PASS` (`0x0101`) | `out, target, flags, clear_rgb565, clear_d24, table` | Encodes the 32-byte descriptor in CPU memory. Use the current FPGA image for depth DISCARD. |
| `ENCODE_TILE` (`0x0102`) | `out, list, count` | Encodes an 8-byte tile entry. |
| `ENCODE_PRIMITIVE` (`0x0103`) | `out, input` | Encodes the 320-byte record from a separate CPU-side `NarmGpuPrimitive`. |
| `LINK_STATUS` (`0x0104`) | `out` | Reads the 64-byte sideband status bank even if the packet client is offline. |
| `ABORT` (`0x0105`) | None | Issues the out-of-band soft abort and waits for recovery. |
| `RESET` (`0x0106`) | None | Issues a hard reset and reloads the terminal font. |
| `FRAME_READY` (`0x0107`) | None | Returns 1 after earlier commands and any async present retire, or 0 while busy. |
| `TRY_PRESENT` (`0x0108`) | `target, token` | Returns 1 when it queues the present, or 0 while busy. |

The other commands return zero on success. Negative results are unknown
command (`-1`), invalid arguments (`-2`), missing capability (`-3`), the
caller lacking GPU focus or an asynchronous present still pending (`-4`),
and device failure (`-5`). The record builders cover all current primitive
settings. Applications prepare geometry, build ordered lists of primitive
addresses, and keep assets immutable while the GPU uses them.

**Always cache reusable textures, fonts, glyphs and sprites in GPU VRAM.**
Retain their addresses across frames and passes. Upload new or changed data
and restore allocations whose residency was lost after a focus handoff,
recovery or eviction. A redraw alone never requires another asset upload.

**Keep processing input and preparing the next frame while the GPU renders.**
`narm_gpu_frame_ready()` checks the completed sequence against the last
submitted sequence while no asynchronous swap is pending. On the Tang Nano
20K board, use blocking PRESENT to finish the swap. MMBasic submits its pass
first and performs blocking PRESENT at a later service point once the pass
retires, or when the next batch needs the back buffer. Query the back target
again after each completed swap.
[MMBasic presentation code](https://github.com/apc53pro/MPC-2350/commit/92b9dc7f904df436c18d5b2f3f4bcb3ccccef985)

`narm_gpu_try_present()` is exercised in the software model. The board still
has intermittent failures with that path. While such a swap is pending,
`narm_gpu_frame_ready()` polls register 1 and reads the matching reply from
register 0. The PIO transport performs bounded bus transfers. An upload of
at most 216 decoded bytes after a successful readiness check fits the empty
RX FIFO even with worst-case encoding. After recovery, follow the resource
and attachment rules in [section 5](#fault-codes-and-recovery).
[MPC-2350 firmware](https://github.com/apc53pro/MPC-2350/commit/b5af504eb1378e80bed713ddbbc6451ecc610eb6)

### Write and read timing

Set data/control before the rising edge; changing them on the falling edge is
the testbench convention. Do not assert `host_wr_valid` and `host_rd_req`
together. Writes are legal only to logical register 0.

| Host event | Write behavior | Read behavior |
| --- | --- | --- |
| Before rising edge | Present byte, address 0 and `wr_valid=1`. | Present address and `rd_req=1`, with writes disabled. |
| At rising edge | Byte transfers iff `wr_valid && wr_ready` were asserted before the edge. | Read logic advances; a byte may become valid after the edge. |
| After edge | Keep an unaccepted byte and its address stable while valid remains asserted. Advance only after acceptance. | Sample `rd_data` only when returned `rd_valid=1`; the simulator samples at a later falling edge. |
| No acceptance/result | Continue clocking and retain a stalled offered write. | Continue clocking until valid; a status snapshot crosses clock domains and is not an immediate combinational read. |

Each edge with both read request and a valid response can consume a stream byte.
For a single read, deassert the request after observing its result and before
the next rising edge. Do not use `host_wr_ready` as a read-valid indication.
Do not interpret arbitrary old `host_rd_data` as a new reply. Callback and DMA
implementations need finite timeouts and sideband fault checks while waiting.
Electrical setup/hold values and allowable clock rates remain board-validation
work; the C++ model's configurable digital timing checks are not PCB margins.

### Register/bank selection

| Logical address | Write | Read |
| --- | --- | --- |
| 0 | Append one request byte. | Consume one reply-stream byte when valid. |
| 1 | Invalid. | Refresh/latch a coherent 64-byte status snapshot and return status byte 1 with link flags. |
| 2..63 | Invalid. | Read that byte from the latched status snapshot. |
| 64..127 | Invalid. | Read immutable capability byte `address-64`. |

Read register 1 first, then 2..63 to inspect one coherent snapshot. Status byte
0 is reserved zero; it cannot be read through register 0 because that address
selects the reply FIFO. The packet `GET_STATUS` returns all 64 bytes directly.
Link flags added to register 1 are described in section 5.

Sideband reads remain available when a pass, packet parser or `PRESENT` is
blocked. **Packet** `GET_STATUS` and `GET_CAPS` are ordinary ordered commands in
the current RTL; they cannot overtake a long pass or a pending presentation.
Use sideband access for emergency fault/ready polling.

Current logical capacities are 256 RX bytes, two published command leases,
three 1024-byte packet leases, and 1024 TX bytes. The three packet leases use
two physical 2048×8 native RAMs; this sharing does not increase the advertised
payload limit or queue depth. Honor backpressure rather than
assuming a whole packet always fits. There is no packet CRC or automatic
retransmission in this protocol revision.

### Programming and first board bring-up

Use the full-board image `bitstreams/tangnano20k/nano-gpu.fs` and its matching
`docs/hardware/tang20k-board.json` implementation report. Do not program a
serial-probe image as a working GPU.

The image is built with Gowin EDA V1.9.8.11 Education by `make tang20k`
(`scripts/build-tang20k.py`). The open-source flow's host address latch
failed on the board. See [Address-path isolation](#address-path-isolation-2026-11-07).

The temporary exception in `AGENTS.md` records the unverified Gowin EDA
installer and the encrypted synthesis netlist, which prevents checking the
fixed SiP SDRAM connection. Font upload, copies and scanout exercise the
SDRAM on the board.

The current image is **7,264,137 bytes**, SHA-256
`5b6f75aa2d988df4a897f57cd38a4f3768c1cf78ab1848b06787eeabded908e6`. It accepts load-flag
bit 3 (SKIP). On 2026-11-08 it was written to the board's SPI flash and
verified, and the FPGA reloaded it (status `0x6020`, user code `0x72CD`). The
user measured `demo_3d_engine6` on it, with blocking presentation, at a stable
60 fps. With the earlier MPC-2350 firmware's asynchronous presentation
(`try_present` and `frame_ready` polling), the same demo lost one byte at the
start of a PRESENT reply. The firmware rejected the header and took the link
offline until the RP2350 was reset. The software model passed with the earlier
read order. On 2026-11-09 the user reported intermittent crashes with the
revised asynchronous polling order. The cause remains open. The current
MMBasic build uses blocking PRESENT with this FPGA image.

The previous image (`3fc16700…06ef`, user code `0xE7A9`, flashed and verified on
2026-11-08) and the one before it (`a249064f…9ef9`, user code `0x3354`)
reject SKIP with fault 6 but accept the depth-DISCARD flag (bit 2); the image
before those (`55ebdfbd…d170`, user code `0x33E2`) rejects DISCARD too.
Program this image before running software built with the current draw SDK.
The [programming-file check](../docs/hardware/tang20k-bitstream-check.json)
verifies device ID, declared frame count and frame CRCs against that hash. The
[functional report](../docs/hardware/tang20k-functional.json) separately scopes
wrapper-model and real-core simulation; neither is a physical-board test.

Check the release checksum from the repository root with:

```bash
sha256sum -c bitstreams/tangnano20k/SHA256SUMS
```

#### Tools: one for building, a different one for programming

| Job | Tool (exact) | Location on the user's PC |
| --- | --- | --- |
| Build the image (`make tang20k`) | Gowin EDA **V1.9.8.11 Education** `gw_sh.exe` | `C:\Gowin\Gowin_V1.9.8.11_Education\IDE\bin\gw_sh.exe` |
| Program SRAM or flash | Standalone Gowin Programmer **V1.9.12.04 (64-bit)** `programmer_cli.exe`, Authenticode-signed by Gowin | `C:\Gowin\Gowin_Programmer_V1.9.12.04\Programmer\bin\programmer_cli.exe` |

**Never program the flash with the Education IDE's own
`C:\Gowin\Gowin_V1.9.8.11_Education\Programmer\bin\programmer_cli.exe`.** It
detects the Tang's SPI flash (`0x0B4017`) but fails every flash operation at
0% with `Error found!`. Each attempt leaves the FPGA unconfigured (status
`0x00000020`) and the flash contents uncertain. It can still load SRAM. The
signed V1.9.12.04 programmer came from Gowin's
`Gowin_Programmer_V1.9.12.04_x64.zip`; check `Get-AuthenticodeSignature`
reports `Valid` if it is reinstalled. Copy it to the location above, or set
`GOWIN_PROGRAMMER` to its `programmer_cli.exe`.

#### Programming commands

Connect the Tang's onboard USB-C programming connection. From an MSYS2 shell
at the repository root:

```bash
make tang20k-flash
```

```bash
make tang20k-sram
```

```bash
make tang20k-status
```

`tang20k-flash` checks the image hash, then erases, programs and verifies the
external SPI flash (operation 9; expect `SPI program and verify success!`).
It then reloads the FPGA from flash (operation 1) and reads its codes. The
image persists across power cycles. `tang20k-sram` loads volatile SRAM
(operation 2) for a quick test; power-off returns to the flash image. The
equivalent PowerShell commands are:

```powershell
Remove-Item Env:PYTHONIOENCODING -ErrorAction SilentlyContinue
$cli = 'C:\Gowin\Gowin_Programmer_V1.9.12.04\Programmer\bin\programmer_cli.exe'
& $cli --cable-index 1 -d GW2AR-18C -r 9 --fsFile "$PWD\bitstreams\tangnano20k\nano-gpu.fs"
& $cli --cable-index 1 -d GW2AR-18C -r 1
& $cli --cable-index 1 -d GW2AR-18C -r 0
```

A configured image reads **status `0x00006020`** (configuration done, no CRC
error) and that image's user code. Immediately after a flash write or a
reload, the device can read `0x00004460` with user code 0 for a few seconds
while it configures; read again before deciding it failed. If the codes are
missing entirely or the IDE programmer reports `Cable open failed`, the board
is unplugged or another programmer holds the cable.

Gowin bitstreams enable read-back security, so an SRAM program-and-verify
(operation 4) reports a verify failure even after a good load. On Windows,
`programmer_cli` fails to start if `PYTHONIOENCODING` is set; clear that
variable first (the make targets do). Installing Gowin software binds the
debugger's FTDI interfaces to FTDI's driver. [openFPGALoader](https://trabucayre.github.io/openFPGALoader/guide/installation.html)
(`-b tangnano20k`, with `-f` for flash) then reports `device not found` until
interface A is bound to WinUSB again, for example with Zadig.

Successful implementation and file generation are not a claim that the
received board, HDMI connection, SDRAM or RP2350 bus has been qualified physically.

For an isolated Tang-only test, keep all 16 RP2350 signal links **disconnected**;
the previous VGA firmware is incompatible with the GPU bus. Power the Tang normally and
connect HDMI. GPU_RESET_N has an FPGA pad pull-down in the package constraints:
with it LOW the GPU remains in
reset. With the RP2350 completely isolated, a local jumper from Tang J6.19
(3.3 V) to Tang J5.18 (GPU_RESET_N, FPGA pin 71) can release reset. Never fit
that jumper while the RP2350 or another output can drive the reset wire LOW.
Keep that jumper absent while programming; fit it only after this GPU image
has loaded successfully. Remove it before power-cycling an SRAM-only test,
since the factory flash image can return and may drive that pin. The expected
output after reset release is the GPU's own 640×480/60 Hz scanout of its front
buffer, which is black until a host draws.
There is no built-in scene, command player, status overlay or visible animation.
The [current MPC-2350 GPU driver](#mpc-2350-gpu-driver)
draws its terminal with the commands documented below. Remove the isolated
bring-up jumper before connecting the host.

## 3. Packets and replies

Every multibyte integer is little-endian. Signed fields use two's complement.
No C compiler structure layout or padding is transmitted.

### Request header: 16 bytes

| Offset | Bytes | Field |
| --- | ---: | --- |
| 0 | 4 | `4E 47 50 55`, ASCII `NGPU`; LE u32 `0x5550474E`. |
| 4 | 1 | Major version **0**. |
| 5 | 1 | Minor version **2**; other versions are rejected. |
| 6 | 2 | Opcode, u16. |
| 8 | 4 | Payload byte length, excluding this header; maximum 1024. |
| 12 | 4 | Request sequence, u32, nonzero and strictly increasing within the epoch. |

The parser publishes only a complete, valid packet. Partial payloads do not
execute. Never insert padding between packets unless that padding belongs to
the declared payload. Never search for a new `NGPU` magic inside failed upload
data; recover out of band instead.

### Reply header and common payload

Replies use the same 16-byte header, with opcode `request_opcode | 0x8000`,
the request sequence, and payload length. Every successful reply starts with:

| Payload offset | Bytes | Field |
| --- | ---: | --- |
| 0 | 4 | Execution epoch. |
| 4 | 4 | Result status, currently always zero for emitted successful replies. |
| 8 | 4 | Token: caller token for `FENCE`, `PRESENT`, `READ_MEMORY`; zero for queries. |
| 12 | Variable | Command-specific extra data. |

Validate magic, version, response opcode, exact length, sequence, epoch, status
and token. The current RTL stops on a fault and does **not** emit a normal
error/NACK reply. A reply wait must therefore time out or detect a sideband
fault and enter recovery. Do not wait forever for a successful fence after a
failed pass. Only one software consumer may drain the reply stream.

### Command table

Offsets in the payload column are relative to the first byte after the header.

| Opcode | Name | Exact request payload | Successful response |
| --- | --- | --- | --- |
| `0x0001` | `GET_CAPS` | Empty. | Common 12 bytes + 64 capability bytes; **92 bytes total**. |
| `0x0002` | `GET_STATUS` | Empty. | Common 12 bytes + 64 status bytes; **92 bytes total**. |
| `0x0003` | `WRITE_HEAP` | u32 destination at 0, u32 **decoded byte length 1..956** at 4, then grouped encoded words at 8; total payload at most 1024 bytes. | No reply; completion through a later fence/status sequence. |
| `0x0004` | `SUBMIT_PASS` | u32 pass-descriptor address at 0; **4 bytes**. | No reply; later fence waits for rendering and stores. |
| `0x0005` | `COPY_RECT` | **56 bytes**, layout in section 8. | No reply; later fence waits for the copy. |
| `0x0006` | `FENCE` | u32 caller token at 0; **4 bytes**. | Common payload only; **28 bytes total**. |
| `0x0007` | `PRESENT` | u32 physical color target at 0, u32 presentation token at 4; **8 bytes**. | Common payload only, after the swap at vertical blank; **28 bytes total**. |
| `0x0008` | `READ_MEMORY` | u32 byte address at 0, u32 length at 4, u32 result token at 8; **12 bytes**. Length **1..512**. | Common payload + u32 requested address + u32 returned length + data; **36+length bytes total**. |

`WRITE_HEAP` destinations may be unaligned, and byte masks preserve adjacent
bytes. Its entire range must be in the heap. `READ_MEMORY` may be unaligned
and may inspect any valid SDRAM range, including attachments; it is diagnostic
readback, not the display data path. Neither command permits address wrap.
There is no normal command opcode for soft abort or hard reset.

### Sole resource-upload encoding

All textures, records, reference lists, tables and descriptors use this same
`WRITE_HEAP` encoding. Starting at the first byte of the decoded input, form
little-endian words of up to four bytes. The CPU zero-pads the last partial
word. For each group of up to four words, send one descriptor byte followed by
the selected payloads in word order. The first word's code is in bits 1:0,
the next in 3:2, then 5:4 and 7:6.

| Code | Payload bytes | Reconstructed u32 |
| --- | ---: | --- |
| 0 | 0 | Zero |
| 1 | 1 | Sign-extended signed 8-bit value |
| 2 | 2 | Sign-extended little-endian signed 16-bit value |
| 3 | 4 | Literal little-endian 32 bits |

Literal words are part of this one encoding, not another upload mode. Every
possible byte sequence is representable. The current CPU encoder chooses the
shortest exact representation for each zero-padded word; the GPU accepts any
of these representations. It writes exactly the decoded byte length and
ignores the unused high bytes of a partial final word. Unused codes in a
partial final group must be zero. Missing descriptors or word bytes, trailing
encoded bytes and nonzero unused codes fault with core code 3. A malformed
upload can have written earlier complete chunks: its destination range is
unspecified until a successful completion fence, and must be reuploaded after
recovery.

For decoded words `00000000 0000007f ffffff80 00001234`, the encoded body is
`94 7f 80 34 12`: descriptor `94` selects codes 0,1,1,2. The eight-byte address
and decoded-length prefix precedes that body.

The 956-byte limit guarantees every input fits: 239 literal words plus 60
descriptor bytes occupy 1016 bytes, leaving eight bytes for the prefix inside
the existing 1024-byte packet store. Split larger resources into successive
uploads. Compression can grow incompressible input; there is no raw fallback.

`FENCE` orders all earlier commands and their required SDRAM stores. A fence
before `PRESENT` establishes that drawing is finished. A fence after `PRESENT`
also waits for the preceding swap. A reply's bytes becoming available and the
host actually consuming them are different events; drain replies to avoid
blocking later work through a full TX FIFO.

## 4. Memory, passes and buffer ownership

The GPU has 8 MiB of SDRAM. Ranges are half-open:

| Region | Start | End, excluded | Size |
| --- | --- | --- | ---: |
| Physical color 0, RGB565 | `0x000000` | `0x096000` | 614,400 bytes |
| Physical color 1, RGB565 | `0x096000` | `0x12C000` | 614,400 bytes |
| Shared D24 attachment, stored as u32 | `0x12C000` | `0x258000` | 1,228,800 bytes |
| Heap: textures, records, lists, tables, descriptors | `0x258000` | `0x800000` | 5,931,008 bytes |

Color pitch is 1280 bytes; pixel `(x,y)` is at `color_base + 1280*y + 2*x`.
Depth pitch is 2560 bytes, with low 24 bits meaningful and high eight bits zero.
Depth zero is near and `0xFFFFFF` is far; LESS rejects a sample equal to the
clear value, while LEQUAL accepts equality.
`WRITE_HEAP` cannot write attachments. Heap allocation, nonoverlap and resource
lifetime belong to the CPU driver. Check sums with at least 64-bit arithmetic.

After boot initialization, color 0 is front, color 1 is back, both are black,
and depth is `0xFFFFFF`. Render/copy only to the writable non-front buffer;
LOAD and partial copies additionally require valid preserved contents.
`PRESENT` seals its target when it executes, then swaps at vertical blank.
The old front becomes writable after the reply. Never mutate the displayed or
sealed buffer. Late rendering repeats the current front.

Depth is shared and never swapped. After a swap, the new back contains the old
front's color, while depth contains the latest pass's depth. Clear depth for a
new 3D frame unless the application deliberately maintains matching contents.
`LOAD` preserves the named physical buffer, not automatically the latest image.
The CPU may copy front to back first when preservation of that image is needed.

### Pass descriptor: 32 bytes, address aligned to four bytes

| Offset | Type | Value |
| --- | --- | --- |
| 0 | u32 | Magic `0x3153504E`, bytes `4E 50 53 31` (`NPS1`). |
| 4 | u32 | Physical target color ID, 0 or 1. |
| 8 | u32 | Bit 0 color CLEAR, bit 1 depth CLEAR; a clear bit of 0 means LOAD. Bit 2 depth DISCARD: depth is never written back. Bit 3 SKIP: tiles without references are left untouched. Other bits zero. |
| 12 | u32 | Clear RGB565 in low 16 bits; upper bits zero. |
| 16 | u32 | Clear D24 in low 24 bits; upper bits zero. |
| 20 | u32 | Four-byte-aligned tile-table heap address. |
| 24 | u32 | Tile count, exactly **300**. |
| 28 | u32 | Reserved zero. |

The table is 300 entries of `{u32 list_address, u32 reference_count}`: **2400
bytes**, in order `tile_index = (y/32)*20 + x/32`. Each referenced list contains
ordered u32 primitive-record addresses. Empty lists have count zero **and**
address zero. Nonempty lists and records are four-byte aligned heap ranges.
Total references per pass must not exceed `1,048,576`; the supplied CPU compiler
additionally limits its input to 16,384 primitive records.

Without bit 3, the load flags apply to every tile, including empty ones; the
result is as if each tile were loaded or cleared and then stored. Only the
work differs:

| Flags | Use | Tile without draws | Tile with draws |
| --- | --- | --- | --- |
| `3` | New 3D frame | Constant color and depth written | Clear, draw, store color and depth |
| `7` | New 2D frame | Constant color written | Clear, draw, store color |
| `6` | 2D update over valid color | **Skipped** | Load color, draw, store color |
| `0` | Incremental 3D over valid color and depth | **Skipped** | Load both, draw, store both |
| `15` | New 2D or single-pass 3D frame, dirty tiles only | **Skipped** | Clear, draw, store color |

With bit 2 set, depth is cleared or loaded into each tile and tested as usual,
but the external depth attachment is left unchanged and becomes **invalid**:
any later pass with bit 1 clear (depth LOAD) faults with category 5 until a
pass with bit 1 set and bit 2 clear stores a depth CLEAR. Use flags `7`/`6` for work that
never tests depth; then no depth crosses SDRAM. A LOAD-color pass costs only
the tiles that have references, so small updates should bin only the tiles
they touch.

With bit 3 (SKIP) set, a tile whose list is empty is not processed at all: its
color and depth stay exactly as they are in SDRAM, even when the pass clears.
Tiles with references are cleared or loaded, drawn and stored as usual. SKIP
therefore needs a valid target color, like a color LOAD (fault 5 otherwise),
and the external depth stays valid afterwards only if it was valid before or
is discarded. Use it to redraw only part of a frame with a CLEAR: list every
tile drawn now **and** every tile that may still hold earlier drawing in that
physical buffer. A tile that must be cleared but has nothing to draw needs one
reference to a record that draws nothing; a rectangle with an empty scissor
(words 4..7 zero) is binned by its bounds and is clipped away at once. The
MPC-2350 draw SDK tracks those dirty tiles per color buffer and uses SKIP
after the first full clear of each buffer. For incremental updates it also
tracks tiles that can differ between the two buffers, copies only those tiles
from front to back, and submits new draws with color LOAD. Each
primitive must appear once in every tile whose clipped bounds overlap it; use
the original logical draw order within each list. It is safe to include a tile
that ultimately has no covered pixels. Do not omit a tile that can contain a
sample. Uploading a record does not bin it automatically.

All descriptor/table/list/record/texture allocations referenced by an active
pass remain immutable until that pass finishes. Ordered updates after a fence
can reuse them. Textures must live in the heap; sampling a framebuffer or the
active target as a texture is unsupported. The RTL invalidates texture cache
state at pass submission, so ordered heap updates are visible to the next pass.

## 5. Status, capabilities and faults

All offsets in this section refer to the 64-byte bank, **not** to packet byte
zero. Packet snapshots begin at total reply byte 28.

### Status bank

| Byte offset | Type | Current RTL value |
| --- | --- | --- |
| 0 | u8 | Reserved zero. |
| 1 | u8 | Core flags: bit 0 ready, bit 1 busy, bit 2 sealed/presentation pending, bit 3 core fault. |
| 2 | u32 | Execution epoch. |
| 6 | u32 | Last completed command sequence. |
| 10 | u32 | Last successful fence token. |
| 14 | u8 | Current physical front color ID. |
| 15 | u8 | Bit 0 color 0 valid, bit 1 color 1 valid, bit 2 depth valid. |
| 16 | u64 | 48-bit core cycle counter zero-extended to u64; wraps at 2^48, restarts on hard reset. |
| 24 | u64 | Scanout underflows; this RTL supplies a 32-bit count and zero upper half. |
| 32 | u32 | Core fault code, currently low eight bits used. |
| 36 | u32 | Last successful presentation token. |
| 40..55 | Bytes | Reserved zero in RTL. |
| 56 | u8 | Transport/parser fault code. |
| 57..63 | Bytes | Reserved zero. |

For elapsed-time arithmetic using the core counter, mask differences to 48
bits; the u64 slot does not imply a physical 64-bit counter. Driver timeouts
should use the CPU's own monotonic clock and bounded transport operations.

Sideband register 1 adds bit 7 transport fault, bit 6 RX full, bit 5 published
command queue full, and bit 4 reply available. Queue-full and parser-fault
contributions come from the transferred core snapshot; a local host fault also
sets bit 7 directly. RX-full/reply-available bits are evaluated in the host domain.
Check both core fault bit 3 and transport fault bit 7, plus the diagnostic
fault bytes. The packet `GET_STATUS` returns the raw core snapshot and does
not add these upper four host-link flag bits. Reserved RTL bytes must not be
interpreted as the extra diagnostic counters provided by the older C++ model.

`busy` includes command processing; a packet `GET_STATUS` snapshot can itself
show busy. `ready` means initialization/cross-domain link recovery permits the
core to run; it does **not** alone mean fault-free, idle, or abort-complete.

An ordered `GET_STATUS` reply remains one coherent snapshot during slow reply
draining. The implementation captures independently changing counters and
flags, and holds execution state stable until serialization finishes.
`GET_CAPS` serializes immutable constants directly. This internal storage
sharing does not make either reply a series of unrelated live register reads.
Sideband reads use the acknowledged core snapshot bank, which stays unchanged
until a new refresh request; refresh blocks reads until its acknowledgement.
There is no second host-domain copy of all 64 bytes.

### Capability bank

| Byte offset | Type | Current value / meaning |
| --- | --- | --- |
| 0 | u8 | Major 0. |
| 1 | u8 | Minor 2. |
| 2 | u8 | Bus data width 8. |
| 3 | u8 | Tile side 32. |
| 4 | u32 | SDRAM bytes: `0x800000`. |
| 8 | u32 | Heap base: `0x258000`. |
| 12 | u32 | Maximum request payload: 1024. |
| 16 | u32 | Primitive record bytes: 320. |
| 20 | u32 | Maximum pass references: 1,048,576. |
| 24 | u32 | Configured core Hz: board 50,400,000; default simulation 100,800,000. Not measured Fmax. |
| 28 | u32 | Host-divider metadata: board 0 (independent), default simulation 4; host supplies its clock. |
| 32 | u32 | Logical scanout pixel divider: board 2 (one synchronized physical pixel event per two core clocks), default simulation 4. |
| 36 | u32 | RX FIFO bytes: 256. |
| 40 | u32 | Published command capacity: 2. |
| 44 | u32 | TX FIFO bytes: 1024. |
| 48 | u32 | Display FIFO pixels: 2048. |
| 52 | u32 | Width: 640. |
| 56 | u32 | Height: 480. |
| 60 | u32 | Current feature mask `0x000000FF`; do not infer future features from reserved bits. |

### Fault codes and recovery

Core fault codes are: 1 transport, 2 payload/opcode, 3 address/range, 4 buffer
ownership or COPY arguments, 5 invalid LOAD contents, 6 descriptor/record/
texture/numeric failure, 7 reference limit/list failure, 8 SDRAM client error,
9 internal state, and `0xFF` exhausted epoch counter. The code is a category,
not a full failing-address report. Several structural failures share code 6.

Current host RTL emits: 1 simultaneous read/write, 2 write to nonzero register,
5 changed stalled write byte/address, 7 magic, 8 version, 9 opcode, 10 packet
length, 11 non-increasing/zero sequence. Do not assume the C++ model's additional
digital setup/hold fault codes are measured by this RTL.

Recovery procedure:

1. Stop host transfers, quiesce any packet DMA and deassert write/read requests. Discard local
   expectations for unfinished replies; stop submitting new work.
2. Request out-of-band soft abort. Keep **both clocks running** while accepted
   memory work drains and the cross-domain RX/TX/parser flush completes.
3. Through sideband reads, wait for a **changed epoch**, ready asserted and
   fault indications cleared. Ready alone is insufficient. ABORT is internally
   synchronized into the core domain: hold it HIGH for at least four core
   cycles, then release it before waiting for recovery; begin with a generous
   microsecond pulse during bring-up. There is no dedicated ACK pin; observe
   the changed epoch. Electrical setup/hold margins remain unmeasured.
4. Discard any old local reply bytes, reset sequence allocation to 1, and
   invalidate the CPU's assumptions about interrupted resources. An interrupted
   heap write leaves its touched range unspecified; reupload it before use.
5. If back/depth validity was lost, submit a full CLEAR/CLEAR pass before LOAD
   or presentation. Preserve the front until valid new work completes.

Internally, recovery first registers a quiescent foreground state and stops
new foreground memory commands, including texture refills. It then waits for
accepted work to drain before resetting the renderer and flushing the host
stream. A command accepted on the edge that establishes quiescence is still
tracked and drained. Quiescence remains asserted through host flush acknowledgement before
the executor returns to idle. These internal control edges can extend recovery;
drivers must use the epoch/status checks above rather than a fixed delay.
The display memory client remains active throughout soft recovery.

Soft recovery increments the 32-bit epoch. It cannot recover past
`0xFFFFFFFF`; hard reset is then required. **Hard reset sets epoch to 1 again**,
reinitializes attachments, and invalidates heap assumptions. Therefore epoch
is not a globally unique power-cycle identifier: a reconnecting driver must
establish a fresh transport session and discard stale bytes/completions.

## 6. Primitive record format: 320 bytes

Zero the entire record before encoding it. Primitive kinds are triangle 0,
rectangle 1, line 2, sprite 3. Version is 1. All unused/reserved bytes and bits
must be zero.

`WRITE_HEAP` decodes into this unchanged record layout. The
[earlier standalone experiment](../docs/hardware/upload-codec.md) measured a
separate decoder and failed GPU co-residency probe; the current implementation
instead shares the existing upload staging and register datapath. Shared-state
references and compact records stored in SDRAM remain unimplemented.

| Offset | Type / size | Meaning |
| --- | --- | --- |
| 0 | u32 | Kind bits 0..7; version bits 8..15; color write bit 16, depth test bit 17, depth write bit 18, color-key enable bit 19. Bits 20..31 zero. |
| 4 | u32 | Shade mode bits 0..7; depth function bits 8..15; U REPEAT bit 16; V REPEAT bit 17; texture descriptor present bit 18. Bits 19..31 zero. |
| 8 | u32 | Constant RGB888: R in bits 0..7, G in 8..15, B in 16..23; high byte zero. |
| 12 | u32 | Raw RGB565 color key in low 16 bits; zero if keying disabled; high half zero. |
| 16 | i32 × 4 | Scissor `{x0,y0,x1,y1}`, half-open. |
| 32 | i32 × 4 | Primitive bounds `{x0,y0,x1,y1}`; all zero for a line. |
| 48 | u32 × 4 | Texture `{base,width,height,pitch_bytes}`; all zero if descriptor absent. |
| 64 | u32 | Constant D24 for 2D; zero for a triangle. |
| 68,72 | i32, i32 | Sprite source X/Y or line endpoint A X/Y; zero for triangle/rectangle. |
| 76 | u32 | Reserved zero. |
| 80 | i32 × 6 | Triangle Q4 vertex `{X0,Y0,X1,Y1,X2,Y2}`. For a line only offsets 80/84 hold endpoint B X/Y; remaining bytes zero. |
| 104 | 3 × 16 bytes | Triangle edges `{i32 A,i32 B,i32 C,u32 inclusive}`; inclusive is 0 or 1. |
| 152 | 7 × 24 bytes | Triangle planes in order **z,r,g,b,q,U,V**; each `{i64 seed,i64 step_x,i64 step_y}`. |

Shade modes: 0 FLAT, 1 GOURAUD, 2 TEXTURE_REPLACE, 3 TEXTURE_FLAT_MODULATE,
4 TEXTURE_GOURAUD_MODULATE. Depth functions: 0 LESS, 1 LEQUAL, 2 ALWAYS.
Each address axis uses CLAMP when its REPEAT bit is zero.

Scissors, rectangle/sprite bounds, sprite source origins and line endpoints
use integer pixels. Bounds are half-open; a 1×1 rectangle draws one pixel.
Line endpoints must be CPU-clipped inside the viewport before encoding. Lines
include both endpoints and use the global line sequence in the
[numeric contract](../docs/spec/rasterization.md#2d-coverage); never restart the
line from separately clipped tile endpoints. Bin a line using the inclusive
endpoint bounding box converted to half-open bounds, intersected with scissor
and viewport, even though its record's bounds field is zero.

Rectangles and lines support flat, untextured, unkeyed color. Sprites support
raw 1:1 texture replacement with optional key and constant depth; mode 0 is also
accepted as a raw sprite alias, but new drivers should use mode 2. Sprites have
no scaling, flipping, rotation or Gouraud/modulated shading. Copies are a
different command, not shaded primitives. Color/depth writes and depth testing
are independently enabled. Key rejection suppresses **both** attachment writes.

For triangles, only textured modes may enable color keying. The CPU setup/encoder rejects
untextured keying; the current raw RTL validator has a known gap and can ignore
that invalid flag. Drivers must reject it instead of relying on accidental
acceptance. Supported combinations are not an exhaustive proof of all possible
state combinations.

Texture dimensions are powers of two in 1..1024; pitch is at least `2*width`
and divisible by four. Base is four-byte aligned and the full `pitch*height`
extent, including last-row padding, lies in the heap. Format is RGB565, stored
little-endian. Nearest sampling uses floor before per-axis clamp/repeat. Raw
color key comparison occurs before RGB expansion or modulation. Sprite source
rectangles must fit the texture even when destination clipping removes pixels.
A textured triangle needs its descriptor when color writes or keying require
sampling. A depth-only triangle with both disabled may omit texture sampling
and the descriptor even if a textured shade mode is selected. Sprites always
require a compatible descriptor.

### Preparing triangle records

The CPU transforms, clips, projects, snaps vertices to Q4, normalizes winding,
computes edges/bounds and prepares the attribute planes. It also performs any
lighting used to produce constant or vertex RGB values. No vertex/index buffer
or projection matrix command exists in this revision.

For sample anchor `(x0,y0)` from the triangle's clipped bounds, each plane is
`seed + (x-x0)*step_x + (y-y0)*step_y`. Depth is D24-code units with 16 fractional
bits; RGB channel planes have 16 fractional bits; q has scale `2^37`; U=u*q and
V=v*q have scale `2^27`. All stored values are signed 64-bit two's complement.
Coverage uses unbiased edges at pixel centers and top-left inclusion. The
maximum W ratio is 64 in the current setup profile; normalized vertex q is
in `[1/128,1/2]`, and covered reconstructed q must be `[1/256,1]`.

Use the complete equations, overflow checks and RNE rules in the
[numeric contract](../docs/spec/rasterization.md). Do not invent coefficient
scales from these short descriptions. The desktop
[`setup_triangle()` implementation](../reference/src/setup.cpp) is an executable
reference, not a ready-to-flash RP2350 driver: it uses host facilities including
`__int128`, which typical 32-bit RP2350 GCC targets do not provide. An RP2350
port needs suitable bounded/wide arithmetic or a validated equivalent setup.
The fixed prepared triangle in section 9 avoids that portability requirement
for an initial interface test.

## 7. Frame sequence

For a new session, first establish initialization and a fault-free epoch through
sideband status, then query capabilities. A simple serialized frame is:

```text
WRITE_HEAP(new, changed or missing asset ranges only)
WRITE_HEAP(immutable primitive records)
WRITE_HEAP(ordered reference lists, 300-entry table, pass descriptor)
SUBMIT_PASS(descriptor address)
FENCE(render token) -> wait for matching successful reply
PRESENT(back buffer, presentation token) -> wait for matching reply
```

Allocate resident assets separately from transient records and tile lists.
Keep a valid asset's VRAM address across frames and upload only changed
ranges for dynamic content. Reclaim or overwrite an allocation only after
all submitted users finish. The RTL demo uses `HostPassCompiler` to retain
its texture, glyph and sprite allocations across frames. Its statistics
report the total texture bytes uploaded and the number of frames with
texture uploads. For the two-frame showcase, `texture_upload_frames` is 1.
Drivers that queue work must preserve sequence/epoch
identity and physical buffer ownership.

## 8. COPY_RECT payload: 56 bytes

All fields are four bytes and little-endian. Coordinates and requested
width/height are signed i32; widths/heights must be nonnegative.

| Offset | Field |
| --- | --- |
| 0 | u32 source kind: 0 physical framebuffer, 1 heap image. |
| 4 | u32 source framebuffer ID 0/1; must be 0 for heap images. |
| 8 | u32 target framebuffer ID 0/1. |
| 12,16 | i32 source X/Y. |
| 20,24 | i32 destination X/Y. |
| 28,32 | i32 requested width/height. |
| 36,40,44,48 | u32 heap image base/width/height/pitch; zero for framebuffer source. |
| 52 | Reserved zero. |

Heap image dimensions may be non-power-of-two, in 1..1024, with aligned base/
pitch, pitch at least twice the width, and the full extent inside the heap.
The target must be writable back; front is allowed as a read-only source.
Source/destination clipping preserves their pixel correspondence. Same-buffer
overlap has snapshot/memmove semantics. Zero-area copies are no-ops. Copies
do not apply keying, shading, filtering or depth operations. A partial copy
does not initialize an invalid target; a full destination-covering copy can.
The current executor validates the prefix one full raw word at a time, then
refetches 14 COPY parameter words from its immutable packet lease. X and Y
reuse one signed 33-bit interval/clipping datapath. For a nonempty copy, that
refetch/clipping phase takes about 128 core clocks before address walking and
pixel transfers. Completion is determined by the ordered fence/reply rules,
not a fixed CPU delay. Native coefficient capture adds one setup clock per
nonempty primitive/tile invocation. Staged draw validation/clipping adds three
setup clocks per valid invocation, including valid empty windows; texture
configuration has two preparation edges before final acceptance. The sampler's
private exact integer-texel pipeline has 19 enabled-edge latency and initiation
interval one. These internal latencies do not modify prepared records, numeric
rules or completion semantics.

## 9. Generic C99 driver examples

The following single C99 block is self-contained packet/heap logic. It requires
only standard C headers and the three callbacks in `GpuTransport`. It contains
no GPIO numbers, PIO program, host timing delays, or SDK-specific calls.

Callback contract:

- `write_byte`: transfer exactly one byte to register 0, respecting valid/ready;
  return true only after acceptance.
- `read_byte`: consume exactly one valid register-0 reply byte, sampling after
  the host edge; return true only after that byte is received.
- `read_register`: issue one read of a nonzero logical register, wait for its
  registered valid result, and deassert the request before another transfer.
- Every callback must have a finite timeout, continue clocking as required,
  and return false on transport failure. The HAL must keep clocks running for
  cross-domain progress; it must implement abort/reset separately.

The helper is intentionally serialized and allows one expected reply at a time.
After a partial transfer or invalid reply it marks the link failed; perform the
recovery procedure before reinitializing it. `gpu_init()` only initializes CPU
state and must be used with a fresh/quiesced transport session, not to restart
sequence numbers against a GPU that is still processing old commands.

Its scratch layout reserves 3952 heap bytes for one record/pass. A real driver
should replace this fixed allocation with its allocator. Call `gpu_submit_one`
only after a prior fence has released this scratch range. It clears both
attachments and bins one primitive; this is a small complete example, not an
efficient many-object renderer.

<!-- gpu-control-c99:start -->
```c
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
    GPU_GET_CAPS = 1, GPU_GET_STATUS = 2, GPU_WRITE_HEAP = 3,
    GPU_SUBMIT_PASS = 4, GPU_COPY_RECT = 5, GPU_FENCE = 6,
    GPU_PRESENT = 7, GPU_READ_MEMORY = 8,
    GPU_WIDTH = 640, GPU_HEIGHT = 480, GPU_TILES = 300,
    GPU_RECORD_BYTES = 320, GPU_MAX_PAYLOAD = 1024, GPU_MAX_UPLOAD_DECODED = 956,
    GPU_MAX_REPLY_PAYLOAD = 532
};
#define GPU_MEMORY_BYTES UINT32_C(0x800000)
#define GPU_HEAP_BASE UINT32_C(0x258000)
#define GPU_TABLE_ADDR GPU_HEAP_BASE
#define GPU_RECORD_ADDR (GPU_TABLE_ADDR + UINT32_C(2400))
#define GPU_REFS_ADDR (GPU_RECORD_ADDR + UINT32_C(320))
#define GPU_PASS_ADDR (GPU_REFS_ADDR + UINT32_C(1200))
#define GPU_TEXTURE_ADDR (GPU_PASS_ADDR + UINT32_C(32))

typedef struct {
    void *user;
    bool (*write_byte)(void *, uint8_t);
    bool (*read_byte)(void *, uint8_t *);
    bool (*read_register)(void *, uint8_t, uint8_t *);
} GpuTransport;
typedef struct {
    GpuTransport transport;
    uint32_t next_sequence, epoch;
    bool failed;
} Gpu;
typedef struct {
    uint32_t payload_bytes;
    uint8_t payload[GPU_MAX_REPLY_PAYLOAD];
} GpuReply;
typedef struct { int32_t x0, y0, x1, y1; } GpuBounds;

void gpu_put16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8);
}
void gpu_put32(uint8_t *p, uint32_t value) {
    unsigned i;
    for (i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (8 * i));
}
void gpu_put64(uint8_t *p, int64_t value) {
    uint64_t bits = (uint64_t)value;
    unsigned i;
    for (i = 0; i < 8; ++i) p[i] = (uint8_t)(bits >> (8 * i));
}
uint32_t gpu_get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
void gpu_init(Gpu *g, GpuTransport transport) {
    memset(g, 0, sizeof(*g)); g->transport = transport; g->next_sequence = 1;
}
bool gpu_fail(Gpu *g) { g->failed = true; return false; }
bool gpu_heap_range(uint32_t address, uint32_t length) {
    return address >= GPU_HEAP_BASE &&
           (uint64_t)address + length <= GPU_MEMORY_BYTES;
}
bool gpu_read_bytes(Gpu *g, uint8_t *bytes, uint32_t length) {
    uint32_t i;
    if (g->failed || !g->transport.read_byte) return false;
    for (i = 0; i < length; ++i)
        if (!g->transport.read_byte(g->transport.user, bytes + i)) return gpu_fail(g);
    return true;
}
bool gpu_snapshot(Gpu *g, uint8_t status[64]) {
    unsigned i;
    if (g->failed || !g->transport.read_register) return false;
    status[0] = 0;
    for (i = 1; i < 64; ++i)
        if (!g->transport.read_register(g->transport.user, (uint8_t)i, status + i))
            return gpu_fail(g);
    return true;
}
bool gpu_send(Gpu *g, uint16_t opcode, const uint8_t *payload,
              uint32_t length, uint32_t *sequence) {
    uint8_t header[16];
    uint32_t i, seq;
    if (g->failed || !g->transport.write_byte || !sequence ||
        opcode < 1 || opcode > 8 || length > GPU_MAX_PAYLOAD ||
        (length && !payload) || g->next_sequence == 0 ||
        g->next_sequence == UINT32_MAX) return false;
    seq = g->next_sequence++; /* Reserve the final value for planned recovery. */
    gpu_put32(header, UINT32_C(0x5550474e));
    header[4] = 0; header[5] = 2;
    gpu_put16(header + 6, opcode); gpu_put32(header + 8, length);
    gpu_put32(header + 12, seq);
    for (i = 0; i < 16; ++i)
        if (!g->transport.write_byte(g->transport.user, header[i])) return gpu_fail(g);
    for (i = 0; i < length; ++i)
        if (!g->transport.write_byte(g->transport.user, payload[i])) return gpu_fail(g);
    *sequence = seq; return true;
}
bool gpu_reply(Gpu *g, uint16_t opcode, uint32_t sequence,
               uint32_t token, uint32_t extra_bytes, GpuReply *reply) {
    uint8_t header[16];
    uint32_t length, epoch;
    uint16_t returned_opcode;
    if (!reply || extra_bytes > GPU_MAX_REPLY_PAYLOAD - 12 ||
        !gpu_read_bytes(g, header, 16)) return false;
    length = gpu_get32(header + 8);
    returned_opcode = (uint16_t)((uint16_t)header[6] | ((uint16_t)header[7] << 8));
    if (gpu_get32(header) != UINT32_C(0x5550474e) || header[4] != 0 || header[5] != 2 ||
        returned_opcode != (uint16_t)(opcode | UINT16_C(0x8000)) ||
        gpu_get32(header + 12) != sequence || length != 12 + extra_bytes)
        return gpu_fail(g);
    if (!gpu_read_bytes(g, reply->payload, length)) return false;
    epoch = gpu_get32(reply->payload);
    if (!epoch || (g->epoch && epoch != g->epoch) ||
        gpu_get32(reply->payload + 4) != 0 || gpu_get32(reply->payload + 8) != token)
        return gpu_fail(g);
    g->epoch = epoch; reply->payload_bytes = length; return true;
}
bool gpu_query(Gpu *g, uint16_t opcode, uint8_t result[64]) {
    uint32_t sequence;
    GpuReply reply;
    if ((opcode != GPU_GET_CAPS && opcode != GPU_GET_STATUS) ||
        !gpu_send(g, opcode, NULL, 0, &sequence) ||
        !gpu_reply(g, opcode, sequence, 0, 64, &reply)) return false;
    memcpy(result, reply.payload + 12, 64); return true;
}
bool gpu_start_session(Gpu *g, GpuTransport transport, unsigned max_polls,
                       uint8_t caps[64], uint8_t status[64]) {
    unsigned poll;
    gpu_init(g, transport);
    for (poll = 0; poll < max_polls; ++poll) {
        if (!gpu_snapshot(g, status)) return false;
        if ((status[1] & UINT8_C(0x88)) || status[56]) return gpu_fail(g);
        if (status[1] & 1) {
            g->epoch = gpu_get32(status + 2);
            if (!g->epoch || !gpu_query(g, GPU_GET_CAPS, caps)) return false;
            return caps[0] == 0 && caps[1] == 2 && caps[2] == 8 && caps[3] == 32 &&
                   gpu_get32(caps + 4) == GPU_MEMORY_BYTES &&
                   gpu_get32(caps + 8) == GPU_HEAP_BASE &&
                   gpu_get32(caps + 12) == GPU_MAX_PAYLOAD &&
                   gpu_get32(caps + 16) == GPU_RECORD_BYTES;
        }
    }
    return false;
}
bool gpu_upload(Gpu *g, uint32_t address, const uint8_t *data, uint32_t length) {
    uint8_t payload[GPU_MAX_PAYLOAD];
    uint32_t cursor = 0, sequence;
    if (!gpu_heap_range(address, length) || (length && !data)) return false;
    while (cursor < length) {
        uint32_t chunk = length - cursor, source = 0, payload_bytes = 8;
        if (chunk > GPU_MAX_UPLOAD_DECODED) chunk = GPU_MAX_UPLOAD_DECODED;
        gpu_put32(payload, address + cursor); gpu_put32(payload + 4, chunk);
        while (source < chunk) {
            uint32_t descriptor = payload_bytes++, slot;
            payload[descriptor] = 0;
            for (slot = 0; slot < 4 && source < chunk; ++slot) {
                uint32_t word = 0, valid = chunk - source, byte, code, count;
                if (valid > 4) valid = 4;
                for (byte = 0; byte < valid; ++byte)
                    word |= (uint32_t)data[cursor + source + byte] << (8 * byte);
                source += valid;
                if (word == 0) code = 0;
                else if ((word & UINT32_C(0xffffff80)) == 0 ||
                         (word & UINT32_C(0xffffff80)) == UINT32_C(0xffffff80)) code = 1;
                else if ((word & UINT32_C(0xffff8000)) == 0 ||
                         (word & UINT32_C(0xffff8000)) == UINT32_C(0xffff8000)) code = 2;
                else code = 3;
                payload[descriptor] |= (uint8_t)(code << (2 * slot));
                count = code == 3 ? 4 : code;
                for (byte = 0; byte < count; ++byte)
                    payload[payload_bytes++] = (uint8_t)(word >> (8 * byte));
            }
        }
        if (!gpu_send(g, GPU_WRITE_HEAP, payload, payload_bytes, &sequence)) return false;
        cursor += chunk;
    }
    return true; /* A zero-length CPU upload emits no packet. */
}
bool gpu_fence(Gpu *g, uint32_t token) {
    uint8_t payload[4]; uint32_t sequence; GpuReply reply;
    gpu_put32(payload, token);
    return gpu_send(g, GPU_FENCE, payload, 4, &sequence) &&
           gpu_reply(g, GPU_FENCE, sequence, token, 0, &reply);
}
bool gpu_present(Gpu *g, uint32_t target, uint32_t token) {
    uint8_t payload[8]; uint32_t sequence; GpuReply reply;
    if (target > 1) return false;
    gpu_put32(payload, target); gpu_put32(payload + 4, token);
    return gpu_send(g, GPU_PRESENT, payload, 8, &sequence) &&
           gpu_reply(g, GPU_PRESENT, sequence, token, 0, &reply);
}
bool gpu_read_memory(Gpu *g, uint32_t address, uint32_t length,
                     uint32_t token, uint8_t *data) {
    uint8_t payload[12]; uint32_t sequence; GpuReply reply;
    if (!data || !length || length > 512 || (uint64_t)address + length > GPU_MEMORY_BYTES)
        return false;
    gpu_put32(payload, address); gpu_put32(payload + 4, length); gpu_put32(payload + 8, token);
    if (!gpu_send(g, GPU_READ_MEMORY, payload, 12, &sequence) ||
        !gpu_reply(g, GPU_READ_MEMORY, sequence, token, 8 + length, &reply)) return false;
    if (gpu_get32(reply.payload + 12) != address || gpu_get32(reply.payload + 16) != length)
        return gpu_fail(g);
    memcpy(data, reply.payload + 20, length); return true;
}
void gpu_put_bounds(uint8_t *p, GpuBounds bounds) {
    gpu_put32(p, (uint32_t)bounds.x0); gpu_put32(p + 4, (uint32_t)bounds.y0);
    gpu_put32(p + 8, (uint32_t)bounds.x1); gpu_put32(p + 12, (uint32_t)bounds.y1);
}
void gpu_record_base(uint8_t record[320], uint32_t kind, uint32_t flags) {
    GpuBounds viewport = {0, 0, 640, 480};
    memset(record, 0, 320);
    gpu_put32(record, kind | UINT32_C(0x100) | (flags << 16));
    gpu_put_bounds(record + 16, viewport);
}
void gpu_make_rect(uint8_t record[320], GpuBounds bounds, uint32_t rgb888) {
    gpu_record_base(record, 1, 1); /* color write; no depth test/write or key */
    gpu_put32(record + 8, rgb888 & UINT32_C(0xffffff));
    gpu_put_bounds(record + 32, bounds);
}
void gpu_make_example_triangle(uint8_t record[320]) {
    const int32_t vertex[6] = {0, 0, 256, 0, 0, 256}; /* Q4: (0,0),(16,0),(0,16) */
    const int32_t edges[3][4] = {{0, 256, 0, 1}, {-256, -256, 65536, 0}, {256, 0, 0, 1}};
    GpuBounds bounds = {0, 0, 16, 16};
    unsigned i, j;
    gpu_record_base(record, 0, 7); /* color+depth test+depth write, LESS, FLAT */
    gpu_put32(record + 8, UINT32_C(0x2040ff));
    gpu_put_bounds(record + 32, bounds);
    for (i = 0; i < 6; ++i) gpu_put32(record + 80 + 4 * i, (uint32_t)vertex[i]);
    for (i = 0; i < 3; ++i) for (j = 0; j < 4; ++j)
        gpu_put32(record + 104 + 16 * i + 4 * j, (uint32_t)edges[i][j]);
    gpu_put64(record + 152, INT64_C(0x4000000000)); /* Constant D24 0x400000, Q16. */
}
bool gpu_make_sprite(uint8_t record[320], GpuBounds bounds, int32_t source_x,
                     int32_t source_y, uint32_t base, uint32_t width,
                     uint32_t height, uint32_t pitch, bool key_on, uint16_t key) {
    int64_t draw_width = (int64_t)bounds.x1 - bounds.x0;
    int64_t draw_height = (int64_t)bounds.y1 - bounds.y0;
    if (!width || !height || width > 1024 || height > 1024 ||
        (width & (width - 1)) || (height & (height - 1)) ||
        (base & 3) || (pitch & 3) || pitch < width * 2 ||
        (uint64_t)base + (uint64_t)pitch * height > GPU_MEMORY_BYTES || base < GPU_HEAP_BASE ||
        source_x < 0 || source_y < 0 || draw_width < 0 || draw_height < 0 ||
        (draw_width && draw_height &&
         ((int64_t)source_x + draw_width > width || (int64_t)source_y + draw_height > height)))
        return false;
    gpu_record_base(record, 3, 1u | (key_on ? 8u : 0u));
    gpu_put32(record + 4, 2u | (1u << 18)); /* REPLACE + descriptor present */
    gpu_put32(record + 12, key_on ? key : 0);
    gpu_put_bounds(record + 32, bounds);
    gpu_put32(record + 48, base); gpu_put32(record + 52, width);
    gpu_put32(record + 56, height); gpu_put32(record + 60, pitch);
    gpu_put32(record + 68, (uint32_t)source_x); gpu_put32(record + 72, (uint32_t)source_y);
    return true;
}
bool gpu_submit_one(Gpu *g, uint32_t target, uint16_t clear565,
                    const uint8_t *record, GpuBounds bounds) {
    uint8_t table[2400] = {0}, refs[1200] = {0}, descriptor[32] = {0}, payload[4];
    int32_t x0 = bounds.x0 < 0 ? 0 : bounds.x0, y0 = bounds.y0 < 0 ? 0 : bounds.y0;
    int32_t x1 = bounds.x1 > 640 ? 640 : bounds.x1, y1 = bounds.y1 > 480 ? 480 : bounds.y1;
    uint32_t sequence;
    if (target > 1) return false;
    if (record) {
        int32_t tx, ty;
        if (!gpu_upload(g, GPU_RECORD_ADDR, record, 320)) return false;
        if (x0 < x1 && y0 < y1) for (ty = y0 / 32; ty <= (y1 - 1) / 32; ++ty)
            for (tx = x0 / 32; tx <= (x1 - 1) / 32; ++tx) {
                uint32_t tile = (uint32_t)ty * 20u + (uint32_t)tx;
                gpu_put32(refs + 4 * tile, GPU_RECORD_ADDR);
                gpu_put32(table + 8 * tile, GPU_REFS_ADDR + 4 * tile);
                gpu_put32(table + 8 * tile + 4, 1);
            }
        if (!gpu_upload(g, GPU_REFS_ADDR, refs, sizeof(refs))) return false;
    }
    if (!gpu_upload(g, GPU_TABLE_ADDR, table, sizeof(table))) return false;
    gpu_put32(descriptor, UINT32_C(0x3153504e)); gpu_put32(descriptor + 4, target);
    gpu_put32(descriptor + 8, 3); /* color CLEAR + depth CLEAR */
    gpu_put32(descriptor + 12, clear565); gpu_put32(descriptor + 16, UINT32_C(0xffffff));
    gpu_put32(descriptor + 20, GPU_TABLE_ADDR); gpu_put32(descriptor + 24, GPU_TILES);
    if (!gpu_upload(g, GPU_PASS_ADDR, descriptor, sizeof(descriptor))) return false;
    gpu_put32(payload, GPU_PASS_ADDR);
    return gpu_send(g, GPU_SUBMIT_PASS, payload, 4, &sequence);
}
bool gpu_patch_bounds(Gpu *g, uint32_t record_address, GpuBounds bounds) {
    uint8_t bytes[16];
    if ((record_address & 3) || !gpu_heap_range(record_address, 320)) return false;
    gpu_put_bounds(bytes, bounds);
    return gpu_upload(g, record_address + 32, bytes, sizeof(bytes));
}
```
<!-- gpu-control-c99:end -->

### Using the helpers

The snippet below uses the C99 helpers above. `transport` is the application's
initialized `GpuTransport`, and its physical link must already be fresh and
quiesced. A real firmware HAL must also implement reset/abort and timeouts.

```c
/* Inside the caller's application, with a supplied GpuTransport transport: */
Gpu gpu;
uint8_t caps[64], status[64], record[320];
GpuBounds bounds = {40, 60, 160, 120};
uint32_t target;

if (!gpu_start_session(&gpu, transport, 1000, caps, status)) {
    /* Inspect sideband faults, then recover the physical link. */
    return false;
}
target = 1u - status[14];

/* A complete blue clear: all 300 bins are empty. */
if (!gpu_submit_one(&gpu, target, 0x001f, NULL, bounds) ||
    !gpu_fence(&gpu, 100) || !gpu_present(&gpu, target, 101)) return false;
target = 1u - target;

/* Flat orange rectangle over a cleared black background. */
gpu_make_rect(record, bounds, 0x0080ff); /* R=255, G=128, B=0. */
if (!gpu_submit_one(&gpu, target, 0, record, bounds) ||
    !gpu_fence(&gpu, 102) || !gpu_present(&gpu, target, 103)) return false;
target = 1u - target;

/* A concrete prepared triangle, requiring no floating-point setup library. */
gpu_make_example_triangle(record);
bounds.x0 = 0; bounds.y0 = 0; bounds.x1 = 16; bounds.y1 = 16;
if (!gpu_submit_one(&gpu, target, 0, record, bounds) ||
    !gpu_fence(&gpu, 104) || !gpu_present(&gpu, target, 105)) return false;
target = 1u - target;
```

For a keyed sprite, upload a 4×4 RGB565 bitmap once, then reuse its heap address:

```c
uint8_t pixels[32]; /* 4 pixels * 2 bytes * 4 rows, pitch 8. */
unsigned i;
GpuBounds sprite_bounds = {100, 80, 104, 84};
for (i = 0; i < 16; ++i) gpu_put16(pixels + 2 * i, (i & 1) ? 0x07e0 : 0xf81f);
if (!gpu_upload(&gpu, GPU_TEXTURE_ADDR, pixels, sizeof(pixels)) ||
    !gpu_make_sprite(record, sprite_bounds, 0, 0, GPU_TEXTURE_ADDR, 4, 4, 8, true, 0xf81f) ||
    !gpu_submit_one(&gpu, target, 0, record, sprite_bounds) ||
    !gpu_fence(&gpu, 106) || !gpu_present(&gpu, target, 107)) return false;
target = 1u - target;
```

These snippets continue the same application, toggling `target` after each
successful presentation so it identifies the currently writable physical buffer.
The magenta key texels preserve the cleared background. The sprite is raw
replacement with depth disabled; it does not require new triangle setup.

A framebuffer copy uses a zero-initialized 56-byte payload. Continuing after
the successful sprite presentation above, this copies a 64×64 region from the
current front to the writable back, preserving other back-buffer contents:

```c
uint8_t copy[56] = {0};
uint32_t copy_sequence;
gpu_put32(copy + 4, 1u - target); /* source kind 0: current physical front */
gpu_put32(copy + 8, target);
gpu_put32(copy + 12, 20); gpu_put32(copy + 16, 30);  /* source X/Y */
gpu_put32(copy + 20, 200); gpu_put32(copy + 24, 100); /* destination X/Y */
gpu_put32(copy + 28, 64); gpu_put32(copy + 32, 64);   /* width/height */
if (!gpu_send(&gpu, GPU_COPY_RECT, copy, sizeof(copy), &copy_sequence) ||
    !gpu_fence(&gpu, 108) || !gpu_present(&gpu, target, 109)) return false;
target = 1u - target;
```

### Moving a sprite

There is no sprite-position register or persistent sprite overlay. After the
prior pass's fence, a driver may use `gpu_patch_bounds()` to update the four
destination coordinates, then update affected tile-table/list references and
submit a new pass. Keep width/height fixed for a pure move, preserve source
origin, and recheck source extent. Crossing a 32×32 tile boundary can require
different references. A bounds patch alone neither redraws nor presents.
This destination-bounds helper is suitable for rectangles and sprites. Moving
a triangle also changes its vertices, edges and attribute-plane setup; patching
only its bounds does not move its geometry.

For the small example, rebuild `record` with a new destination and call
`gpu_submit_one()` again: it regenerates the bins and clears the background,
so no old sprite trail remains. Reuse `GPU_TEXTURE_ADDR`; do not upload the
bitmap merely because its screen position changed. An efficient scene driver
can update a bounded dirty region, but must restore its old background and
maintain the real back buffer's contents/ownership. Active records are never
patched while the GPU is still using them.

### What these examples do not provide

The examples are packet/control code and small CPU-built records. They do not
provide an RP2350 GPIO/PIO program, electrical timing closure, a complete
allocator, a scene engine, a triangle setup port, or retained hardware sprites.
The repository's desktop host compiler is available in
[`protocol.hpp`](../reference/include/nano_gpu/hw/protocol.hpp) and
[`hw_protocol.cpp`](../reference/src/hw_protocol.cpp), and actual RTL bus driving,
reply handling and differential tests are in
[`reference/rtl/main.cpp`](../reference/rtl/main.cpp). Those implementations are
useful references, but their host language/runtime requirements must be ported
deliberately to the RP2350.

Run `make gpu-docs-test` to compile the marked block as strict C99, compare
packet bytes with the desktop encoder, and execute the examples through host
pins in the clocked C++ `SystemModel`. This checks the documented control code;
it is not a Verilator RTL-equivalence result or a board timing measurement.

## 10. Sources of truth when changing the interface

- [`nano_host_link.sv`](../rtl/gpu/nano_host_link.sv): wire-edge behavior, packet
  publication, capacities, sideband reads and parser faults.
- [`nano_gpu_core.sv`](../rtl/gpu/nano_gpu_core.sv): implemented commands,
  payload checks, capability/status bytes, sequencing, replies and recovery.
- [`nano_tile_renderer.sv`](../rtl/gpu/nano_tile_renderer.sv): primitive fields,
  supported modes and hardware-side record checks.
- [`protocol.hpp`](../reference/include/nano_gpu/hw/protocol.hpp) and its encoder:
  desktop serialization and validation reference.
- [GPU_ARCHITECTURE.md](GPU_ARCHITECTURE.md): supported feature set, memory and
  pipeline behavior that the control guide must describe consistently.

Change these two primary guides whenever a command, field, signal, ordering
rule, memory layout, supported feature or architecture changes. Update examples
and their checks with the implementation, then commit and push the guides with
that change. Older documents under `docs/` retain design history and detailed
derivations; where an M0 sketch differs from the implemented interface above,
this guide describes the current CPU contract.
