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

// One shared bounded record/response staging buffer. Each response is drained
// at one 32-bit word per edge; a newly completed SDR response is never sampled
// until the next system edge. It is not an untimed memory-read shortcut.
struct Reader {
    enum class State { Idle, Request, Wait, Drain, Done } state = State::Idle;
    std::array<std::uint8_t, 512> bytes{};
    MemoryResponse response{};
    MemoryClient client = MemoryClient::Command;
    std::uint32_t address = 0, burst_address = 0;
    unsigned length = 0, copied = 0, word = 0;
    void start(std::uint32_t base, unsigned count, MemoryClient port) {
        if (count == 0 || count > bytes.size() ||
            static_cast<std::uint64_t>(base) + count > memory_bytes)
            throw std::out_of_range("bounded system read outside SDRAM");
        address = base; length = count; copied = 0; client = port; state = State::Request;
    }
    bool done() const { return state == State::Done; }
    void tick(SdramController& memory) {
        switch (state) {
        case State::Idle: case State::Done: break;
        case State::Request: {
            burst_address = (address + copied) & ~3u;
            MemoryRequest request;
            request.address = burst_address;
            request.words = std::min<unsigned>(memory_burst_words,
                (address + length - burst_address + 3u) / 4u);
            if (memory.submit(client, request)) state = State::Wait;
            break;
        }
        case State::Wait:
            if (memory.response_ready(client)) {
                response = memory.take_response(client); word = 0; state = State::Drain;
            }
            break;
        case State::Drain: {
            const auto value = response.data[word];
            for (unsigned byte = 0; byte < 4; ++byte) {
                const auto source = burst_address + 4 * word + byte;
                if (source >= address && source < address + length) {
                    bytes[source - address] = static_cast<std::uint8_t>(value >> (8 * byte));
                    ++copied;
                }
            }
            if (++word == response.words) state = copied == length ? State::Done : State::Request;
            break;
        }
        }
    }
    std::vector<std::uint8_t> view() const { return {bytes.begin(), bytes.begin() + length}; }
};

// One-byte source port fills a bounded 16-word write staging buffer. Payload
// sources can stall until a synchronous packet-RAM read result is established.
struct Writer {
    enum class State { Idle, Fill, Request, Wait, Done } state = State::Idle;
    std::function<std::optional<std::uint8_t>(unsigned)> source;
    MemoryClient client = MemoryClient::Host;
    MemoryRequest request{};
    std::uint32_t address = 0;
    unsigned length = 0, cursor = 0, fill = 0, chunk = 0, prefix = 0;
    void prepare() {
        request = {}; request.write = true;
        request.address = (address + cursor) & ~3u;
        prefix = (address + cursor) & 3u;
        chunk = std::min<unsigned>(length - cursor, memory_burst_words * 4 - prefix);
        request.words = (prefix + chunk + 3) / 4;
        fill = 0; state = State::Fill;
    }
    void start(std::uint32_t base, unsigned count, MemoryClient port,
               std::function<std::optional<std::uint8_t>(unsigned)> input) {
        if (count == 0 || static_cast<std::uint64_t>(base) + count > memory_bytes)
            throw std::out_of_range("system write outside SDRAM");
        address = base; length = count; client = port; source = std::move(input); cursor = 0; prepare();
    }
    bool done() const { return state == State::Done; }
    void tick(SdramController& memory) {
        switch (state) {
        case State::Idle: case State::Done: break;
        case State::Fill: {
            const auto value = source(cursor + fill);
            if (!value) break;
            const auto index = prefix + fill;
            request.data[index >> 2] |= static_cast<std::uint32_t>(*value) << (8 * (index & 3u));
            request.masks[index >> 2] |= static_cast<std::uint8_t>(1u << (index & 3u));
            if (++fill == chunk) state = State::Request;
            break;
        }
        case State::Request:
            if (memory.submit(client, request)) state = State::Wait;
            break;
        case State::Wait:
            if (memory.response_ready(client)) {
                (void)memory.take_response(client); cursor += chunk;
                if (cursor == length) state = State::Done; else prepare();
            }
            break;
        }
    }
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
    enum class State {
        Idle, Capture, Upload, PassDescriptor, TileEntry, LoadRow,
        LoadColor, LoadDepthLow, LoadDepthHigh, LoadLocal,
        Reference, Record, Draw, StoreLocal, StorePrepare, StoreColor, StoreDepthLow,
        StoreDepthHigh, Copy, Present, Readback, ReplyBuild, ReplyRead, Reply, Fault, Abort
    } state = State::Idle;
    SystemConfig configuration;
    SdramController memory;
    HostLink host;
    Scanout scanout;
    OpaqueTile tile;
    SystemCopy copier;
    Sdp1kx16 reply_bank;
    Reader reader;
    Writer writer;
    Statistics counts{};
    SystemStatistics totals{};
    const HostCommand* command = nullptr; // Lease the front packet bank until retirement.
    std::array<std::uint8_t, 64> captured{};
    unsigned capture_size = 0, capture_index = 0;
    bool packet_read_pending = false;
    unsigned packet_read_offset = 0;
    // Bounded serializer shadow of response fields/Reader registers, not a
    // bypass around device RAM. ReplyBuild writes one byte per edge; only the
    // registered reply_bank read port may feed the host TX producer.
    std::vector<std::uint8_t> reply;
    unsigned reply_index = 0;
    std::array<std::uint8_t, 64> row_bytes{};
    std::array<std::uint16_t, 32> row_colors{};
    std::array<std::uint32_t, 32> row_depth{};
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

    explicit Impl(SystemConfig config)
        : configuration(std::move(config)), memory(configuration.memory), host(configuration.host),
          scanout(configuration.scanout), observed_depth(screen_width * screen_height, depth_max) {
        if (configuration.operation_timeout_cycles == 0)
            throw std::invalid_argument("system operation timeout must be positive");
        for (auto& color : observed_color) color.resize(screen_width * screen_height);
        host.set_capabilities(capabilities());
        host.set_status(status());
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
        put(bytes, 16, totals.core_cycles, 8); put(bytes, 24, scanout.statistics().underflows, 8);
        put(bytes, 32, static_cast<std::uint32_t>(error));
        put(bytes, 36, totals.completed_present_token); put(bytes, 40, memory.statistics().refresh_late_cycles, 8);
        put(bytes, 48, memory.statistics().uninitialized_read_words, 8);
        bytes[56] = static_cast<std::uint8_t>(host.fault());
        return bytes;
    }
    std::array<std::uint8_t, 64> capabilities() const {
        std::array<std::uint8_t, 64> bytes{};
        bytes[0] = 0; bytes[1] = 1; bytes[2] = 8; bytes[3] = 32;
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
        if (data.size() > 768) throw std::out_of_range("system reply exceeds bounded staging");
        reply.assign(28 + data.size(), 0);
        reply[0] = 'N'; reply[1] = 'G'; reply[2] = 'P'; reply[3] = 'U'; reply[5] = 1;
        write_u16(reply, 6, static_cast<std::uint16_t>(command->opcode) | 0x8000u);
        write_u32(reply, 8, static_cast<std::uint32_t>(12 + data.size()));
        write_u32(reply, 12, command->sequence); write_u32(reply, 16, totals.epoch);
        write_u32(reply, 20, 0); write_u32(reply, 24, token);
        std::copy(data.begin(), data.end(), reply.begin() + 28);
        reply_index = 0; state = State::ReplyBuild;
    }
    void fail(SystemFault code, const std::string& reason) {
        if (error == SystemFault::None) { error = code; message = reason; }
        if (pass_active) { valid[pass.target] = false; valid_depth = false; }
        if (copy_active) valid[copy.target] = false;
        sealed = driver_present_pending = false;
        state = State::Fault;
    }
    bool drain_issued() {
        if (tile.external_memory_pending()) tile.tick();
        if (copier.memory_pending()) copier.tick(memory, tile);
        bool drained = !tile.external_memory_pending() && !copier.memory_pending();
        for (unsigned port = 1; port < memory_client_count; ++port) {
            const auto client = static_cast<MemoryClient>(port);
            // The tile unit owns its own texture response until it consumes it.
            if (client != MemoryClient::Texture && client != MemoryClient::Copy && memory.response_ready(client))
                (void)memory.take_response(client);
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
    void acquire() {
        command = &host.front_command(); captured.fill(0); capture_index = 0; packet_read_pending = false;
        unsigned exact = 0;
        switch (command->opcode) {
        case Opcode::GetCaps: case Opcode::GetStatus: exact = 0; break;
        case Opcode::WriteHeap:
            if (command->payload_size < 8 || command->payload_size > maximum_packet_payload)
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
            if (length != command->payload_size - 8 || length == 0)
                throw std::invalid_argument("WRITE_HEAP data length");
            validate_heap_range(address, length, 1);
            writer.start(address, length, MemoryClient::Host,
                [this](unsigned index) { return packet_byte(8 + index); });
            state = State::Upload; break;
        }
        case Opcode::SubmitPass:
            validate_heap_range(get(captured, 0), pass_descriptor_bytes);
            reader.start(get(captured, 0), pass_descriptor_bytes, MemoryClient::Command);
            pass_start = totals.core_cycles; state = State::PassDescriptor; break;
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
            reader.start(address, length, MemoryClient::Command); state = State::Readback; break;
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
    void begin_row_load() {
        beat = 0;
        if (pass.color_load == LoadOp::Load) {
            reader.start(color_buffer_base(pass.target) + global_pixel() * 2, 64, MemoryClient::Tile);
            state = State::LoadColor;
        } else {
            row_colors.fill(pass.clear_color); begin_depth_load();
        }
    }
    void begin_depth_load() {
        if (pass.depth_load == LoadOp::Load) {
            reader.start(depth_buffer_base + global_pixel() * 4, 64, MemoryClient::Tile);
            state = State::LoadDepthLow;
        } else { row_depth.fill(pass.clear_depth); beat = 0; state = State::LoadLocal; }
    }
    void prepare_store(bool depth, unsigned half = 0) {
        for (unsigned index = 0; index < 64; ++index) {
            if (depth) row_bytes[index] = static_cast<std::uint8_t>(row_depth[half * 16 + index / 4] >> (8 * (index & 3u)));
            else row_bytes[index] = static_cast<std::uint8_t>(row_colors[index / 2] >> (8 * (index & 1u)));
        }
        const auto address = depth ? depth_buffer_base + global_pixel() * 4 + half * 64 :
                                    color_buffer_base(pass.target) + global_pixel() * 2;
        writer.start(address, 64, MemoryClient::Tile,
            [this](unsigned index) -> std::optional<std::uint8_t> { return row_bytes[index]; });
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
        valid[pass.target] = valid_depth = true; pass_active = false;
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
                for (unsigned i = 0; i < 32; ++i)
                    row_colors[i] = static_cast<std::uint16_t>(reader.bytes[2*i] | (reader.bytes[2*i+1] << 8));
                begin_depth_load();
            }
            break;
        }
        case State::LoadDepthLow: case State::LoadDepthHigh: {
            const bool high = state == State::LoadDepthHigh;
            const bool completed = reader.done();
            reader.tick(memory);
            if (completed) {
                for (unsigned i = 0; i < 16; ++i) {
                    row_depth[(high ? 16 : 0) + i] = 0;
                    for (unsigned byte = 0; byte < 4; ++byte)
                        row_depth[(high ? 16 : 0) + i] |= std::uint32_t{reader.bytes[4*i+byte]} << (8*byte);
                }
                if (!high) {
                    reader.start(depth_buffer_base + global_pixel() * 4 + 64, 64, MemoryClient::Tile);
                    state = State::LoadDepthHigh;
                } else { beat = 0; state = State::LoadLocal; }
            }
            break;
        }
        case State::LoadLocal:
            tile.load_word(static_cast<std::uint16_t>(row * tile_size + beat), row_colors[beat], row_depth[beat]);
            if (++beat == 32) {
                if (++row == 32) {
                    reference_index = 0;
                    if (bin.count != 0) fetch_reference();
                    else { row = beat = 0; state = State::StoreLocal; }
                } else state = State::LoadRow;
            }
            break;
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
                const auto decoded = decode_primitive(reader.view());
                totals.fetched_record_bytes += primitive_record_bytes;
                tile.begin_external(decoded.primitive, origin(), decoded.texture, memory); state = State::Draw;
            }
            break;
        }
        case State::Draw:
            tile.tick();
            if (!tile.busy()) {
                if (++reference_index < bin.count) fetch_reference();
                else { row = beat = 0; state = State::StoreLocal; }
            }
            break;
        case State::StoreLocal:
            if (beat != 0) {
                const auto value = tile.read_word_result(); row_colors[beat-1] = value.color;
                row_depth[beat-1] = value.depth;
            }
            if (beat < 32) tile.read_word_request(static_cast<std::uint16_t>(row * 32 + beat));
            else tile.idle_edge();
            if (++beat == 33) state = State::StorePrepare;
            break;
        case State::StorePrepare:
            prepare_store(false); state = State::StoreColor;
            break;
        case State::StoreColor:
            writer.tick(memory);
            if (writer.done()) { prepare_store(true); state = State::StoreDepthLow; }
            break;
        case State::StoreDepthLow:
            writer.tick(memory);
            if (writer.done()) { prepare_store(true, 1); state = State::StoreDepthHigh; }
            break;
        case State::StoreDepthHigh:
            writer.tick(memory);
            if (writer.done()) {
                if (++row != 32) { beat = 0; state = State::StoreLocal; }
                else {
                    ++counts.tiles_processed;
                    if (++tile_index != tile_count) fetch_entry(); else finish_pass();
                }
            }
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
                append(result, reader.view()); make_reply(get(captured, 8), result);
            }
            break;
        }
        case State::ReplyBuild:
            reply_bank.tick({true, static_cast<std::uint16_t>(reply_index), reply[reply_index], false, 0, false});
            if (++reply_index == reply.size()) { reply_index = 0; state = State::ReplyRead; }
            break;
        case State::ReplyRead:
            reply_bank.tick({false, 0, 0, true, static_cast<std::uint16_t>(reply_index), false});
            state = State::Reply;
            break;
        case State::Reply:
            if (host.push_response_byte(static_cast<std::uint8_t>(reply_bank.output()))) {
                if (++reply_index == reply.size()) finish(); else state = State::ReplyRead;
            }
            reply_bank.tick({});
            break;
        case State::Fault: (void)drain_issued(); break;
        case State::Abort:
            if (drain_issued()) {
                if (totals.epoch == std::numeric_limits<std::uint32_t>::max())
                    throw std::overflow_error("system epoch exhausted; start a fresh power-on session");
                copier.reset(); tile.reset(); reader.state = Reader::State::Idle; writer.state = Writer::State::Idle;
                host.recover(++totals.epoch); next_sequence = 1;
                totals.completed_sequence = totals.completed_fence_token = totals.completed_present_token = 0;
                command = nullptr; packet_read_pending = false; pass_active = copy_active = false;
                sealed = driver_present_pending = false; error = SystemFault::None; message.clear(); state = State::Idle;
                if (!initialized) { boot_address = 0; boot_wait = false; }
            }
            break;
        }
    }
    void boot() {
        if (!memory.initialized()) return;
        if (boot_wait) {
            if (!memory.response_ready(MemoryClient::Tile)) return;
            (void)memory.take_response(MemoryClient::Tile); boot_address += 4 * memory_burst_words; boot_wait = false;
            if (boot_address == heap_base) {
                initialized = true; valid = {{true, true}}; valid_depth = true;
                totals.boot_cycles = totals.core_cycles; scanout.set_memory_ready(true); return;
            }
        }
        MemoryRequest request; request.write = true; request.address = boot_address; request.words = memory_burst_words;
        for (unsigned word = 0; word < request.words; ++word) {
            request.data[word] = boot_address >= depth_buffer_base ? depth_max : 0;
            request.masks[word] = 0xf;
        }
        if (memory.submit(MemoryClient::Tile, request)) boot_wait = true;
    }
    HostBusResult tick(const HostPins& pins) {
        // The host samples established device registers, never state newly
        // produced by the controller/scanout on this same common edge.
        if (pins.read && pins.register_address == 1) host.set_status(status());
        ++totals.core_cycles;
        if (host.abort_requested() && state != State::Abort) {
            if (pass_active) { valid[pass.target] = false; valid_depth = false; }
            if (copy_active) valid[copy.target] = false;
            sealed = driver_present_pending = false; state = State::Abort; ++totals.aborts;
        }
        if (host.faulted() && state != State::Abort && error == SystemFault::None)
            fail(SystemFault::Transport, "parallel link or packet decoder fault");
        try {
            if (state == State::Abort || state == State::Fault) execute();
            else if (!initialized) boot();
            else execute();
        } catch (const std::exception& exception) { fail(SystemFault::Record, exception.what()); }
        // Consumers see only responses established by a previous memory edge.
        scanout.tick(memory);
        const auto result = host.tick(pins);
        memory.tick();
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
        if (epoch == std::numeric_limits<std::uint32_t>::max()) throw std::overflow_error("system epoch exhausted");
        impl_ = std::make_unique<Impl>(config); impl_->host.recover(epoch + 1); impl_->totals.epoch = epoch + 1;
    }
    return impl_->tick(pins);
}
void SystemModel::advance(std::uint64_t cycles) { while (cycles-- != 0) tick(); }
void SystemModel::reset() {
    auto& p = *impl_;
    if (p.totals.epoch == std::numeric_limits<std::uint32_t>::max()) throw std::overflow_error("system epoch exhausted");
    const auto epoch = p.totals.epoch + 1; const auto config = p.configuration;
    impl_ = std::make_unique<Impl>(config); impl_->host.recover(epoch); impl_->totals.epoch = epoch;
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
                if (read_u32(response, 0) != 0x5550474eu || response[4] != 0 || response[5] != 1 ||
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
    const auto program = compile_pass(pass, p.next_sequence);
    p.next_sequence = program.next_sequence;
    const auto start = p.totals.core_cycles;
    send_bytes(program.bytes); const auto response = read_reply(Opcode::Fence, program.fence_sequence);
    if (read_u32(response, 24) != program.fence_token) throw std::runtime_error("wrong fence token");
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
        const auto size = std::min<std::size_t>(maximum_packet_payload - 8, data.size() - offset);
        auto payload = scalar(heap_base + static_cast<std::uint32_t>(offset)); append(payload, scalar(static_cast<std::uint32_t>(size)));
        payload.insert(payload.end(), data.begin() + offset, data.begin() + offset + size);
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
