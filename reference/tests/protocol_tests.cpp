#include "nano_gpu/hw/protocol.hpp"

#include <algorithm>
#include <array>
#include <functional>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>

namespace {
using namespace nano_gpu;
using namespace nano_gpu::hw;

unsigned cases = 0;
unsigned failures = 0;
void expect(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Operation> void expect_throws(Operation&& operation, const std::string& message) {
    bool threw = false;
    try { operation(); } catch (const std::exception&) { threw = true; }
    expect(threw, message);
}
template<class Operation> void run_case(const char* name, Operation&& operation) {
    ++cases;
    try { operation(); }
    catch (const std::exception& error) {
        ++failures;
        std::cerr << "FAIL " << name << ": " << error.what() << '\n';
    }
}

std::shared_ptr<Image> pattern() {
    auto image = std::make_shared<Image>(4, 4, 12);
    for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x)
        image->at(x, y) = static_cast<std::uint16_t>(0x1000u + 0x100u * y + x);
    return image;
}
Triangle triangle(ShadeMode shade, const std::shared_ptr<Image>& image = {}) {
    RenderState state;
    state.shade = shade;
    state.color = {101, 202, 33};
    state.texture = image;
    state.address_u = AddressMode::Repeat;
    state.address_v = AddressMode::Clamp;
    state.depth_func = DepthFunc::LessEqual;
    const std::array<ScreenVertex, 3> vertices{{
        {4, 4, 0.2L, 1, -1.5L, 0.25L, 10, 40, 100},
        {60, 8, 0.7L, 2, 3.25L, 1.5L, 200, 60, 120},
        {9, 55, 0.4L, 3, 0.5L, 4.5L, 100, 240, 80}}};
    const auto prepared = setup_triangle(vertices, state);
    expect(prepared.has_value(), "test triangle unexpectedly culled");
    return *prepared;
}
std::vector<std::uint8_t> slice(const std::vector<std::uint8_t>& heap, std::uint32_t address,
                               std::size_t size) {
    expect(address >= heap_base, "heap slice starts below heap");
    const auto offset = static_cast<std::size_t>(address - heap_base);
    expect(offset <= heap.size() && size <= heap.size() - offset, "heap slice exceeds upload");
    return {heap.begin() + static_cast<std::ptrdiff_t>(offset),
             heap.begin() + static_cast<std::ptrdiff_t>(offset + size)};
}
struct AppliedProgram {
    std::vector<std::uint8_t> heap;
    unsigned uploads = 0;
    unsigned submits = 0;
    unsigned fences = 0;
};
AppliedProgram apply_host_program(const HostProgram& program, std::uint32_t first_sequence,
                                 const std::vector<std::uint8_t>& previous_heap = {}) {
    AppliedProgram result;
    result.heap = previous_heap;
    result.heap.resize(static_cast<std::size_t>(program.heap_bytes_used));
    std::size_t cursor = 0;
    auto sequence = first_sequence;
    while (cursor < program.bytes.size()) {
        expect(program.bytes.size() - cursor >= packet_header_bytes, "truncated CPU packet header");
        const std::vector<std::uint8_t> header(program.bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
            program.bytes.begin() + static_cast<std::ptrdiff_t>(cursor + packet_header_bytes));
        expect(header[0] == 'N' && header[1] == 'G' && header[2] == 'P' && header[3] == 'U' &&
               header[4] == 0 && header[5] == 2, "CPU program has invalid NGPU envelope");
        const auto length = read_u32(header, 8);
        expect(length <= maximum_packet_payload && program.bytes.size() - cursor - packet_header_bytes >= length,
               "CPU program has an unbounded or truncated packet body");
        expect(read_u32(header, 12) == sequence++, "CPU program lost request sequence order");
        const auto payload_begin = cursor + packet_header_bytes;
        const std::vector<std::uint8_t> payload(program.bytes.begin() + static_cast<std::ptrdiff_t>(payload_begin),
            program.bytes.begin() + static_cast<std::ptrdiff_t>(payload_begin + length));
        const auto opcode = static_cast<Opcode>(read_u16(header, 6));
        if (opcode == Opcode::WriteHeap) {
            ++result.uploads;
            expect(result.submits == 0 && length >= 8, "upload appeared after pass submission");
            const auto upload = decode_heap_upload(payload);
            const auto address = upload.address;
            const auto bytes = upload.bytes.size();
            expect(bytes % 4 == 0, "compiled upload is not word aligned");
            validate_heap_range(address, bytes);
            const auto offset = static_cast<std::size_t>(address - heap_base);
            expect(offset <= result.heap.size() && bytes <= result.heap.size() - offset,
                   "upload outside CPU program's declared heap use");
            std::copy(upload.bytes.begin(), upload.bytes.end(), result.heap.begin() + static_cast<std::ptrdiff_t>(offset));
        } else if (opcode == Opcode::SubmitPass) {
            ++result.submits;
            expect(length == 4 && result.submits == 1 && result.fences == 0 &&
                   read_u32(payload, 0) == program.descriptor_address, "incorrect pass submission");
        } else if (opcode == Opcode::Fence) {
            ++result.fences;
            expect(length == 4 && result.submits == 1 && result.fences == 1 &&
                   read_u32(payload, 0) == program.fence_token && sequence - 1 == program.fence_sequence,
                   "incorrect completion fence identity");
        } else throw std::runtime_error("CPU pass program contains an unexpected opcode");
        cursor += packet_header_bytes + length;
    }
    expect(result.submits == 1 && result.fences == 1 && sequence == program.next_sequence,
           "CPU pass program did not end with submit/fence");
    return result;
}

TextureDescriptor first_texture(const HostProgram& program, const AppliedProgram& applied) {
    const auto descriptor = decode_pass_descriptor(slice(applied.heap,
        program.descriptor_address, pass_descriptor_bytes));
    for (unsigned tile = 0; tile < tile_count; ++tile) {
        const auto entry = decode_tile_entry(slice(applied.heap,
            descriptor.tile_table_address + tile * tile_entry_bytes, tile_entry_bytes));
        if (entry.count == 0) continue;
        const auto record = read_u32(slice(applied.heap, entry.address, 4), 0);
        const auto decoded = decode_primitive(slice(applied.heap, record, primitive_record_bytes));
        if (decoded.texture) return *decoded.texture;
    }
    throw std::runtime_error("test program has no textured primitive");
}

void byte_tests() {
    run_case("little-endian fields and signed endpoints have fixed wire representations", [] {
        std::vector<std::uint8_t> bytes(32, 0);
        write_u16(bytes, 0, 0x1234); write_u32(bytes, 2, 0x89abcdefu);
        write_i32(bytes, 6, std::numeric_limits<std::int32_t>::min());
        write_i64(bytes, 10, std::numeric_limits<std::int64_t>::min());
        write_i64(bytes, 18, std::numeric_limits<std::int64_t>::max());
        expect(bytes[0] == 0x34 && bytes[1] == 0x12 && bytes[2] == 0xef && bytes[5] == 0x89,
               "wire byte order changed");
        expect(bytes[6] == 0 && bytes[9] == 0x80 && bytes[10] == 0 && bytes[17] == 0x80 &&
               bytes[18] == 0xff && bytes[25] == 0x7f, "signed field bits changed");
        expect(read_u16(bytes, 0) == 0x1234 && read_u32(bytes, 2) == 0x89abcdefu &&
               read_i32(bytes, 6) == std::numeric_limits<std::int32_t>::min() &&
               read_i64(bytes, 10) == std::numeric_limits<std::int64_t>::min() &&
               read_i64(bytes, 18) == std::numeric_limits<std::int64_t>::max(), "signed endpoint decode failed");
        expect_throws([&] { read_u32(bytes, std::numeric_limits<std::size_t>::max()); }, "wrapped field read accepted");
        const auto before = bytes;
        expect_throws([&] { write_i64(bytes, 30, -1); }, "truncated field write accepted");
        expect(bytes == before, "invalid field write mutated bytes");
    });
    run_case("NGPU envelope matches independent byte vector", [] {
        const std::vector<std::uint8_t> expected{'N', 'G', 'P', 'U', 0, 2, 3, 0,
            4, 0, 0, 0, 0x78, 0x56, 0x34, 0x12, 0x01, 0x23, 0x45, 0x67};
        expect(encode_packet(Opcode::WriteHeap, 0x12345678u, {1, 0x23, 0x45, 0x67}) == expected,
               "NGPU header no longer matches published envelope");
        expect_throws([] { encode_packet(Opcode::Fence, 0); }, "zero request sequence accepted");
        expect_throws([] { encode_packet(static_cast<Opcode>(99), 1); }, "unknown opcode accepted");
        expect_throws([] { encode_packet(Opcode::WriteHeap, 1, std::vector<std::uint8_t>(1025)); },
                      "oversized packet accepted");
    });
    run_case("heap and image address checks reject wrap and attachment aliases", [] {
        validate_heap_range(heap_base, memory_bytes - heap_base);
        expect_throws([] { validate_heap_range(0, 4); }, "attachment alias accepted as heap");
        expect_throws([] { validate_heap_range(heap_base + 1, 4); }, "unaligned address accepted");
        expect_throws([] { validate_heap_range(0xfffffffcu, 8); }, "wrapped address accepted");
        expect_throws([] { validate_heap_range(heap_base, std::numeric_limits<std::uint64_t>::max()); },
                      "wrapped wide extent accepted");
        expect(texture_extent({memory_bytes - 4, 2, 1, 4}) == 4, "last legal image word rejected");
        expect(texture_extent({heap_base, 23, 17, 52}) == 884, "NPOT generic image rejected");
        expect_throws([] { texture_extent({heap_base, 1024, 1024, 0xfffffffcu}); }, "oversized stride accepted");
    });
}

void upload_codec_tests() {
    run_case("unified upload codec matches an independent four-width byte vector", [] {
        std::vector<std::uint8_t> bytes(16);
        write_u32(bytes, 0, 0); write_u32(bytes, 4, 0xffffff80u);
        write_u32(bytes, 8, 0x80); write_u32(bytes, 12, 0x12345678);
        const std::vector<std::uint8_t> expected{
            0x00, 0x80, 0x25, 0x00, 0x10, 0, 0, 0,
            0xe4, 0x80, 0x80, 0, 0x78, 0x56, 0x34, 0x12};
        expect(encode_heap_upload(heap_base, bytes) == expected,
               "zero/s8/s16/literal words differ from published byte order");
        const auto decoded = decode_heap_upload(expected);
        expect(decoded.address == heap_base && decoded.bytes == bytes,
               "independent encoded words did not reconstruct exactly");
    });
    run_case("all byte lengths and signed-width boundaries retain exact upload bytes", [] {
        const std::array<std::uint32_t, 12> boundaries{{0, 0x7f, 0xffffff80u, 0x80,
            0xffffff7fu, 0x7fff, 0xffff8000u, 0x8000, 0xffff7fffu,
            0x80000000u, 0x7fffffffu, 0x89abcdefu}};
        std::vector<std::uint8_t> source(maximum_upload_decoded_bytes);
        for (std::size_t word = 0; word < source.size() / 4; ++word)
            write_u32(source, 4 * word, boundaries[word % boundaries.size()]);
        for (std::size_t length = 1; length <= source.size(); ++length) {
            const std::vector<std::uint8_t> bytes(source.begin(), source.begin() + static_cast<std::ptrdiff_t>(length));
            const auto payload = encode_heap_upload(memory_bytes - static_cast<std::uint32_t>(length), bytes);
            const auto restored = decode_heap_upload(payload);
            expect(payload.size() <= maximum_packet_payload && restored.bytes == bytes &&
                   restored.address == memory_bytes - length, "partial word/group or heap-end bytes changed");
        }
        std::vector<std::uint8_t> raw(maximum_upload_decoded_bytes);
        for (std::size_t word = 0; word < raw.size() / 4; ++word)
            write_u32(raw, 4 * word, 0x12345678u + static_cast<std::uint32_t>(word));
        expect(encode_heap_upload(heap_base, raw).size() == maximum_packet_payload,
               "maximum literal upload no longer fills its exact bounded packet");
    });
    run_case("malformed compressed uploads cannot silently truncate or grow their extent", [] {
        auto payload = encode_heap_upload(heap_base, {0x78, 0x56, 0x34, 0x12});
        auto bad = payload; bad.pop_back();
        expect_throws([&] { decode_heap_upload(bad); }, "truncated literal word accepted");
        bad = payload; bad.push_back(0);
        expect_throws([&] { decode_heap_upload(bad); }, "trailing encoded byte accepted");
        bad = payload; bad.resize(8);
        expect_throws([&] { decode_heap_upload(bad); }, "missing descriptor accepted");
        bad = encode_heap_upload(heap_base, {0}); bad[8] |= 4;
        expect_throws([&] { decode_heap_upload(bad); }, "unused last-group code accepted");
        for (const auto length : {0u, static_cast<unsigned>(maximum_upload_decoded_bytes + 1)}) {
            bad = payload; write_u32(bad, 4, length);
            expect_throws([&] { decode_heap_upload(bad); }, "invalid decoded length accepted");
        }
        bad = payload; write_u32(bad, 0, memory_bytes - 3);
        expect_throws([&] { decode_heap_upload(bad); }, "decoded extent outside heap accepted");
        expect_throws([&] { encode_heap_upload(heap_base, {}); }, "empty upload encoded");
        expect_throws([&] { encode_heap_upload(heap_base,
            std::vector<std::uint8_t>(maximum_upload_decoded_bytes + 1)); }, "oversized decoded upload encoded");
    });
}

void record_tests() {
    run_case("rectangle wire fields have independently checked offsets", [] {
        Rect rectangle;
        rectangle.bounds = {-7, 2, 100, 30};
        rectangle.state = overlay_state({0x12, 0x34, 0x56});
        rectangle.depth = 0x123456;
        const auto bytes = encode_primitive(rectangle);
        expect(bytes.size() == 320 && read_u32(bytes, 0) == 0x00010101u && read_u32(bytes, 4) == 0 &&
               read_u32(bytes, 8) == 0x00563412u && read_i32(bytes, 32) == -7 &&
               read_u32(bytes, 40) == 100 && read_u32(bytes, 64) == 0x123456u,
               "rectangle differs from packed ABI offsets");
        expect(std::all_of(bytes.begin() + 68, bytes.end(), [](auto value) { return value == 0; }),
               "rectangle contains unused geometry payload");
        const auto decoded = decode_primitive(bytes);
        expect(encode_primitive(decoded.primitive, decoded.texture) == bytes, "rectangle roundtrip failed");
    });
    run_case("line endpoints and sprite origins survive signed packing", [] {
        Line line{{639, 0}, {0, 479}, overlay_state({6, 7, 8}), 777};
        const auto bytes = encode_primitive(line);
        expect(read_u32(bytes, 68) == 639 && read_u32(bytes, 72) == 0 &&
               read_u32(bytes, 80) == 0 && read_u32(bytes, 84) == 479, "line endpoint fields moved");
        auto decoded = decode_primitive(bytes);
        const auto& restored = std::get<Line>(decoded.primitive);
        expect(restored.a == line.a && restored.b == line.b && restored.depth == 777, "line data changed");
        auto image = pattern();
        RenderState state = overlay_state(); state.texture = image; state.color_key = 0x1234;
        Sprite sprite{{-2, -1, 0, 1}, 1, 2, state, 123};
        const TextureDescriptor texture{heap_base, 4, 4, 12};
        const auto packed = encode_primitive(sprite, texture);
        decoded = decode_primitive(packed);
        expect(decoded.texture && *decoded.texture == texture, "sprite lost physical texture descriptor");
        expect(encode_primitive(decoded.primitive, decoded.texture) == packed, "sprite roundtrip failed");
        const auto& metadata = std::get<Sprite>(decoded.primitive).state.texture;
        expect(metadata && metadata->width() == 4 && metadata->pitch_bytes() == 12, "metadata dimensions changed");
        expect_throws([&] { metadata->at(0, 0); }, "decoded descriptor supplied hidden host pixels");
    });
    run_case("all five triangle shading records preserve exact prepared integer planes", [] {
        auto image = pattern();
        for (unsigned shade = 0; shade <= 4; ++shade) {
            auto primitive = triangle(static_cast<ShadeMode>(shade), shade >= 2 ? image : nullptr);
            const auto texture = shade >= 2 ? std::optional<TextureDescriptor>{{heap_base, 4, 4, 12}} : std::nullopt;
            const auto packed = encode_primitive(primitive, texture);
            const auto decoded = decode_primitive(packed);
            expect(encode_primitive(decoded.primitive, decoded.texture) == packed, "triangle integer record changed");
            const auto& restored = std::get<Triangle>(decoded.primitive);
            expect(restored.snapped == primitive.snapped && restored.z.seed == primitive.z.seed &&
                   restored.q.step_y == primitive.q.step_y && restored.u.seed == primitive.u.seed &&
                   restored.v.step_x == primitive.v.step_x, "triangle fields were reconstructed or requantized");
            expect(read_i64(packed, 152) == primitive.z.seed && read_i64(packed, 248) == primitive.q.seed &&
                   read_i64(packed, 272) == primitive.u.seed && read_i64(packed, 296) == primitive.v.seed,
                   "documented plane offsets changed");
        }
    });
    run_case("reserved and malformed primitive fields cannot enter execution", [] {
        const auto original = encode_primitive(Rect{{1, 2, 3, 4}, overlay_state(), 0});
        for (const auto offset : std::array<std::size_t, 5>{3, 11, 15, 76, 152}) {
            auto corrupt = original; corrupt[offset] = 0xff;
            expect_throws([&] { decode_primitive(corrupt); }, "reserved primitive field accepted");
        }
        auto corrupt = original; corrupt[0] = 4;
        expect_throws([&] { decode_primitive(corrupt); }, "unknown primitive kind accepted");
        corrupt = original; corrupt[1] = 2;
        expect_throws([&] { decode_primitive(corrupt); }, "unknown primitive version accepted");
        corrupt = original; corrupt.resize(319);
        expect_throws([&] { decode_primitive(corrupt); }, "truncated primitive accepted");
        auto textured = encode_primitive(triangle(ShadeMode::TextureReplace, pattern()),
                                          TextureDescriptor{heap_base, 4, 4, 12});
        write_u32(textured, 48, 0);
        expect_throws([&] { decode_primitive(textured); }, "texture attachment alias accepted");
        textured = encode_primitive(triangle(ShadeMode::TextureReplace, pattern()),
                                      TextureDescriptor{heap_base, 4, 4, 12});
        write_u32(textured, 52, 3);
        expect_throws([&] { decode_primitive(textured); }, "NPOT shader texture accepted");
    });
    run_case("pass descriptors and tile entries validate exact sizes and extents", [] {
        PassDescriptor pass{1, LoadOp::Load, LoadOp::Clear, 0x9abc, 0x654321, heap_base};
        const auto packed = encode_pass_descriptor(pass);
        expect(packed[0] == 'N' && packed[1] == 'P' && packed[2] == 'S' && packed[3] == '1' &&
               read_u32(packed, 8) == 2 && read_u32(packed, 24) == 300, "pass wire constants changed");
        expect(encode_pass_descriptor(decode_pass_descriptor(packed)) == packed, "pass roundtrip failed");
        auto corrupt = packed; write_u32(corrupt, 20, memory_bytes - 4);
        expect_throws([&] { decode_pass_descriptor(corrupt); }, "out-of-memory tile table accepted");
        corrupt = packed; write_u32(corrupt, 28, 1);
        expect_throws([&] { decode_pass_descriptor(corrupt); }, "reserved pass field accepted");
        std::vector<std::uint8_t> entry(8);
        expect(decode_tile_entry(entry).count == 0, "zero empty tile entry rejected");
        write_u32(entry, 0, heap_base);
        expect_throws([&] { decode_tile_entry(entry); }, "nonzero empty tile-list address accepted");
        write_u32(entry, 4, 3);
        expect(decode_tile_entry(entry).count == 3, "valid tile reference list rejected");
        write_u32(entry, 0, memory_bytes - 4);
        expect_throws([&] { decode_tile_entry(entry); }, "reference list overrunning memory accepted");
    });
    run_case("copy packets preserve NPOT images and clipped signed coordinate domains", [] {
        CopyCommand image;
        image.image_source = true;
        image.source_origin = {-1, std::numeric_limits<int>::min()};
        image.destination_origin = {std::numeric_limits<int>::max(), 479};
        image.width = std::numeric_limits<int>::max(); image.height = 37;
        image.image = {heap_base, 23, 17, 52};
        const auto packed = encode_copy_command(image);
        expect(packed.size() == 56 && read_u32(packed, 0) == 1 && read_i32(packed, 12) == -1 &&
               read_u32(packed, 36) == heap_base && read_u32(packed, 40) == 23, "copy ABI fields changed");
        expect(encode_copy_command(decode_copy_command(packed)) == packed, "NPOT copy roundtrip failed");
        CopyCommand framebuffer;
        framebuffer.source = 1; framebuffer.target = 0; framebuffer.width = 640; framebuffer.height = 480;
        expect(encode_copy_command(decode_copy_command(encode_copy_command(framebuffer))) == encode_copy_command(framebuffer),
               "framebuffer copy roundtrip failed");
        auto corrupt = packed; write_u32(corrupt, 52, 1);
        expect_throws([&] { decode_copy_command(corrupt); }, "reserved copy field accepted");
        corrupt = packed; write_i32(corrupt, 28, -1);
        expect_throws([&] { decode_copy_command(corrupt); }, "negative copy width accepted");
    });
}

void program_tests() {
    run_case("CPU program uploads shared textures once and fetchable ordered primitive lists", [] {
        auto image = pattern();
        Pass pass;
        pass.clear_color = 0x4567;
        pass.primitives.emplace_back(triangle(ShadeMode::TextureReplace, image));
        auto second = triangle(ShadeMode::TextureGouraudModulate, image);
        second.state.color_key = 0x1234;
        pass.primitives.emplace_back(second);
        pass.primitives.emplace_back(Rect{{-2, 10, 70, 75}, overlay_state({4, 5, 6}), 0});
        pass.primitives.emplace_back(Line{{0, 0}, {639, 479}, overlay_state({7, 8, 9}), 0});
        RenderState sprite_state = overlay_state(); sprite_state.texture = image; sprite_state.color_key = 0;
        pass.primitives.emplace_back(Sprite{{20, 20, 22, 22}, 1, 1, sprite_state, 0});
        const auto program = compile_pass(pass, 17);
        const auto applied = apply_host_program(program, 17);
        expect(applied.uploads > 1 && program.upload_bytes == applied.heap.size(), "heap upload accounting differs");
        const auto descriptor = decode_pass_descriptor(slice(applied.heap, program.descriptor_address, pass_descriptor_bytes));
        expect(descriptor.target == 1 && descriptor.clear_color == 0x4567, "compiled pass state changed");
        const TextureDescriptor texture{heap_base, 4, 4, 12};
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x)
                expect(read_u16(applied.heap, 12u * y + 2u * x) == image->at(x, y), "uploaded pixel differs");
            for (unsigned padding = 8; padding < 12; ++padding)
                expect(applied.heap[12u * y + padding] == 0, "host pitch padding was not initialized");
        }
        const auto expected_bins = bin_primitives(pass.primitives);
        std::set<std::uint32_t> unique_records;
        std::uint64_t reference_count = 0;
        for (std::size_t tile = 0; tile < expected_bins.size(); ++tile) {
            const auto entry = decode_tile_entry(slice(applied.heap,
                descriptor.tile_table_address + static_cast<std::uint32_t>(tile * tile_entry_bytes), tile_entry_bytes));
            expect(entry.count == expected_bins[tile].size(), "compiled bin count changed");
            reference_count += entry.count;
            if (entry.count == 0) continue;
            const auto addresses = slice(applied.heap, entry.address, entry.count * 4u);
            for (std::size_t item = 0; item < expected_bins[tile].size(); ++item) {
                const auto address = read_u32(addresses, 4u * item);
                unique_records.insert(address);
                const auto index = expected_bins[tile][item];
                const auto expected_texture = (index < 2 || index == 4) ? std::optional<TextureDescriptor>{texture} : std::nullopt;
                expect(slice(applied.heap, address, primitive_record_bytes) == encode_primitive(pass.primitives[index], expected_texture),
                       "compiled list reordered or duplicated the wrong primitive");
                const auto decoded = decode_primitive(slice(applied.heap, address, primitive_record_bytes));
                if (expected_texture) expect(decoded.texture && *decoded.texture == texture, "shared texture was uploaded twice");
            }
        }
        expect(reference_count == program.reference_count && unique_records.size() == pass.primitives.size(),
               "reference counts or record deduplication differ");
    });
    run_case("empty pass still uploads 300 bins and submits an ordered completion fence", [] {
        Pass pass; pass.color_load = LoadOp::Load; pass.depth_load = LoadOp::Load;
        const auto program = compile_pass(pass, 42);
        const auto applied = apply_host_program(program, 42);
        expect(program.heap_bytes_used == tile_count * tile_entry_bytes + pass_descriptor_bytes &&
               program.reference_count == 0, "empty pass invented records or omitted its tile table");
        const auto descriptor = decode_pass_descriptor(slice(applied.heap, program.descriptor_address, pass_descriptor_bytes));
        expect(descriptor.color_load == LoadOp::Load && descriptor.depth_load == LoadOp::Load, "LOAD state changed");
        for (unsigned tile = 0; tile < tile_count; ++tile)
            expect(decode_tile_entry(slice(applied.heap, descriptor.tile_table_address + tile * tile_entry_bytes, tile_entry_bytes)).count == 0,
                   "empty pass has nonempty tile entry");
    });
    run_case("aggregate texture allocations and request sequence wrap fail before upload", [] {
        Pass pass;
        for (unsigned index = 0; index < 3; ++index) {
            auto image = std::make_shared<Image>(1024, 1024);
            auto state = overlay_state(); state.texture = image;
            pass.primitives.emplace_back(Sprite{{0, 0, 1, 1}, 0, 0, state, 0});
        }
        expect_throws([&] { compile_pass(pass); }, "aggregate heap overflow accepted");
        expect_throws([] { compile_pass(Pass{}, std::numeric_limits<std::uint32_t>::max() - 1); },
                      "request sequence wrapped without a new epoch");
        expect_throws([] { compile_pass(Pass{}, 0); }, "zero first request sequence accepted");
        Pass invalid; invalid.target = 3;
        expect_throws([&] { compile_pass(invalid); }, "unsupported framebuffer target accepted");
    });
}

void resident_program_tests() {
    run_case("resident compiler reuses texture bytes only after the previous fence", [] {
        auto image = pattern();
        Pass pass;
        pass.primitives.emplace_back(triangle(ShadeMode::TextureReplace, image));
        pass.primitives.emplace_back(triangle(ShadeMode::TextureGouraudModulate, image));
        HostPassCompiler compiler;
        const auto initial = compiler.compile(pass, 10);
        const auto initial_heap = apply_host_program(initial, 10);
        expect(initial.texture_upload_bytes == 48 && initial.resident_texture_bytes == 48,
               "initial shared texture was not uploaded exactly once");
        expect_throws([&] { compiler.compile(pass, initial.next_sequence); },
                      "transient allocations reused before completion fence");
        compiler.complete();
        const auto next = compiler.compile(pass, initial.next_sequence);
        const auto next_heap = apply_host_program(next, initial.next_sequence, initial_heap.heap);
        expect(next.texture_upload_bytes == 0 && next.upload_bytes + 48 == initial.upload_bytes &&
               next.resident_texture_bytes == 48 && next.heap_bytes_used == initial.heap_bytes_used,
               "unchanged texture consumed another upload or heap allocation");
        expect(first_texture(next, next_heap) == first_texture(initial, initial_heap),
               "unchanged texture moved to a different physical descriptor");
        expect(next_heap.heap == initial_heap.heap, "resident frame changed uploaded primitive/pixel contents");
        compiler.complete();
    });
    run_case("mutable aliases and changed image dimensions invalidate CPU residency", [] {
        auto image = pattern();
        Pass pass;
        pass.primitives.emplace_back(triangle(ShadeMode::TextureReplace, image));
        HostPassCompiler compiler;
        const auto initial = compiler.compile(pass);
        const auto initial_heap = apply_host_program(initial, 1);
        compiler.complete();
        image->at(1, 2) ^= 0x5a5a;
        const auto changed = compiler.compile(pass, initial.next_sequence);
        const auto changed_heap = apply_host_program(changed, initial.next_sequence, initial_heap.heap);
        expect(changed.texture_upload_bytes == 48, "mutable pixel edit silently retained old GPU pixels");
        const auto descriptor = first_texture(changed, changed_heap);
        expect(descriptor.base == heap_base &&
               read_u16(changed_heap.heap, 2u * 12u + 2u) == image->at(1, 2),
               "changed pixels did not reach the retained texture allocation");
        // The uploaded snapshot must be independent of later Image writes.
        image->at(1, 2) ^= 0xffff;
        compiler.complete();
        const auto changed_again = compiler.compile(pass, changed.next_sequence);
        expect(changed_again.texture_upload_bytes == 48, "pending snapshot aliased mutable host pixels");
        compiler.complete();
        *image = Image(8, 8);
        const auto resized = compiler.compile(pass, changed_again.next_sequence);
        const auto resized_heap = apply_host_program(resized, changed_again.next_sequence, changed_heap.heap);
        const auto resized_descriptor = first_texture(resized, resized_heap);
        expect(resized.texture_upload_bytes == 128 && resized_descriptor.width == 8 &&
               resized_descriptor.height == 8 && resized_descriptor.base >= heap_base + 48,
               "same Image owner with new dimensions reused an incompatible allocation");
        compiler.complete();
    });
    run_case("residency resets on recovery and failed compilation is transactional", [] {
        auto image = pattern();
        Pass pass; pass.primitives.emplace_back(triangle(ShadeMode::TextureReplace, image));
        HostPassCompiler compiler;
        expect_throws([&] { compiler.complete(); }, "completion without a pending submission accepted");
        const auto initial = compiler.compile(pass);
        compiler.complete();
        expect_throws([&] { compiler.compile(pass, std::numeric_limits<std::uint32_t>::max() - 1); },
                      "resident compile wrapped its packet sequence");
        Pass invalid = pass; invalid.target = 3;
        expect_throws([&] { compiler.compile(invalid, initial.next_sequence); },
                      "invalid pass changed compiler state");
        const auto reused = compiler.compile(pass, initial.next_sequence);
        expect(reused.texture_upload_bytes == 0, "failed compilation invalidated a completed resident texture");
        compiler.reset();
        const auto recovered = compiler.compile(pass);
        expect(recovered.texture_upload_bytes == 48 && recovered.resident_texture_bytes == 48,
               "recovery assumed stale GPU contents survived");
        compiler.complete();
    });
    run_case("resident heap compacts between completed passes and rejects aggregate overflow", [] {
        HostPassCompiler compiler;
        std::uint32_t sequence = 1;
        for (unsigned frame = 0; frame < 3; ++frame) {
            auto image = std::make_shared<Image>(1024, 1024);
            image->at(0, 0) = static_cast<std::uint16_t>(frame + 1);
            auto state = overlay_state(); state.texture = image;
            Pass pass; pass.primitives.emplace_back(Sprite{{0, 0, 1, 1}, 0, 0, state, 0});
            const auto program = compiler.compile(pass, sequence);
            const auto applied = apply_host_program(program, sequence);
            const auto descriptor = first_texture(program, applied);
            expect(program.texture_upload_bytes == 2u * 1024u * 1024u &&
                   read_u16(applied.heap, descriptor.base - heap_base) == frame + 1,
                   "new resident texture omitted its bytes");
            expect(program.resident_texture_bytes == (frame == 1 ? 4u : 2u) * 1024u * 1024u,
                   "unused residents were not reclaimed when the heap filled");
            sequence = program.next_sequence; compiler.complete();
        }
        Pass overflow;
        for (unsigned index = 0; index < 3; ++index) {
            auto state = overlay_state(); state.texture = std::make_shared<Image>(1024, 1024);
            overflow.primitives.emplace_back(Sprite{{0, 0, 1, 1}, 0, 0, state, 0});
        }
        expect_throws([&] { compiler.compile(overflow, sequence); },
                      "aggregate active textures exceeded the shared GPU heap");
        const auto after_failure = compiler.compile(Pass{}, sequence);
        expect(after_failure.texture_upload_bytes == 0, "overflow poisoned the next compile");
        compiler.complete();
    });
    run_case("resident ownership prevents host pointer reuse until allocator reset", [] {
        HostPassCompiler compiler;
        std::weak_ptr<const Image> lifetime;
        {
            auto image = pattern(); lifetime = image;
            Pass pass; pass.primitives.emplace_back(triangle(ShadeMode::TextureReplace, image));
            compiler.compile(pass); compiler.complete();
        }
        expect(!lifetime.expired(), "resident cache retained an unowned raw Image pointer");
        compiler.reset();
        expect(lifetime.expired(), "allocator reset leaked host texture ownership");
    });
}

} // namespace

int main() {
    byte_tests(); upload_codec_tests(); record_tests(); program_tests(); resident_program_tests();
    std::cout << cases - failures << '/' << cases << " protocol cases passed\n";
    return failures == 0 ? 0 : 1;
}
