// Lossless upload-codec traffic experiment; not a production wire ABI.
// Uses exact memory images emitted by the current standalone pass compiler.
#include "nano_gpu/demo.hpp"
#include "nano_gpu/hw/protocol.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace nano_gpu;
namespace hw = nano_gpu::hw;

struct Totals {
    std::uint64_t records = 0, encoded = 0, selected = 0, fallback = 0;
    std::array<std::uint64_t, 4> words{};
    std::uint64_t minimum = 1000, maximum = 0;
    void add(const std::vector<std::uint8_t>& packed,
             const std::array<std::uint64_t, 4>& counts) {
        ++records;
        encoded += packed.size();
        selected += std::min<std::size_t>(packed.size(), hw::primitive_record_bytes);
        fallback += packed.size() >= hw::primitive_record_bytes;
        minimum = std::min<std::uint64_t>(minimum, packed.size());
        maximum = std::max<std::uint64_t>(maximum, packed.size());
        for (unsigned i = 0; i < 4; ++i) words[i] += counts[i];
    }
};

unsigned width_code(std::uint32_t bits) {
    if (bits == 0) return 0;
    const std::uint32_t sx8 = (bits & 0x80u) ? (bits | 0xffffff00u) : (bits & 0xffu);
    if (sx8 == bits) return 1;
    const std::uint32_t sx16 = (bits & 0x8000u) ? (bits | 0xffff0000u) : (bits & 0xffffu);
    if (sx16 == bits) return 2;
    return 3;
}

std::vector<std::uint8_t> encode(const std::vector<std::uint8_t>& raw,
                               std::array<std::uint64_t, 4>& counts) {
    std::vector<std::uint8_t> packed;
    for (unsigned group = 0; group < 20; ++group) {
        const auto descriptor = packed.size();
        packed.push_back(0);
        for (unsigned slot = 0; slot < 4; ++slot) {
            const auto bits = hw::read_u32(raw, 4 * (4 * group + slot));
            const auto code = width_code(bits);
            packed[descriptor] |= static_cast<std::uint8_t>(code << (2 * slot));
            ++counts[code];
            const unsigned bytes = code == 3 ? 4 : code;
            for (unsigned b = 0; b < bytes; ++b)
                packed.push_back(static_cast<std::uint8_t>(bits >> (8 * b)));
        }
    }
    return packed;
}

std::vector<std::uint8_t> decode(const std::vector<std::uint8_t>& packed) {
    std::vector<std::uint8_t> raw(320);
    std::size_t offset = 0;
    for (unsigned group = 0; group < 20; ++group) {
        if (offset >= packed.size()) throw std::runtime_error("truncated group descriptor");
        const unsigned descriptor = packed[offset++];
        for (unsigned slot = 0; slot < 4; ++slot) {
            const unsigned code = (descriptor >> (2 * slot)) & 3;
            const unsigned bytes = code == 3 ? 4 : code;
            if (offset + bytes > packed.size()) throw std::runtime_error("truncated encoded word");
            std::uint32_t bits = 0;
            for (unsigned b = 0; b < bytes; ++b)
                bits |= static_cast<std::uint32_t>(packed[offset++]) << (8 * b);
            if (code == 1 && (bits & 0x80u)) bits |= 0xffffff00u;
            if (code == 2 && (bits & 0x8000u)) bits |= 0xffff0000u;
            hw::write_u32(raw, 4 * (4 * group + slot), bits);
        }
    }
    if (offset != packed.size()) throw std::runtime_error("trailing encoded payload");
    return raw;
}

std::vector<std::uint8_t> reconstruct_heap(const hw::HostProgram& program) {
    std::vector<std::uint8_t> heap(program.heap_bytes_used, 0);
    std::size_t offset = 0;
    while (offset < program.bytes.size()) {
        const auto opcode = hw::read_u16(program.bytes, offset + 6);
        const auto length = hw::read_u32(program.bytes, offset + 8);
        const auto payload = offset + hw::packet_header_bytes;
        if (payload + length > program.bytes.size()) throw std::runtime_error("truncated program");
        if (opcode == static_cast<unsigned>(hw::Opcode::WriteHeap)) {
            const auto address = hw::read_u32(program.bytes, payload);
            const auto count = hw::read_u32(program.bytes, payload + 4);
            if (length != count + 8 || address < hw::heap_base ||
                static_cast<std::uint64_t>(address - hw::heap_base) + count > heap.size())
                throw std::runtime_error("invalid experiment heap extent");
            std::copy_n(program.bytes.begin() + static_cast<std::ptrdiff_t>(payload + 8), count,
                        heap.begin() + static_cast<std::ptrdiff_t>(address - hw::heap_base));
        }
        offset = payload + length;
    }
    return heap;
}

void emit_totals(std::ostream& out, const Totals& t) {
    out << "{\"records\": " << t.records
        << ", \"raw_bytes\": " << 320 * t.records
        << ", \"encoded_bytes\": " << t.encoded
        << ", \"bytes_with_raw_fallback\": " << t.selected
        << ", \"raw_fallback_records\": " << t.fallback
        << ", \"minimum_encoded_record_bytes\": " << (t.records ? t.minimum : 0)
        << ", \"maximum_encoded_record_bytes\": " << t.maximum
        << ", \"word_counts_zero_s8_s16_raw32\": [";
    for (unsigned i = 0; i < 4; ++i) out << (i ? ", " : "") << t.words[i];
    out << "]}";
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("usage: traffic OUTPUT_DIRECTORY");
        const std::filesystem::path output(argv[1]);
        std::filesystem::create_directories(output);
        std::ofstream report(output / "traffic.json"), encoded(output / "encoded.hex"),
            expected(output / "expected.hex"), lengths(output / "lengths.hex"),
            metadata(output / "records.csv");
        if (!report || !encoded || !expected || !lengths || !metadata)
            throw std::runtime_error("cannot create experiment output");
        metadata << "record,scene,frame,primitive,kind,encoded_bytes,selected_bytes\n";
        report << "{\n  \"schema\": \"N20K-UPLOAD-CODEC-TRAFFIC-1\",\n"
            << "  \"codec\": \"20 groups: 1-byte descriptor of four2-bitcodes, then word payloads zero/s8/s16/raw32; little-endian\",\n"
            << "  \"scope\": \"Proposed codec of exact current compiled 320-byte records; no new protocol or measured FPS\",\n"
            << "  \"raw_fallback_scope\": \"Select raw WRITE_HEAP if encoding grows; framing/command/address overhead excluded\",\n"
            << "  \"times_seconds\": [1.25, 1.2666666666666667],\n"
            << "  \"cases\": [\n";
        std::uint64_t record_index = 0, encoded_bytes = 0;
        bool first = true;
        for (const auto scene : {DemoScene::Showcase, DemoScene::Perspective,
                                 DemoScene::Overdraw, DemoScene::Opaque}) {
            for (unsigned frame = 0; frame < 2; ++frame) {
                const auto pass = make_demo_pass(scene, frame ? 0 : 1, 1.25L + frame / 60.0L);
                const auto program = hw::compile_pass(pass);
                const auto heap = reconstruct_heap(program);
                Totals all, triangles;
                std::array<Totals, 4> kinds;
                for (std::size_t i = 0; i < pass.primitives.size(); ++i) {
                    const auto start = program.texture_upload_bytes + 320 * i;
                    if (start + 320 > heap.size()) throw std::runtime_error("record exceeds heap");
                    std::vector<std::uint8_t> raw(heap.begin() + static_cast<std::ptrdiff_t>(start),
                        heap.begin() + static_cast<std::ptrdiff_t>(start + 320));
                    const auto kind = hw::read_u32(raw, 0) & 0xffu;
                    if (kind != pass.primitives[i].index()) throw std::runtime_error("record extraction differs from compiler");
                    // Decode the exact compiler record as an additional extraction check.
                    (void) hw::decode_primitive(raw);
                    std::array<std::uint64_t, 4> counts{};
                    const auto packed = encode(raw, counts);
                    if (decode(packed) != raw) throw std::runtime_error("lossless roundtrip mismatch");
                    all.add(packed, counts); kinds[kind].add(packed, counts);
                    if (kind == 0) triangles.add(packed, counts);
                    encoded << std::hex << std::setfill('0');
                    for (const auto byte : packed) encoded << std::setw(2) << static_cast<unsigned>(byte) << '\n';
                    expected << std::hex << std::setfill('0');
                    for (unsigned word = 0; word < 80; ++word)
                        expected << std::setw(8) << hw::read_u32(raw, 4 * word) << '\n';
                    lengths << std::hex << packed.size() << '\n';
                    metadata << record_index << ',' << scene_name(scene) << ',' << frame << ',' << i
                             << ',' << kind << ',' << packed.size() << ',' << std::min<std::size_t>(320, packed.size()) << '\n';
                    ++record_index; encoded_bytes += packed.size();
                }
                report << (first ? "" : ",\n") << "    {\"scene\": \"" << scene_name(scene)
                    << "\", \"frame\": " << frame << ", \"texture_upload_bytes_unchanged\": " << program.texture_upload_bytes
                    << ", \"original_heap_upload_bytes\": " << program.upload_bytes
                    << ", \"projected_heap_payload_bytes\": " << program.upload_bytes - 320 * all.records + all.selected
                    << ", \"tile_reference_count_unchanged\": " << program.reference_count
                    << ", \"all_primitives\": "; emit_totals(report, all);
                report << ", \"triangles\": "; emit_totals(report, triangles);
                report << ", \"by_kind_triangle_rect_line_sprite\": [";
                for (unsigned kind = 0; kind < 4; ++kind) {
                    if (kind) report << ", ";
                    emit_totals(report, kinds[kind]);
                }
                report << "]}";
                first = false;
            }
        }
        report << "\n  ],\n  \"vector_records\": " << record_index
               << ",\n  \"vector_encoded_bytes\": " << encoded_bytes
               << ",\n  \"vector_expected_words\": " << record_index * 80
               << ",\n  \"software_exact_roundtrip_passed\": true\n}\n";
        std::cout << "Exact codec roundtrip: " << record_index << " real scene records, "
                  << encoded_bytes << " encoded bytes, " << record_index * 80 << " words\n";
    } catch (const std::exception& error) {
        std::cerr << "upload-codec-traffic: " << error.what() << '\n';
        return 1;
    }
}
