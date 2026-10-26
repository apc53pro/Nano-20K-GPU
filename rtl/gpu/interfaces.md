# Integrated RTL simulation interfaces

These interfaces connect functional RTL modules. They are the logical simulation
and synthesis boundary; they do not finalize RP2350 board pin assignments.
Every module uses rising-edge registered behavior. `reset` is active high.

## Native RAM

`nano_sdp16 #(INIT_FILE="")`: `wclk,wce,we,waddr[9:0],wdata[15:0]`,
`rclk,rce,re,raddr[9:0],reset_output,rdata[15:0]`. The output is synchronous;
resetting it does not clear RAM. Simulation models the primitive only;
`SYNTHESIS` binds the same ports to a Gowin SDPB in 1024x16 mode.

## Memory client

All clients use these ports with prefix `mem_`:

| Direction from client | Signal | Meaning |
| --- | --- | --- |
| Out | `cmd_valid`, `cmd_write`, `cmd_addr[22:0]`, `cmd_words[4:0]` | Byte address, word aligned, 1..16 words |
| In | `cmd_ready` | Command acceptance |
| Out / In | `w_valid` / `w_ready` | Write payload handshake after command acceptance |
| Out | `w_data[31:0]`, `w_strb[3:0]` | Word and byte enables |
| In / Out | `r_valid` / `r_ready` | Read response handshake |
| In | `r_data[31:0]`, `r_last` | Read word and final-word indicator |
| In | `done`, `error` | One-cycle completion; all words have retired |

One transaction per client may be outstanding. The client retains command and
payload signals while blocked. The memory service owns a bounded native RAM
slot, stages writes before issuing unpausable SDRAM bursts, and holds read
payload under response backpressure. No client has a C++ memory access path.
The service flattens vectors for six clients: 0 scanout, 1 host upload, 2 command
fetch/readback, 3 tile transfers, 4 texture cache, 5 copies. Data-word lane zero
is the least-significant slice. Its physical interface exposes SDRAM commands,
bank/address, DQM, DQ input/output/enable and clock/CKE.

The host-upload client assembles each bounded 64-byte chunk in one native SDPB
before requesting the shared service. Its payload can then advance every two
core clocks; host byte decoding never holds the SDRAM transaction owner.

## Texture cache

`nano_texture_cache`: `clk,reset,flush`; configuration handshake
`cfg_valid,cfg_ready,cfg_base[22:0],cfg_width[10:0],cfg_height[10:0],cfg_pitch[22:0],
cfg_repeat_u,cfg_repeat_v`; texel request `req_valid,req_ready,req_x[31:0],
req_y[31:0],req_tag[5:0]` (signed integer coordinates); response
`rsp_valid,rsp_ready,rsp_texel[15:0],rsp_tag[5:0]`; sticky `fault`.
Configuration and flush occur while drained. The memory client is read-only.

## Tile renderer

`nano_tile_renderer`: `clk,reset`; serial command loading
`cmd_word_we,cmd_word_index[6:0],cmd_word_data[31:0]` for the existing eighty-word
primitive record. `cmd_valid,cmd_ready,cmd_tile_x[9:0],cmd_tile_y[8:0]` starts a
primitive; `busy,done,fault` expose completion. Loading is legal only when idle.

Native attachment access while idle uses `pix_we,pix_addr[9:0],pix_color[15:0],
pix_depth[23:0]` and `pix_re,pix_raddr[9:0]` with registered
`pix_rvalid,pix_rcolor[15:0],pix_rdepth[23:0]`. The renderer has texture-cache
configuration/request/response ports with prefix `tex_` and the same names and
widths as the cache above. `tex_flush` is a separate idle control input.

## Host transport

`nano_host_link`: `clk,reset,host_clk,core_abort`; external ports
`host_wr_valid,host_wr_data[7:0],host_wr_ready,host_rd_req,host_reg_addr[6:0],
host_rd_data[7:0],host_rd_valid`. Host edges are independent of core edges.
Status and capabilities are `status_snapshot[511:0],capabilities[511:0]`.
Core command lease: `cmd_valid,cmd_opcode[15:0],cmd_sequence[31:0],
cmd_payload_length[10:0]`, input `cmd_pop`; registered payload access
`payload_req,payload_offset[9:0]` -> `payload_valid,payload_data[7:0]`.
Replies enter `response_valid,response_ready,response_data[7:0]`.
`fault[7:0]` and `recovering` are core-domain outputs.

## Scanout and integrated core

`nano_scanout`: `clk,reset,pixel_ce,memory_ready,front_id`; RGB565 output
`pixel[15:0],hsync,vsync,de,x[9:0],y[8:0],vblank_pulse` and sticky/counted
underflow diagnostics. It owns memory client zero. Scanout always advances at
the pixel cadence, independently of rendering or host activity.
It offers the next refill while the current transaction retires, reserving
FIFO space for both. This keeps scanout eligible when the arbiter becomes idle.
An old completion and a new command acceptance may share an edge; the new
transaction remains outstanding and its generation is discarded if a front
swap occurs on that edge.

`nano_gpu_core` exposes core `clk,reset`, the host ports above, input `abort_request`,
physical SDRAM ports, and logical video outputs `video_rgb565[15:0],
video_hsync,video_vsync,video_de,video_x[9:0],video_y[8:0],video_pixel_ce`.
Top-level `busy,fault,fault_code[7:0]` describe device state. A production board
wrapper must add its actual clock/reset generation, standard video-output
encoding/serialization and pin constraints before full-device fit can qualify.
