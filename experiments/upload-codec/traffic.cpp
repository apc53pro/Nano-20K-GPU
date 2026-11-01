// Upload-codec traffic analysis using the current standalone pass compiler.
// Per-record codec counts remain an isolated comparison; actual packet counts
// below come from the production compiler's sole encoded WRITE_HEAP format.
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

struct WireTraffic {
    std::uint64_t packets = 0, packet_payload_bytes = 0;
    std::uint64_t upload_packets = 0, upload_payload_bytes = 0;
    std::uint64_t upload_encoded_body_bytes = 0, upload_decoded_bytes = 0;
    std::uint64_t maximum_upload_decoded_bytes = 0, maximum_upload_payload_bytes = 0;
};

std::vector<std::uint8_t> reconstruct_heap(const hw::HostProgram& program, WireTraffic& wire) {
    std::vector<std::uint8_t> heap(program.heap_bytes_used, 0);
    std::size_t offset = 0;
    while (offset < program.bytes.size()) {
        if (program.bytes.size() - offset < hw::packet_header_bytes)
            throw std::runtime_error("truncated program header");
        const auto opcode = hw::read_u16(program.bytes, offset + 6);
        const auto length = hw::read_u32(program.bytes, offset + 8);
        const auto payload = offset + hw::packet_header_bytes;
        if (payload + length > program.bytes.size()) throw std::runtime_error("truncated program");
        ++wire.packets;
        wire.packet_payload_bytes += length;
        if (opcode == static_cast<unsigned>(hw::Opcode::WriteHeap)) {
            const auto upload = hw::decode_heap_upload(std::vector<std::uint8_t>(
                program.bytes.begin() + static_cast<std::ptrdiff_t>(payload),
                program.bytes.begin() + static_cast<std::ptrdiff_t>(payload + length)));
            if (upload.address < hw::heap_base ||
                static_cast<std::uint64_t>(upload.address - hw::heap_base) + upload.bytes.size() > heap.size())
                throw std::runtime_error("invalid experiment heap extent");
            std::copy(upload.bytes.begin(), upload.bytes.end(),
                heap.begin() + static_cast<std::ptrdiff_t>(upload.address - hw::heap_base));
            ++wire.upload_packets;
            wire.upload_payload_bytes += length;
            wire.upload_encoded_body_bytes += length - 8;
            wire.upload_decoded_bytes += upload.bytes.size();
            wire.maximum_upload_decoded_bytes = std::max<std::uint64_t>(
                wire.maximum_upload_decoded_bytes, upload.bytes.size());
            wire.maximum_upload_payload_bytes = std::max<std::uint64_t>(
                wire.maximum_upload_payload_bytes, length);
        }
        offset = payload + length;
    }
    if (wire.upload_decoded_bytes != program.upload_bytes ||
        wire.packet_payload_bytes + wire.packets * hw::packet_header_bytes != program.bytes.size())
        throw std::runtime_error("wire traffic accounting mismatch");
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
        report << "{\n  \"schema\": \"N20K-UPLOAD-CODEC-TRAFFIC-2\",\n"
            << "  \"codec\": \"20 groups per record: 1-byte descriptor of four 2-bit codes, then word payloads zero/s8/s16/raw32; little-endian\",\n"
            << "  \"scope\": \"Exact current compiled heap images and encoded packet byte counts; no measured FPS\",\n"
            << "  \"raw_fallback_scope\": \"Historical per-record comparison only, excluding framing; current production uploads always use the sole codec and have no raw fallback\",\n"
            << "  \"projected_heap_payload_scope\": \"Historical isolated per-record replacement estimate; use wire_traffic for actual current packet sizes\",\n"
            << "  \"wire_scope\": \"Compiled program contains WRITE_HEAP, SUBMIT_PASS and FENCE; PRESENT request serialized separately; successful FENCE/PRESENT reply widths counted from ABI, not sampled on bus\",\n"
            << "  \"times_seconds\": [1.25, 1.2666666666666667],\n"
            << "  \"cases\": [\n";
        std::uint64_t record_index = 0, encoded_bytes = 0;
        bool first = true;
        for (const auto scene : {DemoScene::Showcase, DemoScene::Perspective,
                                 DemoScene::Overdraw, DemoScene::Opaque}) {
            for (unsigned frame = 0; frame < 2; ++frame) {
                const auto pass = make_demo_pass(scene, frame ? 0 : 1, 1.25L + frame / 60.0L);
                const auto program = hw::compile_pass(pass);
                WireTraffic wire;
                const auto heap = reconstruct_heap(program, wire);
                std::vector<std::uint8_t> present_payload(8);
                hw::write_u32(present_payload, 0, static_cast<std::uint32_t>(pass.target));
                hw::write_u32(present_payload, 4, program.next_sequence);
                const auto present = hw::encode_packet(hw::Opcode::Present, program.next_sequence, present_payload);
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
                    << ", \"wire_traffic\": {\"compiled_program_request_bytes\": " << program.bytes.size()
                    << ", \"compiled_program_packets\": " << wire.packets
                    << ", \"compiled_program_header_bytes\": " << wire.packets * hw::packet_header_bytes
                    << ", \"compiled_program_payload_bytes\": " << wire.packet_payload_bytes
                    << ", \"upload_packets\": " << wire.upload_packets
                    << ", \"upload_payload_bytes\": " << wire.upload_payload_bytes
                    << ", \"upload_address_length_bytes\": " << wire.upload_packets * 8
                    << ", \"upload_encoded_body_bytes\": " << wire.upload_encoded_body_bytes
                    << ", \"upload_decoded_bytes\": " << wire.upload_decoded_bytes
                    << ", \"maximum_upload_decoded_bytes\": " << wire.maximum_upload_decoded_bytes
                    << ", \"maximum_upload_payload_bytes\": " << wire.maximum_upload_payload_bytes
                    << ", \"present_request_bytes\": " << present.size()
                    << ", \"frame_request_bytes_including_present\": " << program.bytes.size() + present.size()
                    << ", \"successful_fence_present_reply_bytes\": 56"
                    << ", \"frame_request_reply_bytes\": " << program.bytes.size() + present.size() + 56 << "}"
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
