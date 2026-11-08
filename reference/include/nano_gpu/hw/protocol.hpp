#pragma once

#include "nano_gpu/hw/host_link.hpp"
#include "nano_gpu/hw/resources.hpp"
#include "nano_gpu/reference.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace nano_gpu::hw {

// Provisional simulation ABI N20K-SYS-0.2, not a released RP2350 driver ABI.
// Every integer is little-endian; every heap structure is four-byte aligned.
constexpr std::size_t packet_header_bytes = 16;
constexpr std::size_t maximum_packet_payload = 1024;
static_assert(maximum_packet_payload == host_max_payload_bytes);
// Worst case:239 literal words plus60 descriptors =1016 encoded bytes.
constexpr std::size_t maximum_upload_decoded_bytes = 956;
constexpr std::size_t primitive_record_bytes = 320;
constexpr std::size_t pass_descriptor_bytes = 32;
constexpr std::size_t tile_entry_bytes = 8;
constexpr std::uint32_t pass_descriptor_magic = 0x3153504eu; // "NPS1"
constexpr std::uint32_t maximum_pass_references = 1u << 20;
constexpr std::uint32_t maximum_primitive_records = 16384;

// Primitive record offsets:
//   0 u32: kind[7:0] (Triangle=0,Rect=1,Line=2,Sprite=3), version[15:8]=1,
//          color-write/depth-test/depth-write/key-enabled bits[19:16].
//   4 u32: ShadeMode[7:0], DepthFunc[15:8], repeat-U/V bits16/17,
//          texture-descriptor-present bit18. All other bits are zero.
//   8 u32: R,G,B in bits0..23; 12 u32: raw RGB565 key in bits0..15.
//  16 i32[4]: scissor; 32 i32[4]: bounds (zero for a Line).
//  48 u32[4]: texture base,width,height,pitch; all zero if absent.
//  64 u32: constant D24 depth (zero for a Triangle).
//  68/72 i32: Sprite source X/Y, or Line endpoint A X/Y.
//  76 u32: reserved zero; for a Line, endpoint B is at80/84.
//  80 i32[6]: Triangle snapped Q4 X/Y pairs (except Line B as above).
// 104 i32/u32[12]: three {a,b,c,inclusive} edges, inclusive is 0 or1.
// 152 i64[21]: {seed,step_x,step_y} for z,r,g,b,q,u,v.
// Unused kind-specific bytes MUST be zero.
//
// Pass descriptor: magic,target,load flags(bit0=colorCLEAR,bit1=depthCLEAR,
// bit2=depthDISCARD: depth is never stored and becomes invalid,
// bit3=SKIP: tiles with no references keep their contents untouched),
// clearRGB565,clearD24,tileTableAddress,tileCount(300),reservedZero (all u32).
// Tile table: 300 {listAddress,count} u32 pairs. Empty lists have address zero.
// Each nonempty list is an ordered array of u32 primitive-record addresses.
struct PassDescriptor {
    int target = 1;
    LoadOp color_load = LoadOp::Clear;
    LoadOp depth_load = LoadOp::Clear;
    std::uint16_t clear_color = 0;
    std::uint32_t clear_depth = depth_max;
    std::uint32_t tile_table_address = 0;
    StoreOp depth_store = StoreOp::Store;
    bool skip_empty_tiles = false;
};
struct TileEntry {
    std::uint32_t address = 0;
    std::uint32_t count = 0;
};
struct DecodedPrimitive {
    Primitive primitive;
    std::optional<TextureDescriptor> texture;
};

std::uint16_t read_u16(const std::vector<std::uint8_t>& bytes, std::size_t offset);
std::uint32_t read_u32(const std::vector<std::uint8_t>& bytes, std::size_t offset);
std::int32_t read_i32(const std::vector<std::uint8_t>& bytes, std::size_t offset);
std::int64_t read_i64(const std::vector<std::uint8_t>& bytes, std::size_t offset);
void write_u16(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint16_t value);
void write_u32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value);
void write_i32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::int32_t value);
void write_i64(std::vector<std::uint8_t>& bytes, std::size_t offset, std::int64_t value);

void validate_heap_range(std::uint32_t address, std::uint64_t length,
                         unsigned alignment = 4);
std::uint32_t texture_extent(const TextureDescriptor& texture);
std::vector<std::uint8_t> encode_packet(Opcode opcode, std::uint32_t sequence,
                                       const std::vector<std::uint8_t>& payload = {});
struct HeapUpload {
    std::uint32_t address = 0;
    std::vector<std::uint8_t> bytes;
};
// The sole WRITE_HEAP payload representation: address, decoded byte count,
// then descriptor/payload groups of up to four little-endian u32 words.
// Codes0/1/2/3 denote zero/sign-extended8/sign-extended16/literal32. The last
// word is zero-padded by the encoder; only the declared bytes are written.
// Unused final descriptor codes must be zero; no trailing bytes are accepted.
std::vector<std::uint8_t> encode_heap_upload(std::uint32_t address,
                                          const std::vector<std::uint8_t>& bytes);
HeapUpload decode_heap_upload(const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> encode_primitive(const Primitive& primitive,
                                         std::optional<TextureDescriptor> texture = {});
DecodedPrimitive decode_primitive(const std::vector<std::uint8_t>& bytes);
std::vector<std::uint8_t> encode_pass_descriptor(const PassDescriptor& descriptor);
PassDescriptor decode_pass_descriptor(const std::vector<std::uint8_t>& bytes);
TileEntry decode_tile_entry(const std::vector<std::uint8_t>& bytes);

// COPY_RECT payload is 56 bytes, all fields u32/i32 in this order:
// sourceKind(0=physicalFramebuffer,1=heapImage),sourceBuffer,targetBuffer,
// sourceX,sourceY,destinationX,destinationY,width,height,
// textureDescriptor{base,width,height,pitch},reservedZero.
struct CopyCommand {
    bool image_source = false;
    int source = 0;
    int target = 1;
    Point source_origin{};
    Point destination_origin{};
    int width = 0;
    int height = 0;
    TextureDescriptor image{};
};
constexpr std::size_t copy_payload_bytes = 56;
std::vector<std::uint8_t> encode_copy_command(const CopyCommand& command);
CopyCommand decode_copy_command(const std::vector<std::uint8_t>& bytes);

// CPU-side program memory is not GPU storage. compile_pass serializes/uploads
// all resources and records once, then SUBMIT_PASS and FENCE(token=sequence).
// WRITE_HEAP uses encode_heap_upload(); all uploads have the same encoding.
// SUBMIT_PASS payload: u32 descriptorAddress; FENCE payload: u32 hostToken.
struct HostProgram {
    std::vector<std::uint8_t> bytes;
    std::uint32_t descriptor_address = 0;
    std::uint32_t next_sequence = 1;
    std::uint32_t fence_sequence = 0;
    std::uint32_t fence_token = 0;
    std::uint64_t upload_bytes = 0;
    std::uint64_t reference_count = 0;
    std::uint64_t heap_bytes_used = 0;
    std::uint64_t texture_upload_bytes = 0;
    std::uint64_t resident_texture_bytes = 0;
};
HostProgram compile_pass(const Pass& pass, std::uint32_t first_sequence = 1);

// CPU driver resource cache, not additional FPGA memory. Texture snapshots are
// compared exactly, including detection of writes through a mutable Image alias.
// Textures occupy a persistent heap prefix; records/bins use its transient tail.
// There is one outstanding program: call complete() only after its matching
// successful FENCE. compile() then safely reuses transient storage. Call reset()
// after abort/reset, failed delivery, or external writes into this allocator's
// heap. Before reusing addresses, the caller must quiesce older device work.
// The device must invalidate texture tags before executing a newly uploaded
// pass: changed contents and heap compaction can reuse a physical descriptor.
class HostPassCompiler {
public:
    HostPassCompiler();
    ~HostPassCompiler();
    HostPassCompiler(const HostPassCompiler&) = delete;
    HostPassCompiler& operator=(const HostPassCompiler&) = delete;
    HostPassCompiler(HostPassCompiler&&) noexcept;
    HostPassCompiler& operator=(HostPassCompiler&&) noexcept;
    HostProgram compile(const Pass& pass, std::uint32_t first_sequence = 1);
    void complete();
    void reset();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nano_gpu::hw
