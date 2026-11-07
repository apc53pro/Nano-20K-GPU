#include "nano_gpu/hw/system.hpp"

#include "nano_gpu/hw/bsram.hpp"
#include "nano_gpu/hw/protocol.hpp"
#include "nano_gpu/hw/resources.hpp"
#include "nano_gpu/hw/system_copy.hpp"
#include "nano_gpu/hw/tile_pipeline.hpp"

#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

namespace nano_gpu::hw {
namespace {

// The shared reader has two actual SDPB banks, striped low/high16. Incoming
// slot words are clocked into RAM and the already allocated 320-byte decoded
// field registers on the same edge. Consecutive synchronous slot reads overlap
// capture of the previous word. Typed decoders only interpret established
// registers; diagnostic READ_MEMORY replies stream out of RAM instead.
struct Reader {
    enum class State { Idle, Request, Wait, ReadCapture, Done } state = State::Idle;
    std::array<Sdp1kx16, 2> ram;
    std::array<std::uint8_t, primitive_record_bytes> fields{};
    MemoryClient client = MemoryClient::Command;
    std::uint32_t address = 0, burst_address = 0;
    unsigned length = 0, copied = 0, word = 0, response_words = 0;
    bool decode_fields = true;
    void ports(bool write = false, unsigned wa = 0, std::uint32_t value = 0,
               bool read = false, unsigned ra = 0) {
        for (unsigned bank = 0; bank < 2; ++bank)
            ram[bank].tick({write, static_cast<std::uint16_t>(wa),
                static_cast<std::uint16_t>(value >> (16 * bank)), read,
                static_cast<std::uint16_t>(ra), false});
    }
    std::uint32_t result() const { return ram[0].output() | (std::uint32_t{ram[1].output()} << 16); }
    void start(std::uint32_t base, unsigned count, MemoryClient port, bool decode = true) {
        if (count == 0 || count > 512 || (decode && count > fields.size()) ||
            static_cast<std::uint64_t>(base) + count > memory_bytes)
            throw std::out_of_range("bounded system read outside SDRAM");
        address = base; length = count; copied = 0; client = port;
        if (decode) fields.fill(0);
        decode_fields = decode; state = State::Request;
    }
    bool done() const { return state == State::Done; }
    void tick(SdramController& memory) {
        switch (state) {
        case State::Idle: case State::Done: ports(); break;
        case State::Request: {
            burst_address = (address + copied) & ~3u;
            MemoryRequest request;
            request.address = burst_address;
            request.words = std::min<unsigned>(memory_burst_words,
                (address + length - burst_address + 3u) / 4u);
            if (memory.submit_prepared(client, request)) state = State::Wait;
            ports();
            break;
        }
        case State::Wait:
            if (memory.response_ready(client)) {
                response_words = memory.response_info(client).words;
                word = 0;
                if (!memory.request_response_word(client, word))
                    throw std::logic_error("system reader could not prime its owned slot");
                state = State::ReadCapture;
            }
            ports();
            break;
        case State::ReadCapture: {
            if (!memory.response_word_valid(client)) { ports(); break; }
            if (memory.response_word_offset(client) != word)
                throw std::logic_error("system reader lost its synchronous slot word");
            const auto value = memory.response_word(client);
            ports(true, (burst_address + 4 * word - (address & ~3u)) / 4, value);
            for (unsigned byte = 0; byte < 4; ++byte) {
                const auto source = burst_address + 4 * word + byte;
                if (source >= address && source < address + length) {
                    if (decode_fields) fields[source - address] = static_cast<std::uint8_t>(value >> (8 * byte));
                    ++copied;
                }
            }
            if (++word == response_words) {
                memory.release_response(client);
                state = copied == length ? State::Done : State::Request;
            } else if (!memory.request_response_word(client, word))
                throw std::logic_error("system reader could not advance its owned slot");
            break;
        }
        }
    }
    std::vector<std::uint8_t> view() const {
        if (!done() || !decode_fields) throw std::logic_error("reader fields are not established");
        return {fields.begin(), fields.begin() + length};
    }
};

// Byte uploads use one pack register. Native-word sources feed the owned
// controller slot directly from an established synchronous source result.
// Neither mode keeps a duplicate16-word request array.
struct Writer {
    enum class State { Idle, Fill, Word, Request, Wait, Done } state = State::Idle;
    std::function<std::optional<std::uint8_t>(unsigned)> source;
    std::function<std::optional<std::uint32_t>(unsigned)> word_source;
    MemoryClient client = MemoryClient::Host;
    MemoryRequest request{};
    std::uint32_t packed = 0;
    std::uint8_t mask = 0;
    unsigned word = 0;
    std::uint32_t address = 0;
    unsigned length = 0, cursor = 0, fill = 0, chunk = 0, prefix = 0;
    void prepare() {
        request = {}; request.write = true;
        request.address = (address + cursor) & ~3u;
        prefix = (address + cursor) & 3u;
        chunk = std::min<unsigned>(length - cursor, memory_burst_words * 4 - prefix);
        request.words = (prefix + chunk + 3) / 4;
        fill = word = 0; packed = 0; mask = 0; state = State::Fill;
    }
    void start(std::uint32_t base, unsigned count, MemoryClient port,
               std::function<std::optional<std::uint8_t>(unsigned)> input) {
        if (count == 0 || static_cast<std::uint64_t>(base) + count > memory_bytes)
            throw std::out_of_range("system write outside SDRAM");
        address = base; length = count; client = port; source = std::move(input);
        word_source = {}; cursor = 0; prepare();
    }
    void start_words(std::uint32_t base, unsigned count, MemoryClient port,
                     std::function<std::optional<std::uint32_t>(unsigned)> input) {
        if (count == 0 || (base & 3u) != 0 || (count & 3u) != 0 ||
            static_cast<std::uint64_t>(base) + count > memory_bytes)
            throw std::out_of_range("native-word system write outside SDRAM or unaligned");
        address = base; length = count; client = port; word_source = std::move(input);
        source = {}; cursor = 0; prepare();
    }
    bool done() const { return state == State::Done; }
    void tick(SdramController& memory) {
        switch (state) {
        case State::Idle: case State::Done: break;
        case State::Fill: {
            if (word_source) {
                const auto value = word_source((cursor + fill) / 4);
                if (!value || !memory.write_request_word(client, word, *value, 0xf)) break;
                ++word; fill += 4;
                if (fill == chunk) state = State::Request;
                break;
            }
            const auto value = source(cursor + fill);
            if (!value) break;
            const auto index = prefix + fill;
            packed |= static_cast<std::uint32_t>(*value) << (8 * (index & 3u));
            mask |= static_cast<std::uint8_t>(1u << (index & 3u));
            ++fill;
            if (((prefix + fill) & 3u) == 0 || fill == chunk) state = State::Word;
            break;
        }
        case State::Word:
            if (memory.write_request_word(client, word, packed, mask)) {
                ++word; packed = 0; mask = 0;
                state = fill == chunk ? State::Request : State::Fill;
            }
            break;
        case State::Request:
            if (memory.submit_prepared(client, request)) state = State::Wait;
            break;
        case State::Wait:
            if (memory.response_ready(client)) {
                memory.release_response(client); cursor += chunk;
                if (cursor == length) state = State::Done; else prepare();
            }
            break;
        }
    }
};

// Three native row banks match the tile attachment layout. Accesses are staged
// during execute() and clocked once at the end of the common core edge.
struct RowBanks {
    OpaqueTile::AttachmentBanks ram;
    OpaqueTile::AttachmentBanks* active = &ram;
    std::array<BsramCycle, 3> staged{};
    void read(unsigned index) {
        for (auto& p : staged) {
            if (p.read_enable) throw std::logic_error("two row reads in one edge");
            p.read_enable = true; p.read_address = static_cast<std::uint16_t>(index);
        }
    }
    void write(unsigned index, TileWord value, bool color = true, bool depth = true) {
        const std::array<std::uint16_t, 3> words{{value.color, static_cast<std::uint16_t>(value.depth),
            static_cast<std::uint16_t>(value.depth >> 16)}};
        for (unsigned bank = 0; bank < 3; ++bank) {
            if ((bank == 0 && !color) || (bank != 0 && !depth)) continue;
            auto& p = staged[bank];
            if (p.write_enable) throw std::logic_error("two row writes in one edge");
            p.write_enable = true; p.write_address = static_cast<std::uint16_t>(index); p.write_data = words[bank];
        }
    }
    TileWord result() const {
        return {(*active)[0].output(), (*active)[1].output() | (std::uint32_t{(*active)[2].output()} << 16)};
    }
    void exchange_with(OpaqueTile& tile) {
        for (const auto& p : staged)
            if (p.read_enable || p.write_enable || p.reset)
                throw std::logic_error("attachment bank exchange overlaps a staged RAM port operation");
        tile.exchange_attachment_banks(active);
    }
    void tick() { for (unsigned bank = 0; bank < 3; ++bank) { (*active)[bank].tick(staged[bank]); staged[bank] = {}; } }
};

template<std::size_t N>
void put(std::array<std::uint8_t, N>& data, unsigned offset, std::uint64_t value, unsigned count = 4) {
    for (unsigned byte = 0; byte < count; ++byte)
        data.at(offset + byte) = static_cast<std::uint8_t>(value >> (8 * byte));
}
std::uint32_t get(const std::array<std::uint8_t, 64>& data, unsigned offset) {
    std::uint32_t value = 0;
    for (unsigned byte = 0; byte < 4; ++byte) value |= std::uint32_t{data[offset + byte]} << (8 * byte);
    return value;
}
void append(std::vector<std::uint8_t>& data, const std::vector<std::uint8_t>& tail) {
    data.insert(data.end(), tail.begin(), tail.end());
}
std::vector<std::uint8_t> scalar(std::uint32_t value) {
    std::vector<std::uint8_t> bytes(4); write_u32(bytes, 0, value); return bytes;
}
void check_buffer(int id) {
    if (id < 0 || id > 1) throw std::out_of_range("physical color buffer must be 0 or 1");
}

} // namespace

struct SystemModel::Impl {
    // Host-only instrumentation survives hard-reset reconstruction. Device
    // counters restart with their epoch; observer timestamps and token IDs do
    // not, so a single trace/scoreboard can follow cancellation and recovery.
    struct Observation {
        SystemCycleObserver callback;
        std::uint64_t clock = 0;
        std::uint64_t last_fragment_id = 0, last_primitive_id = 0;
        std::array<FragmentToken, fragment_trace_capacity> active_fragments{};
        unsigned active_count = 0;

        void accept(const FragmentToken& token) {
            const auto found = std::find_if(active_fragments.begin(), active_fragments.end(),
                [](const FragmentToken& active) { return active.id == 0; });
            if (found == active_fragments.end()) throw std::logic_error("system observer capacity exceeded");
            *found = token; ++active_count;
            last_fragment_id = token.id;
            last_primitive_id = std::max(last_primitive_id, token.primitive_id);
        }
        void retire(const FragmentToken& token) {
            const auto found = std::find_if(active_fragments.begin(), active_fragments.end(),
                [&](const FragmentToken& active) { return active.id == token.id && token.id != 0; });
            if (found == active_fragments.end() || found->primitive_id != token.primitive_id ||
                found->x != token.x || found->y != token.y || found->address != token.address ||
                found->textured != token.textured)
                throw std::logic_error("system observer retired an unknown or changed fragment");
            *found = {}; --active_count;
        }
    };
    enum class State {
        Idle, Capture, Upload, PassInvalidate, PassDescriptor, TileEntry, LoadRow,
        LoadColor, LoadDepthLow, LoadDepthHigh, UnpackColor, UnpackDepth,
        LoadLocal, LoadLocalCapture,
        Reference, Record, Prefetched, Draw, StoreLocal, StorePrepare, StoreColor, StoreDepthLow,
        StoreDepthHigh, StoreDrain, Copy, Present, Readback, ReplyBuild, ReplyRead, Reply, Fault, Abort
    } state = State::Idle;
    SystemConfig configuration;
    SdramController memory;
    HostLink host;
    Scanout scanout;
    OpaqueTile tile;
    SystemCopy copier;
    Sdp1kx16 reply_bank;
    Reader reader;
    // The current primitive lives in the tile command registers. This same
    // reader RAM/field bank holds one upcoming record while it draws.
    enum class Prefetch { Idle, Reference, Record, Ready } prefetch = Prefetch::Idle;
    std::uint32_t prefetch_record = 0;
    bool prefetch_invalid = false;
    Writer writer;
    HostPassCompiler compiler; // CPU driver storage, not FPGA resources.
    bool driver_program_pending = false; // CPU driver ownership, not a device bit.
    Statistics counts{};
    SystemStatistics totals{};
    const HostCommand* command = nullptr; // Lease the front packet bank until retirement.
    std::array<std::uint8_t, 64> captured{};
    unsigned capture_size = 0, capture_index = 0;
    bool packet_read_pending = false;
    unsigned packet_read_offset = 0;
    // The C++ system uses the same clocked packet port and existing Writer.
    // This decoder streams one reconstructed word; it never shadows a packet
    // or reserves an additional request/RAM buffer. RTL owns cycle authority.
    std::uint8_t upload_descriptor = 0;
    std::uint32_t upload_decoded_word = 0;
    unsigned upload_encoded_cursor = 8, upload_slot = 0,
             upload_payload_bytes = 0, upload_payload_received = 0;
    bool upload_need_descriptor = true, upload_word_ready = false;
    // A distinct736-bit reply-source register bank:28-byte framing plus at
    // most64 status/capability bytes. READ_MEMORY payload never enters here;
    // it streams through the reader's synchronous native port. ReplyBuild
    // writes one byte per edge into the separately counted reply RAM.
    std::array<std::uint8_t, 92> reply{};
    unsigned reply_index = 0, reply_source_length = 0;
    RowBanks rows;
    bool row_source_depth = false, row_source_pending = false, row_source_word_valid = false;
    unsigned row_source_half = 0, row_source_index = 32, row_source_next = 0,
             row_source_pending_index = 0, unpack_half = 0;
    std::uint16_t row_source_low_color = 0;
    std::uint32_t row_source_word_value = 0;
    // A one-bit role mux exchanges the existing two physical attachment sets.
    // One holds a completed tile for DMA while the other renders its successor.
    // LOAD passes serialize because their row input uses the same idle set.
    enum class TileExport { Idle, PrepareColor, Color, DepthLow, DepthHigh } tile_export = TileExport::Idle;
    bool row_source_staged = false;
    unsigned tile_export_row = 0;
    std::uint32_t tile_export_color_address = 0, tile_export_depth_address = 0;
    bool reply_reader = false, reply_reader_pending = false;
    unsigned reply_length = 0, reply_reader_word = 0;
    std::uint32_t reply_reader_value = 0;
    unsigned boot_fill = 0;
    std::shared_ptr<Observation> observation;
    std::uint64_t fragment_id_base = 0, primitive_id_base = 0;
    bool reset_event_pending = false;
    FragmentCycle edge_event{};
    std::array<FragmentToken, fragment_trace_capacity> cancellation_snapshot{};
    bool tile_edge = false;
    PassDescriptor pass;
    TileEntry bin;
    CopyCommand copy;
    unsigned tile_index = 0, row = 0, beat = 0, reference_index = 0;
    std::uint64_t pass_start = 0, present_start = 0, copy_start = 0;
    std::uint64_t references_seen = 0;
    bool pass_active = false, copy_active = false, copy_was_valid = false;
    bool initialized = false, boot_wait = false;
    std::uint32_t boot_address = 0;
    std::array<bool, 2> valid{{false, false}};
    bool valid_depth = false, sealed = false, driver_present_pending = false;
    std::uint32_t next_sequence = 1, driver_present_sequence = 0;
    SystemFault error = SystemFault::None;
    std::string message;
    std::array<std::vector<std::uint16_t>, 2> observed_color;
    std::vector<std::uint32_t> observed_depth;

    explicit Impl(SystemConfig config, std::shared_ptr<Observation> existing_observation = {})
        : configuration(std::move(config)), memory(configuration.memory), host(configuration.host),
          scanout(configuration.scanout), observation(existing_observation ? std::move(existing_observation) :
              std::make_shared<Observation>()), observed_depth(screen_width * screen_height, depth_max) {
        if (configuration.operation_timeout_cycles == 0)
            throw std::invalid_argument("system operation timeout must be positive");
        for (auto& color : observed_color) color.resize(screen_width * screen_height);
        host.set_capabilities(capabilities());
        host.set_status(status());
        fragment_id_base = observation->last_fragment_id;
        primitive_id_base = observation->last_primitive_id;
        tile.set_cycle_observer([this](const FragmentCycle& event) {
            if (tile_edge) throw std::logic_error("tile advanced twice on one common edge");
            edge_event = event;
            const auto map_token = [this](FragmentToken& token) {
                if (token.id != 0) {
                    token.id += fragment_id_base;
                    token.primitive_id += primitive_id_base;
                }
            };
            map_token(edge_event.accepted_fragment);
            map_token(edge_event.retired_fragment);
            if (event.cancelled_count != 0) {
                if (event.cancelled_count > cancellation_snapshot.size() || event.cancelled_fragments == nullptr)
                    throw std::logic_error("invalid tile cancellation snapshot");
                for (unsigned index = 0; index < event.cancelled_count; ++index) {
                    cancellation_snapshot[index] = event.cancelled_fragments[index];
                    map_token(cancellation_snapshot[index]);
                }
                edge_event.cancelled_fragments = cancellation_snapshot.data();
            }
            tile_edge = true;
        });
    }
    std::array<std::uint8_t, 64> status() const {
        std::array<std::uint8_t, 64> bytes{};
        bytes[1] = static_cast<std::uint8_t>((initialized ? 1 : 0) |
            ((state != State::Idle || !initialized) ? 2 : 0) | (sealed ? 4 : 0) |
            (error != SystemFault::None ? 8 : 0));
        put(bytes, 2, totals.epoch); put(bytes, 6, totals.completed_sequence);
        put(bytes, 10, totals.completed_fence_token); bytes[14] = static_cast<std::uint8_t>(scanout.front_id());
        bytes[15] = static_cast<std::uint8_t>((valid[0] ? 1 : 0) | (valid[1] ? 2 : 0) |
                                           (valid_depth ? 4 : 0));
        put(bytes, 16, totals.core_cycles & UINT64_C(0x0000ffffffffffff), 8);
        put(bytes, 24, scanout.statistics().underflows, 8);
        put(bytes, 32, static_cast<std::uint32_t>(error));
        put(bytes, 36, totals.completed_present_token); put(bytes, 40, memory.statistics().refresh_late_cycles, 8);
        put(bytes, 48, memory.statistics().uninitialized_read_words, 8);
        bytes[56] = static_cast<std::uint8_t>(host.fault());
        return bytes;
    }
    std::array<std::uint8_t, 64> capabilities() const {
        std::array<std::uint8_t, 64> bytes{};
        bytes[0] = protocol_major; bytes[1] = protocol_minor; bytes[2] = 8; bytes[3] = 32;
        put(bytes, 4, memory_bytes); put(bytes, 8, heap_base);
        put(bytes, 12, maximum_packet_payload); put(bytes, 16, primitive_record_bytes);
        put(bytes, 20, maximum_pass_references); put(bytes, 24, configuration.memory.core_hz);
        put(bytes, 28, configuration.host.clock_divider); put(bytes, 32, configuration.scanout.pixel_divider);
        put(bytes, 36, configuration.host.rx_fifo_capacity); put(bytes, 40, configuration.host.command_fifo_capacity);
        put(bytes, 44, configuration.host.response_fifo_capacity); put(bytes, 48, configuration.scanout.fifo_pixels);
        put(bytes, 52, screen_width); put(bytes, 56, screen_height); put(bytes, 60, 0xffu);
        return bytes;
    }
    void require_writable(int target) const {
        check_buffer(target);
        if (target == scanout.front_id() || sealed) throw std::logic_error("front or sealed color buffer is not writable");
    }
    void finish() {
        totals.completed_sequence = command->sequence; ++totals.commands_completed;
        host.pop_command(); command = nullptr; packet_read_pending = false; state = State::Idle;
    }
    void make_reply(std::uint32_t token, const std::vector<std::uint8_t>& data = {}) {
        if (data.size() > 64) throw std::out_of_range("system reply source exceeds bounded registers");
        reply.fill(0); reply_source_length = 28 + static_cast<unsigned>(data.size());
        reply[0] = 'N'; reply[1] = 'G'; reply[2] = 'P'; reply[3] = 'U'; reply[5] = protocol_minor;
        put(reply, 6, static_cast<std::uint16_t>(command->opcode) | 0x8000u, 2);
        put(reply, 8, static_cast<std::uint32_t>(12 + data.size()));
        put(reply, 12, command->sequence); put(reply, 16, totals.epoch);
        put(reply, 20, 0); put(reply, 24, token);
        std::copy(data.begin(), data.end(), reply.begin() + 28);
        reply_index = 0; reply_length = reply_source_length;
        reply_reader = reply_reader_pending = false; state = State::ReplyBuild;
    }
    void fail(SystemFault code, const std::string& reason) {
        if (error == SystemFault::None) { error = code; message = reason; }
        if (pass_active) { valid[pass.target] = false; valid_depth = false; }
        if (copy_active) valid[copy.target] = false;
        sealed = driver_present_pending = false;
        cancel_tile_export();
        state = State::Fault;
    }
    bool drain_issued() {
        if (tile.external_memory_pending()) tile.drain_external_memory_edge();
        if (copier.memory_pending()) copier.tick(memory, tile);
        bool drained = !tile.external_memory_pending() && !copier.memory_pending();
        for (unsigned port = 1; port < memory_client_count; ++port) {
            const auto client = static_cast<MemoryClient>(port);
            // The tile unit owns its own texture response until it consumes it.
            if (client != MemoryClient::Texture && client != MemoryClient::Copy && memory.response_ready(client))
                memory.release_response(client);
            const auto& c = memory.statistics().clients[port];
            drained = drained && c.requests == c.completed;
        }
        return drained;
    }
    std::optional<std::uint8_t> packet_byte(unsigned offset) {
        if (packet_read_pending) {
            if (!host.payload_byte_valid()) return {};
            if (host.payload_byte_offset() != packet_read_offset)
                throw std::logic_error("host packet RAM response lost its owner");
            const auto byte = host.payload_byte(); packet_read_pending = false;
            if (offset != packet_read_offset) throw std::logic_error("host payload read changed while pending");
            return byte;
        }
        host.request_payload_byte(offset); packet_read_pending = true; packet_read_offset = offset;
        return {};
    }
    std::optional<std::uint8_t> upload_encoded_byte() {
        if (upload_encoded_cursor >= command->payload_size)
            throw std::invalid_argument("truncated encoded WRITE_HEAP stream");
        if (const auto byte = packet_byte(upload_encoded_cursor)) {
            ++upload_encoded_cursor;
            return byte;
        }
        return {};
    }
    std::optional<std::uint8_t> upload_byte(unsigned index) {
        if (!upload_word_ready) {
            if (upload_need_descriptor) {
                const auto byte = upload_encoded_byte();
                if (!byte) return {};
                upload_descriptor = *byte;
                upload_need_descriptor = false;
                upload_slot = 0;
                upload_payload_received = 0;
                upload_decoded_word = 0;
                return {};
            }
            const auto code = upload_descriptor & 3u;
            upload_payload_bytes = code == 3 ? 4 : code;
            if (upload_payload_received != upload_payload_bytes) {
                const auto byte = upload_encoded_byte();
                if (!byte) return {};
                upload_decoded_word |= static_cast<std::uint32_t>(*byte) << (8 * upload_payload_received++);
                if (upload_payload_received != upload_payload_bytes) return {};
            }
            if (code == 1 && (upload_decoded_word & 0x80u)) upload_decoded_word |= 0xffffff00u;
            if (code == 2 && (upload_decoded_word & 0x8000u)) upload_decoded_word |= 0xffff0000u;
            upload_word_ready = true;
        }
        const auto byte = static_cast<std::uint8_t>(upload_decoded_word >> (8 * (index & 3u)));
        if ((index & 3u) == 3 || index + 1 == writer.length) {
            upload_descriptor >>= 2;
            if (index + 1 == writer.length &&
                (upload_descriptor != 0 || upload_encoded_cursor != command->payload_size))
                throw std::invalid_argument("unused codes or trailing encoded WRITE_HEAP bytes");
            upload_word_ready = false;
            upload_decoded_word = 0;
            upload_payload_received = 0;
            if (++upload_slot == 4) upload_need_descriptor = true;
        }
        return byte;
    }
    void acquire() {
        command = &host.front_command(); captured.fill(0); capture_index = 0; packet_read_pending = false;
        unsigned exact = 0;
        switch (command->opcode) {
        case Opcode::GetCaps: case Opcode::GetStatus: exact = 0; break;
        case Opcode::WriteHeap:
            if (command->payload_size <= 8 || command->payload_size > maximum_packet_payload)
                throw std::invalid_argument("WRITE_HEAP payload length");
            capture_size = 8; state = State::Capture; return;
        case Opcode::SubmitPass: case Opcode::Fence: exact = 4; break;
        case Opcode::Present: exact = 8; break;
        case Opcode::CopyRect: exact = copy_payload_bytes; break;
        case Opcode::ReadMemory: exact = 12; break;
        }
        if (command->payload_size != exact) throw std::invalid_argument("command payload length");
        capture_size = exact; state = State::Capture;
    }
    void execute_captured() {
        switch (command->opcode) {
        case Opcode::GetCaps: {
            const auto value = capabilities(); make_reply(0, {value.begin(), value.end()}); break;
        }
        case Opcode::GetStatus: {
            const auto value = status(); make_reply(0, {value.begin(), value.end()}); break;
        }
        case Opcode::WriteHeap: {
            const auto address = get(captured, 0), length = get(captured, 4);
            if (length == 0 || length > maximum_upload_decoded_bytes)
                throw std::invalid_argument("WRITE_HEAP data length");
            validate_heap_range(address, length, 1);
            // Low-level host writes may reuse any heap address. The CPU
            // convenience driver must discard its residency knowledge then.
            if (!driver_program_pending) compiler.reset();
            upload_encoded_cursor = 8; upload_slot = 0;
            upload_need_descriptor = true; upload_word_ready = false;
            upload_payload_received = 0; upload_decoded_word = 0;
            writer.start(address, length, MemoryClient::Host,
                [this](unsigned index) { return upload_byte(index); });
            state = State::Upload; break;
        }
        case Opcode::SubmitPass:
            validate_heap_range(get(captured, 0), pass_descriptor_bytes);
            pass_start = totals.core_cycles; state = State::PassInvalidate; break;
        case Opcode::Fence:
            totals.completed_fence_token = get(captured, 0); make_reply(get(captured, 0)); break;
        case Opcode::Present:
            require_writable(static_cast<int>(get(captured, 0)));
            if (!valid[get(captured, 0)]) throw std::logic_error("cannot present invalid color contents");
            sealed = true; present_start = totals.core_cycles; state = State::Present; break;
        case Opcode::CopyRect: {
            copy = decode_copy_command({captured.begin(), captured.begin() + capture_size});
            require_writable(copy.target);
            if (!copy.image_source && !valid[copy.source]) throw std::logic_error("copy source contents invalid");
            copy_was_valid = valid[copy.target]; copier.begin(copy);
            copy_active = true; copy_start = totals.core_cycles; state = State::Copy; break;
        }
        case Opcode::ReadMemory: {
            const auto address = get(captured, 0), length = get(captured, 4);
            if (length == 0 || length > 512 || static_cast<std::uint64_t>(address) + length > memory_bytes)
                throw std::out_of_range("READ_MEMORY length/range");
            reader.start(address, length, MemoryClient::Command, false); state = State::Readback; break;
        }
        }
    }
    Point origin() const {
        return {static_cast<int>(tile_index % tile_columns) * tile_size,
                static_cast<int>(tile_index / tile_columns) * tile_size};
    }
    std::uint32_t global_pixel() const {
        const auto p = origin(); return static_cast<std::uint32_t>((p.y + row) * screen_width + p.x);
    }
    void fetch_entry() {
        reader.start(pass.tile_table_address + tile_index * tile_entry_bytes,
                     tile_entry_bytes, MemoryClient::Command); state = State::TileEntry;
    }
    void fetch_reference() {
        reader.start(bin.address + reference_index * 4, 4, MemoryClient::Command); state = State::Reference;
    }
    void begin_record_draw() {
        const auto decoded = decode_primitive(reader.view());
        tile.begin_external(decoded.primitive, origin(), decoded.texture, memory);
        state = State::Draw;
        prefetch = Prefetch::Idle; prefetch_invalid = false;
        if (reference_index + 1 < bin.count) {
            reader.start(bin.address + (reference_index + 1) * 4, 4, MemoryClient::Command);
            prefetch = Prefetch::Reference;
        }
    }
    void advance_prefetch() {
        if (prefetch == Prefetch::Idle || prefetch == Prefetch::Ready) return;
        const bool completed = reader.done();
        reader.tick(memory);
        if (!completed) return;
        if (prefetch == Prefetch::Reference) {
            prefetch_record = read_u32(reader.view(), 0);
            totals.fetched_bin_bytes += 4;
            // Defer malformed-reference faults until the current draw drains,
            // without issuing an invalid speculative memory request.
            prefetch_invalid = (prefetch_record & 3u) != 0 || prefetch_record < heap_base ||
                static_cast<std::uint64_t>(prefetch_record) + primitive_record_bytes > memory_bytes;
            if (prefetch_invalid) prefetch = Prefetch::Ready;
            else {
                reader.start(prefetch_record, primitive_record_bytes, MemoryClient::Command);
                prefetch = Prefetch::Record;
            }
        } else {
            totals.fetched_record_bytes += primitive_record_bytes;
            ++totals.prefetched_records;
            prefetch = Prefetch::Ready;
        }
    }
    void begin_row_load() {
        beat = 0;
        if (pass.color_load == LoadOp::Load) {
            reader.start(color_buffer_base(pass.target) + global_pixel() * 2, 64, MemoryClient::Tile);
            state = State::LoadColor;
        } else begin_depth_load();
    }
    void begin_depth_load() {
        if (pass.depth_load == LoadOp::Load) {
            reader.start(depth_buffer_base + global_pixel() * 4, 64, MemoryClient::Tile);
            state = State::LoadDepthLow;
        } else { beat = 0; state = State::LoadLocal; }
    }
    std::optional<std::uint32_t> row_word(unsigned index) {
        // Keep a completed word stable if the destination stalls. One pending
        // native read may be retained alongside it; that RAM output remains
        // established until the next request and is never read combinationally.
        if (row_source_word_valid && row_source_index == index) return row_source_word_value;
        row_source_word_valid = false;
        if (row_source_pending) {
            const auto value = row_source_staged ? rows.result() : tile.read_word_result();
            row_source_pending = false;
            const auto element = row_source_pending_index;
            if (row_source_depth) {
                row_source_word_value = value.depth;
                row_source_index = element - row_source_half * 16;
                row_source_word_valid = true;
            } else if ((element & 1u) == 0) row_source_low_color = value.color;
            else {
                row_source_word_value = row_source_low_color | (std::uint32_t{value.color} << 16);
                row_source_index = element / 2;
                row_source_word_valid = true;
            }
        }
        // Pipeline one pixel ahead: depth supplies one x32 word per edge,
        // RGB565 packs two successive x16 reads into each x32 word.
        const auto end = row_source_depth ? (row_source_half + 1) * 16 : 32;
        if (row_source_next < end) {
            row_source_pending_index = row_source_next++;
            if (row_source_staged) rows.read(tile_export_row * tile_size + row_source_pending_index);
            else tile.read_word_request(static_cast<std::uint16_t>(row * tile_size + row_source_pending_index));
            row_source_pending = true;
        }
        if (!row_source_word_valid) return {};
        if (row_source_index != index) throw std::logic_error("row word source lost its sequential offset");
        return row_source_word_value;
    }
    void prepare_store(bool depth, unsigned half = 0, bool staged = false) {
        row_source_depth = depth; row_source_half = half; row_source_staged = staged;
        row_source_index = 32; row_source_pending = row_source_word_valid = false;
        row_source_next = depth ? half * 16 : 0;
        const auto address = staged ? (depth ? tile_export_depth_address + tile_export_row * screen_width * 4 + half * 64 :
                                               tile_export_color_address + tile_export_row * screen_width * 2) :
            (depth ? depth_buffer_base + global_pixel() * 4 + half * 64 :
                     color_buffer_base(pass.target) + global_pixel() * 2);
        writer.start_words(address, 64, MemoryClient::Tile,
            [this](unsigned index) { return row_word(index); });
    }
    void finish_store_row() {
        if (++row != 32) { beat = 0; state = State::StoreLocal; }
        else {
            ++counts.tiles_processed;
            if (++tile_index != tile_count) fetch_entry(); else finish_pass();
        }
    }
    bool can_overlap_tile_export() const {
        return pass.color_load == LoadOp::Clear && pass.depth_load == LoadOp::Clear;
    }
    void cancel_tile_export() {
        tile_export = TileExport::Idle;
        row_source_pending = row_source_word_valid = false;
        writer.state = Writer::State::Idle;
    }
    void advance_tile_export() {
        if (tile_export == TileExport::Idle) return;
        ++totals.tile_export_cycles;
        if (state != State::StoreLocal && state != State::StoreDrain)
            ++totals.tile_export_overlap_cycles;
        switch (tile_export) {
        case TileExport::Idle: break;
        case TileExport::PrepareColor:
            prepare_store(false, 0, true); tile_export = TileExport::Color; break;
        case TileExport::Color:
            writer.tick(memory);
            if (writer.done()) {
                if (pass.depth_store == StoreOp::Store) { prepare_store(true, 0, true); tile_export = TileExport::DepthLow; }
                else if (++tile_export_row == tile_size) tile_export = TileExport::Idle;
                else tile_export = TileExport::PrepareColor;
            }
            break;
        case TileExport::DepthLow:
            writer.tick(memory);
            if (writer.done()) { prepare_store(true, 1, true); tile_export = TileExport::DepthHigh; }
            break;
        case TileExport::DepthHigh:
            writer.tick(memory);
            if (writer.done()) {
                if (++tile_export_row == tile_size) tile_export = TileExport::Idle;
                else tile_export = TileExport::PrepareColor;
            }
            break;
        }
    }
    void finish_pass() {
        const auto& c = tile.statistics();
        counts.candidates = c.candidates; counts.covered_fragments = c.covered_fragments;
        counts.depth_rejected = c.depth_rejected; counts.keyed_rejected = c.keyed_rejected;
        counts.texture_samples = c.texture_samples; counts.color_writes = c.color_writes;
        counts.depth_writes = c.depth_writes; counts.hardware_cycles = c.cycles;
        counts.hardware_stalls = c.stalls; counts.hardware_setup_cycles = c.setup_cycles;
        counts.hardware_forwarded_reads = c.forwarded_reads;
        counts.hardware_perspective_cycles = c.perspective_cycles;
        counts.hardware_texture_cycles = c.texture_cycles; counts.hardware_texture_hits = c.texture_hits;
        counts.hardware_texture_misses = c.texture_misses; counts.hardware_texture_refill_words = c.texture_refill_words;
        valid[pass.target] = true; valid_depth = pass.depth_store == StoreOp::Store; pass_active = false;
        totals.last_pass_cycles = totals.core_cycles - pass_start + 1; ++totals.passes_completed; finish();
    }
    void execute() {
        switch (state) {
        case State::Idle:
            if (host.command_ready()) acquire();
            break;
        case State::Capture:
            if (capture_index == capture_size) execute_captured();
            else if (const auto value = packet_byte(capture_index)) captured[capture_index++] = *value;
            break;
        case State::Upload:
            writer.tick(memory);
            if (writer.done()) { totals.uploaded_bytes += writer.length; finish(); }
            break;
        case State::PassInvalidate:
            // Uploads may replace texels at an unchanged address. Invalidation
            // consumes its own real cache-control edge before pass fetching.
            tile.invalidate_texture();
            prefetch = Prefetch::Idle;
            reader.start(get(captured, 0), pass_descriptor_bytes, MemoryClient::Command);
            state = State::PassDescriptor;
            break;
        case State::PassDescriptor: {
            const bool completed = reader.done();
            reader.tick(memory);
            if (completed) {
                pass = decode_pass_descriptor(reader.view()); require_writable(pass.target);
                if ((pass.color_load == LoadOp::Load && !valid[pass.target]) ||
                    (pass.depth_load == LoadOp::Load && !valid_depth))
                    throw std::logic_error("LOAD requires valid attachments");
                pass_active = true; valid[pass.target] = valid_depth = false;
                tile.reset(); counts = {}; tile_index = 0; references_seen = 0; fetch_entry();
            }
            break;
        }
        case State::TileEntry: {
            const bool completed = reader.done();
            reader.tick(memory);
            if (completed) {
                bin = decode_tile_entry(reader.view()); totals.fetched_bin_bytes += tile_entry_bytes;
                references_seen += bin.count;
                if (references_seen > maximum_pass_references) throw std::out_of_range("pass reference limit");
                counts.tile_references += bin.count; row = 0; state = State::LoadRow;
            }
            break;
        }
        case State::LoadRow: begin_row_load(); break;
        case State::LoadColor: {
            const bool completed = reader.done();
            reader.tick(memory);
            if (completed) {
                beat = 0; state = State::UnpackColor;
            }
            break;
        }
        case State::UnpackColor: {
            const auto color = static_cast<std::uint16_t>(reader.fields[2*beat] | (reader.fields[2*beat+1] << 8));
            rows.write(beat, {color, 0}, true, false);
            if (++beat == 32) begin_depth_load();
            break;
        }
        case State::LoadDepthLow: case State::LoadDepthHigh: {
            const bool high = state == State::LoadDepthHigh;
            const bool completed = reader.done();
            reader.tick(memory);
            if (completed) {
                beat = 0; unpack_half = high ? 1 : 0; state = State::UnpackDepth;
            }
            break;
        }
        case State::UnpackDepth: {
            std::uint32_t depth = 0;
            for (unsigned byte = 0; byte < 4; ++byte)
                depth |= std::uint32_t{reader.fields[4*beat+byte]} << (8*byte);
            rows.write(16 * unpack_half + beat, {0, depth}, false, true);
            if (++beat == 16) {
                if (unpack_half == 0) {
                    reader.start(depth_buffer_base + global_pixel() * 4 + 64, 64, MemoryClient::Tile);
                    state = State::LoadDepthHigh;
                } else { beat = 0; state = State::LoadLocal; }
            }
            break;
        }
        case State::LoadLocal:
            if (!can_overlap_tile_export()) rows.read(0);
            beat = 0; state = State::LoadLocalCapture; break;
        case State::LoadLocalCapture: {
            const auto value = can_overlap_tile_export() ? TileWord{} : rows.result();
            tile.load_word(static_cast<std::uint16_t>(row * tile_size + beat),
                pass.color_load == LoadOp::Clear ? pass.clear_color : value.color,
                pass.depth_load == LoadOp::Clear ? pass.clear_depth : value.depth);
            if (++beat == 32) {
                if (++row == 32) {
                    reference_index = 0;
                    if (bin.count != 0) fetch_reference();
                    else { row = beat = 0; state = State::StoreLocal; }
                } else state = State::LoadRow;
            } else if (!can_overlap_tile_export()) rows.read(beat);
            break;
        }
        case State::Reference: {
            const bool completed = reader.done();
            reader.tick(memory);
            if (completed) {
                const auto record = read_u32(reader.view(), 0);
                validate_heap_range(record, primitive_record_bytes);
                totals.fetched_bin_bytes += 4;
                reader.start(record, primitive_record_bytes, MemoryClient::Command); state = State::Record;
            }
            break;
        }
        case State::Record: {
            const bool completed = reader.done();
            reader.tick(memory);
            if (completed) {
                totals.fetched_record_bytes += primitive_record_bytes;
                begin_record_draw();
            }
            break;
        }
        case State::Prefetched:
            advance_prefetch();
            if (prefetch == Prefetch::Ready) {
                if (prefetch_invalid) validate_heap_range(prefetch_record, primitive_record_bytes);
                begin_record_draw();
            } else ++totals.record_prefetch_wait_cycles;
            break;
        case State::Draw:
            if (prefetch == Prefetch::Reference || prefetch == Prefetch::Record)
                ++totals.record_prefetch_overlap_cycles;
            advance_prefetch();
            tile.tick();
            if (!tile.busy()) {
                if (++reference_index < bin.count) state = State::Prefetched;
                else { row = beat = 0; state = State::StoreLocal; }
            }
            break;
        case State::StoreLocal:
            if (can_overlap_tile_export()) {
                if (tile_export != TileExport::Idle) { ++totals.tile_export_wait_cycles; break; }
                rows.exchange_with(tile);
                ++totals.tile_bank_exchanges;
                totals.tile_staged_pixels += tile_size * tile_size;
                tile_export_row = 0;
                tile_export_color_address = color_buffer_base(pass.target) + global_pixel() * 2;
                tile_export_depth_address = depth_buffer_base + global_pixel() * 4;
                tile_export = TileExport::PrepareColor;
                ++counts.tiles_processed;
                if (++tile_index != tile_count) fetch_entry();
                else state = State::StoreDrain;
                break;
            }
            [[fallthrough]];
        case State::StorePrepare:
            // Read the resident tile directly while filling the SDRAM slot.
            // The row banks remain a LOAD staging resource; exporting does
            // not copy the row through a second set of synchronous RAM ports.
            prepare_store(false); state = State::StoreColor;
            break;
        case State::StoreDrain:
            if (tile_export == TileExport::Idle) finish_pass();
            break;
        case State::StoreColor:
            writer.tick(memory);
            if (writer.done()) {
                if (pass.depth_store == StoreOp::Store) { prepare_store(true); state = State::StoreDepthLow; }
                else finish_store_row();
            }
            break;
        case State::StoreDepthLow:
            writer.tick(memory);
            if (writer.done()) { prepare_store(true, 1); state = State::StoreDepthHigh; }
            break;
        case State::StoreDepthHigh:
            writer.tick(memory);
            if (writer.done()) finish_store_row();
            break;
        case State::Copy:
            copier.tick(memory, tile);
            if (!copier.busy()) {
                counts.hardware_copy_cycles = copier.statistics().cycles;
                counts.hardware_copy_words = copier.statistics().words;
                const auto x0 = std::max<std::int64_t>({0, -std::int64_t{copy.source_origin.x}, -std::int64_t{copy.destination_origin.x}});
                const auto y0 = std::max<std::int64_t>({0, -std::int64_t{copy.source_origin.y}, -std::int64_t{copy.destination_origin.y}});
                const int sw = copy.image_source ? static_cast<int>(copy.image.width) : screen_width;
                const int sh = copy.image_source ? static_cast<int>(copy.image.height) : screen_height;
                const auto x1 = std::min<std::int64_t>({copy.width, std::int64_t{sw}-copy.source_origin.x,
                                                     std::int64_t{screen_width}-copy.destination_origin.x});
                const auto y1 = std::min<std::int64_t>({copy.height, std::int64_t{sh}-copy.source_origin.y,
                                                     std::int64_t{screen_height}-copy.destination_origin.y});
                valid[copy.target] = copy_was_valid || (copy.destination_origin.x+x0 == 0 &&
                    copy.destination_origin.y+y0 == 0 && copy.destination_origin.x+x1 == screen_width &&
                    copy.destination_origin.y+y1 == screen_height);
                copy_active = false; ++totals.copies_completed; finish();
            }
            break;
        case State::Present:
            if (scanout.vertical_blank_pulse()) {
                scanout.set_front(static_cast<int>(get(captured, 0))); sealed = false;
                totals.completed_present_token = get(captured, 4); ++totals.presentations_completed;
                totals.last_present_wait_cycles = totals.core_cycles - present_start;
                make_reply(get(captured, 4));
            }
            break;
        case State::Readback: {
            const bool completed = reader.done();
            reader.tick(memory);
            if (completed) {
                auto result = scalar(get(captured, 0)); append(result, scalar(get(captured, 4)));
                make_reply(get(captured, 8), result);
                reply_reader = true; reply_length += reader.length;
                put(reply, 8, 20 + reader.length);
                reply_reader_word = std::numeric_limits<unsigned>::max();
            }
            break;
        }
        case State::ReplyBuild: {
            std::uint8_t byte = 0;
            if (reply_reader && reply_index >= reply_source_length) {
                const auto at = reply_index - reply_source_length + (reader.address & 3u);
                if (reply_reader_pending) {
                    reply_reader_value = reader.result(); reply_reader_pending = false;
                    reply_reader_word = at / 4;
                }
                if (reply_reader_word != at / 4) {
                    reader.ports(false, 0, 0, true, at / 4); reply_reader_pending = true;
                    reply_bank.tick({}); break;
                }
                byte = static_cast<std::uint8_t>(reply_reader_value >> (8 * (at & 3u)));
                reader.ports();
            } else byte = reply[reply_index];
            reply_bank.tick({true, static_cast<std::uint16_t>(reply_index), byte, false, 0, false});
            if (++reply_index == reply_length) { reply_index = 0; state = State::ReplyRead; }
            break;
        }
        case State::ReplyRead:
            reply_bank.tick({false, 0, 0, true, static_cast<std::uint16_t>(reply_index), false});
            state = State::Reply;
            break;
        case State::Reply:
            if (host.push_response_byte(static_cast<std::uint8_t>(reply_bank.output()))) {
                if (++reply_index == reply_length) finish(); else state = State::ReplyRead;
            }
            reply_bank.tick({});
            break;
        case State::Fault: (void)drain_issued(); break;
        case State::Abort:
            if (drain_issued() && !tile_edge) {
                if (totals.epoch == std::numeric_limits<std::uint32_t>::max())
                    throw std::overflow_error("system epoch exhausted; start a fresh power-on session");
                copier.reset(); tile.reset(); reader.state = Reader::State::Idle; writer.state = Writer::State::Idle;
                for (unsigned client = 1; client < memory_client_count; ++client)
                    memory.discard_request_words(static_cast<MemoryClient>(client));
                host.recover(++totals.epoch); next_sequence = 1;
                totals.completed_sequence = totals.completed_fence_token = totals.completed_present_token = 0;
                command = nullptr; packet_read_pending = false; pass_active = copy_active = false;
                prefetch = Prefetch::Idle; compiler.reset();
                sealed = driver_present_pending = false; error = SystemFault::None; message.clear(); state = State::Idle;
                if (!initialized) { boot_address = 0; boot_wait = false; boot_fill = 0; }
            }
            break;
        }
    }
    void boot() {
        if (!memory.initialized()) return;
        if (boot_wait) {
            if (!memory.response_ready(MemoryClient::Tile)) return;
            memory.release_response(MemoryClient::Tile);
            boot_address += 4 * memory_burst_words; boot_wait = false; boot_fill = 0;
            if (boot_address == heap_base) {
                initialized = true; valid = {{true, true}}; valid_depth = true;
                totals.boot_cycles = totals.core_cycles; scanout.set_memory_ready(true); return;
            }
        }
        MemoryRequest request; request.write = true; request.address = boot_address; request.words = memory_burst_words;
        if (boot_fill < request.words) {
            if (memory.write_request_word(MemoryClient::Tile, boot_fill,
                    boot_address >= depth_buffer_base ? depth_max : 0, 0xf)) ++boot_fill;
            return;
        }
        if (memory.submit_prepared(MemoryClient::Tile, request)) boot_wait = true;
    }
    HostBusResult tick(const HostPins& pins) {
        tile_edge = false;
        edge_event = {};
        edge_event.stage = pass_active ? FragmentStage::Transfer : FragmentStage::Idle;
        edge_event.inflight_before = edge_event.inflight_after = observation->active_count;
        if (reset_event_pending) {
            edge_event.stage = FragmentStage::Reset;
            edge_event.cycle = tile.last_cycle().cycle;
            edge_event.inflight_after = 0;
            for (const auto& token : observation->active_fragments) {
                if (token.id != 0) cancellation_snapshot[edge_event.cancelled_count++] = token;
            }
            if (edge_event.cancelled_count != 0)
                edge_event.cancelled_fragments = cancellation_snapshot.data();
            reset_event_pending = false;
        }
        // The host samples established device registers, never state newly
        // produced by the controller/scanout on this same common edge.
        if (pins.read && pins.register_address == 1) host.set_status(status());
        ++totals.core_cycles;
        if (host.abort_requested() && state != State::Abort) {
            if (pass_active) { valid[pass.target] = false; valid_depth = false; }
            if (copy_active) valid[copy.target] = false;
            cancel_tile_export();
            sealed = driver_present_pending = false; state = State::Abort; ++totals.aborts;
        }
        if (host.faulted() && state != State::Abort && error == SystemFault::None)
            fail(SystemFault::Transport, "parallel link or packet decoder fault");
        try {
            if (initialized && state != State::Abort && state != State::Fault) advance_tile_export();
            if (state == State::Abort || state == State::Fault) execute();
            else if (!initialized) boot();
            else execute();
        } catch (const std::exception& exception) { fail(SystemFault::Record, exception.what()); }
        // Consumers see only responses established by a previous memory edge.
        scanout.tick(memory);
        const auto result = host.tick(pins);
        rows.tick();
        memory.tick();
        const bool new_token_retirement = edge_event.accepted && edge_event.retired &&
            edge_event.accepted_fragment.id == edge_event.retired_fragment.id;
        if (new_token_retirement) observation->accept(edge_event.accepted_fragment);
        if (edge_event.retired) observation->retire(edge_event.retired_fragment);
        for (unsigned index = 0; index < edge_event.cancelled_count; ++index)
            observation->retire(edge_event.cancelled_fragments[index]);
        if (edge_event.accepted && !new_token_retirement) observation->accept(edge_event.accepted_fragment);
        if (observation->active_count != edge_event.inflight_after)
            throw std::logic_error("system observer occupancy differs from the fragment pipeline");
        ++observation->clock;
        if (observation->callback) observation->callback(observation->clock, edge_event);
        return result;
    }
};

SystemModel::SystemModel(SystemConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}
SystemModel::~SystemModel() = default;
SystemModel::SystemModel(SystemModel&&) noexcept = default;
SystemModel& SystemModel::operator=(SystemModel&&) noexcept = default;
HostBusResult SystemModel::tick(const HostPins& pins) {
    if (impl_->host.reset_requested()) {
        const auto config = impl_->configuration;
        const auto epoch = impl_->totals.epoch;
        const auto observation = impl_->observation;
        if (epoch == std::numeric_limits<std::uint32_t>::max()) throw std::overflow_error("system epoch exhausted");
        impl_ = std::make_unique<Impl>(config, observation);
        impl_->reset_event_pending = true;
        impl_->host.recover(epoch + 1); impl_->totals.epoch = epoch + 1;
    }
    return impl_->tick(pins);
}
void SystemModel::advance(std::uint64_t cycles) { while (cycles-- != 0) tick(); }
void SystemModel::set_cycle_observer(SystemCycleObserver observer) { impl_->observation->callback = std::move(observer); }
void SystemModel::reset() {
    auto& p = *impl_;
    if (p.totals.epoch == std::numeric_limits<std::uint32_t>::max()) throw std::overflow_error("system epoch exhausted");
    const auto epoch = p.totals.epoch + 1; const auto config = p.configuration;
    const auto observation = p.observation;
    impl_ = std::make_unique<Impl>(config, observation);
    impl_->reset_event_pending = true;
    impl_->host.recover(epoch); impl_->totals.epoch = epoch;
    (void)impl_->tick({});
}
void SystemModel::abort() {
    HostPins pins; pins.abort = true; tick(pins);
    const auto epoch = impl_->totals.epoch;
    const auto start = impl_->totals.core_cycles;
    while (impl_->totals.epoch == epoch) {
        tick();
        if (impl_->totals.core_cycles - start > impl_->configuration.operation_timeout_cycles)
            throw std::runtime_error("system abort drain timed out");
    }
}
bool SystemModel::ready() const { return impl_->initialized; }
bool SystemModel::busy() const {
    return !ready() || impl_->state != Impl::State::Idle || impl_->host.command_ready() || impl_->host.rx_size() != 0;
}
bool SystemModel::faulted() const { return impl_->error != SystemFault::None || impl_->host.faulted(); }
SystemFault SystemModel::fault() const { return impl_->error; }
const std::string& SystemModel::fault_message() const { return impl_->message; }
void SystemModel::send_bytes(const std::vector<std::uint8_t>& bytes) {
    auto& p = *impl_; const auto start = p.totals.core_cycles;
    const auto phase = (p.configuration.host.transfer_phase + 1) % p.configuration.host.clock_divider;
    while (p.host.statistics().cycles % p.configuration.host.clock_divider != phase) tick();
    for (std::size_t index = 0; index < bytes.size();) {
        HostPins pins; pins.write_valid = true; pins.data = bytes[index];
        if (tick(pins).write_accepted) ++index;
        if (faulted()) throw std::runtime_error("system submission failed: " + fault_message());
        if (p.totals.core_cycles - start > p.configuration.operation_timeout_cycles)
            throw std::runtime_error("host submission timed out under backpressure");
    }
    // Honour the selected hold interval and quiesce before changing direction.
    for (unsigned edge = 0; edge < p.configuration.host.hold_cycles; ++edge) tick();
}
std::vector<std::uint8_t> SystemModel::read_reply(Opcode opcode, std::uint32_t sequence) {
    auto& p = *impl_; const auto start = p.totals.core_cycles;
    const auto phase = (p.configuration.host.transfer_phase + 1) % p.configuration.host.clock_divider;
    while (p.host.statistics().cycles % p.configuration.host.clock_divider != phase) tick();
    std::vector<std::uint8_t> response;
    std::size_t expected = 16;
    while (response.size() < expected) {
        HostPins pins; pins.read = true;
        const auto value = tick(pins);
        if (value.read_valid) {
            response.push_back(value.read_data);
            if (response.size() == 16) {
                if (read_u32(response, 0) != 0x5550474eu || response[4] != protocol_major || response[5] != protocol_minor ||
                    read_u16(response, 6) != (static_cast<std::uint16_t>(opcode) | 0x8000u) ||
                    read_u32(response, 12) != sequence || read_u32(response, 8) > 780)
                    throw std::runtime_error("invalid/stale GPU response header");
                expected += read_u32(response, 8);
            }
        }
        if (faulted()) throw std::runtime_error("GPU execution failed: " + fault_message());
        if (p.totals.core_cycles - start > p.configuration.operation_timeout_cycles)
            throw std::runtime_error("GPU response timed out");
    }
    tick();
    if (response.size() < 28 || read_u32(response, 16) != p.totals.epoch || read_u32(response, 20) != 0)
        throw std::runtime_error("failed or stale GPU completion");
    return response;
}
void SystemModel::observe_memory() {
    auto& p = *impl_;
    for (unsigned id = 0; id < 2; ++id)
        for (unsigned word = 0; word < color_buffer_bytes / 4; ++word) {
            const auto value = p.memory.inspect_word(color_buffer_base(id) + 4 * word);
            p.observed_color[id][2*word] = static_cast<std::uint16_t>(value);
            p.observed_color[id][2*word+1] = static_cast<std::uint16_t>(value >> 16);
        }
    for (unsigned word = 0; word < depth_buffer_bytes / 4; ++word)
        p.observed_depth[word] = p.memory.inspect_word(depth_buffer_base + 4 * word);
}
void SystemModel::render(const Pass& pass, RenderMode) {
    auto& p = *impl_;
    const auto program = p.compiler.compile(pass, p.next_sequence);
    p.next_sequence = program.next_sequence;
    const auto start = p.totals.core_cycles;
    p.driver_program_pending = true;
    try {
        send_bytes(program.bytes); const auto response = read_reply(Opcode::Fence, program.fence_sequence);
        if (read_u32(response, 24) != program.fence_token) throw std::runtime_error("wrong fence token");
        p.compiler.complete();
    } catch (...) {
        p.compiler.reset(); p.driver_program_pending = false; throw;
    }
    p.driver_program_pending = false;
    p.totals.last_submission_cycles = p.totals.core_cycles - start; observe_memory();
}
void SystemModel::copy_rect(int source, int target, Point source_origin, Point destination_origin,
                            int width, int height) {
    auto& p = *impl_;
    CopyCommand command; command.source = source; command.target = target;
    command.source_origin = source_origin; command.destination_origin = destination_origin;
    command.width = width; command.height = height;
    if (p.next_sequence > std::numeric_limits<std::uint32_t>::max() - 2) throw std::overflow_error("host sequence exhausted");
    auto bytes = encode_packet(Opcode::CopyRect, p.next_sequence++, encode_copy_command(command));
    const auto fence = p.next_sequence++; append(bytes, encode_packet(Opcode::Fence, fence, scalar(fence)));
    send_bytes(bytes); (void)read_reply(Opcode::Fence, fence); observe_memory();
}
void SystemModel::copy_image(const Image& image, int target, Point source_origin, Point destination_origin,
                             int width, int height) {
    auto& p = *impl_;
    p.compiler.reset(); // The explicit heap upload may overwrite resident textures.
    TextureDescriptor descriptor{heap_base, static_cast<std::uint32_t>(image.width()),
        static_cast<std::uint32_t>(image.height()), static_cast<std::uint32_t>(image.pitch_bytes())};
    const auto extent = texture_extent(descriptor);
    std::vector<std::uint8_t> data(extent, 0), program;
    for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x) {
        const auto value = image.at(x, y); const auto offset = y * descriptor.pitch_bytes + 2 * x;
        data[offset] = static_cast<std::uint8_t>(value); data[offset+1] = static_cast<std::uint8_t>(value >> 8);
    }
    for (std::size_t offset = 0; offset < data.size();) {
        if (p.next_sequence == std::numeric_limits<std::uint32_t>::max()) throw std::overflow_error("host sequence exhausted");
        const auto size = std::min<std::size_t>(maximum_upload_decoded_bytes, data.size() - offset);
        const auto payload = encode_heap_upload(heap_base + static_cast<std::uint32_t>(offset),
            {data.begin() + static_cast<std::ptrdiff_t>(offset), data.begin() + static_cast<std::ptrdiff_t>(offset + size)});
        append(program, encode_packet(Opcode::WriteHeap, p.next_sequence++, payload)); offset += size;
    }
    CopyCommand command; command.image_source = true; command.image = descriptor; command.target = target;
    command.source_origin = source_origin; command.destination_origin = destination_origin;
    command.width = width; command.height = height;
    if (p.next_sequence > std::numeric_limits<std::uint32_t>::max() - 2) throw std::overflow_error("host sequence exhausted");
    append(program, encode_packet(Opcode::CopyRect, p.next_sequence++, encode_copy_command(command)));
    const auto fence = p.next_sequence++; append(program, encode_packet(Opcode::Fence, fence, scalar(fence)));
    send_bytes(program); (void)read_reply(Opcode::Fence, fence); observe_memory();
}
void SystemModel::queue_present(int target) {
    auto& p = *impl_; check_buffer(target);
    if (p.driver_present_pending) throw std::logic_error("a presentation is already queued");
    if (p.next_sequence == std::numeric_limits<std::uint32_t>::max()) throw std::overflow_error("host sequence exhausted");
    p.driver_present_sequence = p.next_sequence++;
    auto payload = scalar(static_cast<std::uint32_t>(target)); append(payload, scalar(p.driver_present_sequence));
    send_bytes(encode_packet(Opcode::Present, p.driver_present_sequence, payload)); p.driver_present_pending = true;
}
void SystemModel::vertical_blank() {
    auto& p = *impl_; if (!p.driver_present_pending) return;
    (void)read_reply(Opcode::Present, p.driver_present_sequence); p.driver_present_pending = false;
    const auto start = p.totals.core_cycles; const auto before = p.scanout.statistics().completed_frames;
    while (p.scanout.statistics().completed_frames == before || p.scanout.completed_frame_front_id() != p.scanout.front_id()) {
        tick();
        if (faulted()) throw std::runtime_error("GPU failed during scanout: " + fault_message());
        if (p.totals.core_cycles - start > p.configuration.operation_timeout_cycles)
            throw std::runtime_error("scanout observation timed out");
    }
    p.totals.last_scanout_observation_cycles = p.totals.core_cycles - start; observe_memory();
}
int SystemModel::front_id() const { return impl_->scanout.front_id(); }
int SystemModel::back_id() const { return 1 - front_id(); }
bool SystemModel::presentation_pending() const { return impl_->sealed || impl_->driver_present_pending; }
bool SystemModel::color_valid(int id) const { check_buffer(id); return impl_->valid[id]; }
bool SystemModel::depth_valid() const { return impl_->valid_depth; }
const std::vector<std::uint16_t>& SystemModel::color(int id) const { check_buffer(id); return impl_->observed_color[id]; }
const std::vector<std::uint16_t>& SystemModel::front_color() const { return color(front_id()); }
const std::vector<std::uint32_t>& SystemModel::depth() const { return impl_->observed_depth; }
const std::vector<std::uint16_t>& SystemModel::display_color() const { return impl_->scanout.completed_frame(); }
const Statistics& SystemModel::statistics() const { return impl_->counts; }
const SystemStatistics& SystemModel::system_statistics() const { return impl_->totals; }
const SdramStatistics& SystemModel::memory_statistics() const { return impl_->memory.statistics(); }
const HostLinkStatistics& SystemModel::host_statistics() const { return impl_->host.statistics(); }
const ScanoutStatistics& SystemModel::scanout_statistics() const { return impl_->scanout.statistics(); }
const SystemConfig& SystemModel::config() const { return impl_->configuration; }

} // namespace nano_gpu::hw
