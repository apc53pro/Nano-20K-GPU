#include "nano_gpu/hw/protocol.hpp"

#include "nano_gpu/hw/walker2d.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>

namespace nano_gpu::hw {
namespace {

void require_size(const std::vector<std::uint8_t>& bytes, std::size_t size) {
    if (bytes.size() != size) throw std::invalid_argument("incorrect packed structure size");
}
void require_field(const std::vector<std::uint8_t>& bytes, std::size_t offset, std::size_t size) {
    if (offset > bytes.size() || size > bytes.size() - offset)
        throw std::out_of_range("packed field exceeds byte buffer");
}
void require_zero(const std::vector<std::uint8_t>& bytes, std::size_t begin, std::size_t end) {
    if (std::any_of(bytes.begin() + static_cast<std::ptrdiff_t>(begin),
                    bytes.begin() + static_cast<std::ptrdiff_t>(end),
                    [](std::uint8_t value) { return value != 0; }))
        throw std::invalid_argument("nonzero reserved packed field");
}
bool known_opcode(Opcode opcode) {
    const auto code = static_cast<unsigned>(opcode);
    return code >= static_cast<unsigned>(Opcode::GetCaps) && code <= static_cast<unsigned>(Opcode::ReadMemory);
}
bool power_of_two(std::uint32_t value) { return value != 0 && (value & (value - 1)) == 0; }
void require_texture(const TextureDescriptor& texture) {
    texture_extent(texture);
    if (!power_of_two(texture.width) || !power_of_two(texture.height))
        throw std::invalid_argument("bound texture dimensions must be powers of two");
}
void put_bounds(std::vector<std::uint8_t>& bytes, std::size_t at, Bounds bounds) {
    write_i32(bytes, at, bounds.x0); write_i32(bytes, at + 4, bounds.y0);
    write_i32(bytes, at + 8, bounds.x1); write_i32(bytes, at + 12, bounds.y1);
}
Bounds get_bounds(const std::vector<std::uint8_t>& bytes, std::size_t at) {
    return {read_i32(bytes, at), read_i32(bytes, at + 4),
            read_i32(bytes, at + 8), read_i32(bytes, at + 12)};
}
void put_texture(std::vector<std::uint8_t>& bytes, std::size_t at, const TextureDescriptor& texture) {
    write_u32(bytes, at, texture.base); write_u32(bytes, at + 4, texture.width);
    write_u32(bytes, at + 8, texture.height); write_u32(bytes, at + 12, texture.pitch_bytes);
}
TextureDescriptor get_texture(const std::vector<std::uint8_t>& bytes, std::size_t at) {
    return {read_u32(bytes, at), read_u32(bytes, at + 4),
            read_u32(bytes, at + 8), read_u32(bytes, at + 12)};
}
void validate_state_fields(const RenderState& state) {
    if (static_cast<unsigned>(state.shade) > static_cast<unsigned>(ShadeMode::TextureGouraudModulate) ||
        static_cast<unsigned>(state.depth_func) > static_cast<unsigned>(DepthFunc::Always) ||
        static_cast<unsigned>(state.address_u) > static_cast<unsigned>(AddressMode::Repeat) ||
        static_cast<unsigned>(state.address_v) > static_cast<unsigned>(AddressMode::Repeat))
        throw std::invalid_argument("unsupported packed render state");
}
bool needs_texture(const Primitive& primitive) {
    return std::visit([](const auto& item) {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, Sprite>) return true;
        else if constexpr (std::is_same_v<T, Triangle>)
            return static_cast<unsigned>(item.state.shade) >= static_cast<unsigned>(ShadeMode::TextureReplace) &&
                   (item.state.color_write || item.state.color_key.has_value());
        else return false;
    }, primitive);
}
void validate_primitive(const Primitive& primitive) {
    std::visit([&](const auto& item) {
        using T = std::decay_t<decltype(item)>;
        validate_state_fields(item.state);
        if constexpr (std::is_same_v<T, Triangle>) validate_triangle(item);
        else {
            Walker2D validator;
            validator.begin(primitive, {0, 0});
        }
    }, primitive);
}
void validate_pass(const PassDescriptor& descriptor) {
    if (descriptor.target < 0 || descriptor.target > 1 || descriptor.clear_depth > depth_max ||
        (descriptor.color_load != LoadOp::Load && descriptor.color_load != LoadOp::Clear) ||
        (descriptor.depth_load != LoadOp::Load && descriptor.depth_load != LoadOp::Clear))
        throw std::invalid_argument("invalid packed pass state");
    validate_heap_range(descriptor.tile_table_address, tile_count * tile_entry_bytes);
}

class HeapBuilder {
public:
    std::uint32_t allocate(std::uint64_t size) {
        if (size == 0 || (size & 3u) != 0 || size > memory_bytes - heap_base ||
            bytes.size() > memory_bytes - heap_base - size)
            throw std::invalid_argument("compiled pass exceeds the shared GPU heap");
        const auto address = heap_base + static_cast<std::uint32_t>(bytes.size());
        bytes.resize(bytes.size() + static_cast<std::size_t>(size), 0);
        return address;
    }
    void put(std::uint32_t address, const std::vector<std::uint8_t>& source) {
        const auto offset = static_cast<std::size_t>(address - heap_base);
        require_field(bytes, offset, source.size());
        std::copy(source.begin(), source.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset));
    }
    std::vector<std::uint8_t> bytes;
};

} // namespace

std::uint16_t read_u16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    require_field(bytes, offset, 2);
    return static_cast<std::uint16_t>(bytes[offset] | (static_cast<std::uint16_t>(bytes[offset + 1]) << 8));
}
std::uint32_t read_u32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    require_field(bytes, offset, 4);
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= static_cast<std::uint32_t>(bytes[offset + i]) << (8 * i);
    return value;
}
std::int32_t read_i32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    const auto value = read_u32(bytes, offset);
    if (value <= static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()))
        return static_cast<std::int32_t>(value);
    return -1 - static_cast<std::int32_t>(~value);
}
std::int64_t read_i64(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    require_field(bytes, offset, 8);
    const auto value = static_cast<std::uint64_t>(read_u32(bytes, offset)) |
                       (static_cast<std::uint64_t>(read_u32(bytes, offset + 4)) << 32);
    if (value <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return static_cast<std::int64_t>(value);
    return -1 - static_cast<std::int64_t>(~value);
}
void write_u16(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint16_t value) {
    require_field(bytes, offset, 2);
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}
void write_u32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    require_field(bytes, offset, 4);
    for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
}
void write_i32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::int32_t value) {
    write_u32(bytes, offset, static_cast<std::uint32_t>(value));
}
void write_i64(std::vector<std::uint8_t>& bytes, std::size_t offset, std::int64_t value) {
    const auto bits = static_cast<std::uint64_t>(value);
    write_u32(bytes, offset, static_cast<std::uint32_t>(bits));
    write_u32(bytes, offset + 4, static_cast<std::uint32_t>(bits >> 32));
}

void validate_heap_range(std::uint32_t address, std::uint64_t length, unsigned alignment) {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0 || address % alignment != 0 ||
        address < heap_base || address >= memory_bytes || length == 0 || length > memory_bytes ||
        static_cast<std::uint64_t>(address) + length > memory_bytes)
        throw std::invalid_argument("packed address is outside an aligned live heap extent");
}
std::uint32_t texture_extent(const TextureDescriptor& texture) {
    if (texture.width < 1 || texture.width > 1024 || texture.height < 1 || texture.height > 1024 ||
        texture.pitch_bytes < 2u * texture.width || (texture.pitch_bytes & 3u) != 0)
        throw std::invalid_argument("invalid packed image dimensions or pitch");
    const auto extent = static_cast<std::uint64_t>(texture.pitch_bytes) * texture.height;
    validate_heap_range(texture.base, extent);
    return static_cast<std::uint32_t>(extent);
}

std::vector<std::uint8_t> encode_packet(Opcode opcode, std::uint32_t sequence,
                                       const std::vector<std::uint8_t>& payload) {
    if (!known_opcode(opcode) || sequence == 0 || payload.size() > maximum_packet_payload)
        throw std::invalid_argument("invalid provisional NGPU packet");
    std::vector<std::uint8_t> result(packet_header_bytes + payload.size(), 0);
    result[0] = 'N'; result[1] = 'G'; result[2] = 'P'; result[3] = 'U';
    result[4] = 0; result[5] = 1;
    write_u16(result, 6, static_cast<std::uint16_t>(opcode));
    write_u32(result, 8, static_cast<std::uint32_t>(payload.size()));
    write_u32(result, 12, sequence);
    std::copy(payload.begin(), payload.end(), result.begin() + packet_header_bytes);
    return result;
}

std::vector<std::uint8_t> encode_primitive(const Primitive& primitive,
                                         std::optional<TextureDescriptor> texture) {
    validate_primitive(primitive);
    if (needs_texture(primitive) && !texture)
        throw std::invalid_argument("packed textured primitive needs a GPU texture descriptor");
    if (texture) require_texture(*texture);
    std::vector<std::uint8_t> result(primitive_record_bytes, 0);
    std::visit([&](const auto& item) {
        using T = std::decay_t<decltype(item)>;
        const auto& state = item.state;
        const auto flags = (state.color_write ? 1u : 0u) | (state.depth_test ? 2u : 0u) |
                           (state.depth_write ? 4u : 0u) | (state.color_key ? 8u : 0u);
        write_u32(result, 0, static_cast<std::uint32_t>(primitive.index()) | 0x100u | (flags << 16));
        write_u32(result, 4, static_cast<std::uint32_t>(state.shade) |
            (static_cast<std::uint32_t>(state.depth_func) << 8) |
            (state.address_u == AddressMode::Repeat ? 1u << 16 : 0u) |
            (state.address_v == AddressMode::Repeat ? 1u << 17 : 0u) |
            (texture ? 1u << 18 : 0u));
        write_u32(result, 8, state.color.r | (static_cast<std::uint32_t>(state.color.g) << 8) |
                               (static_cast<std::uint32_t>(state.color.b) << 16));
        write_u32(result, 12, state.color_key.value_or(0));
        put_bounds(result, 16, state.scissor);
        if (texture) {
            if (state.texture && (texture->width != static_cast<unsigned>(state.texture->width()) ||
                texture->height != static_cast<unsigned>(state.texture->height()) ||
                texture->pitch_bytes != static_cast<unsigned>(state.texture->pitch_bytes())))
                throw std::invalid_argument("packed texture descriptor differs from bound image");
            put_texture(result, 48, *texture);
        }
        if constexpr (std::is_same_v<T, Triangle>) {
            put_bounds(result, 32, item.bounds);
            for (unsigned i = 0; i < 3; ++i) {
                write_i32(result, 80 + 8 * i, item.snapped[i].x);
                write_i32(result, 84 + 8 * i, item.snapped[i].y);
                write_i32(result, 104 + 16 * i, item.edges[i].a);
                write_i32(result, 108 + 16 * i, item.edges[i].b);
                write_i32(result, 112 + 16 * i, item.edges[i].c);
                write_u32(result, 116 + 16 * i, item.edges[i].inclusive ? 1u : 0u);
            }
            const std::array<Plane, 7> planes{item.z, item.r, item.g, item.b, item.q, item.u, item.v};
            for (unsigned i = 0; i < planes.size(); ++i) {
                write_i64(result, 152 + 24 * i, planes[i].seed);
                write_i64(result, 160 + 24 * i, planes[i].step_x);
                write_i64(result, 168 + 24 * i, planes[i].step_y);
            }
        } else {
            write_u32(result, 64, item.depth);
            if constexpr (std::is_same_v<T, Line>) {
                write_i32(result, 68, item.a.x); write_i32(result, 72, item.a.y);
                write_i32(result, 80, item.b.x); write_i32(result, 84, item.b.y);
            } else {
                put_bounds(result, 32, item.bounds);
                if constexpr (std::is_same_v<T, Sprite>) {
                    write_i32(result, 68, item.source_x); write_i32(result, 72, item.source_y);
                }
            }
        }
    }, primitive);
    return result;
}

DecodedPrimitive decode_primitive(const std::vector<std::uint8_t>& bytes) {
    require_size(bytes, primitive_record_bytes);
    const auto kind_flags = read_u32(bytes, 0);
    const unsigned kind = kind_flags & 0xffu;
    if (kind > 3 || ((kind_flags >> 8) & 0xffu) != 1 || (kind_flags & 0xfff00000u) != 0)
        throw std::invalid_argument("unsupported primitive kind, version, or flags");
    const auto modes = read_u32(bytes, 4);
    if ((modes & 0xfff80000u) != 0 || (modes & 0xffu) > 4 || ((modes >> 8) & 0xffu) > 2 ||
        (read_u32(bytes, 8) & 0xff000000u) != 0 || (read_u32(bytes, 12) & 0xffff0000u) != 0)
        throw std::invalid_argument("invalid packed primitive state or reserved bits");
    RenderState state;
    state.color_write = (kind_flags & (1u << 16)) != 0;
    state.depth_test = (kind_flags & (1u << 17)) != 0;
    state.depth_write = (kind_flags & (1u << 18)) != 0;
    if ((kind_flags & (1u << 19)) != 0) state.color_key = read_u16(bytes, 12);
    else if (read_u32(bytes, 12) != 0) throw std::invalid_argument("disabled color key must be zero");
    state.shade = static_cast<ShadeMode>(modes & 0xffu);
    state.depth_func = static_cast<DepthFunc>((modes >> 8) & 0xffu);
    state.address_u = (modes & (1u << 16)) != 0 ? AddressMode::Repeat : AddressMode::Clamp;
    state.address_v = (modes & (1u << 17)) != 0 ? AddressMode::Repeat : AddressMode::Clamp;
    const auto rgb = read_u32(bytes, 8);
    state.color = {static_cast<std::uint8_t>(rgb), static_cast<std::uint8_t>(rgb >> 8),
                   static_cast<std::uint8_t>(rgb >> 16)};
    state.scissor = get_bounds(bytes, 16);
    DecodedPrimitive result;
    if ((modes & (1u << 18)) != 0) {
        result.texture = get_texture(bytes, 48);
        require_texture(*result.texture);
        state.texture = Image::descriptor(static_cast<int>(result.texture->width),
            static_cast<int>(result.texture->height), static_cast<int>(result.texture->pitch_bytes));
    } else require_zero(bytes, 48, 64);
    require_zero(bytes, 76, 80);
    if (kind == 0) {
        require_zero(bytes, 64, 76);
        Triangle triangle;
        triangle.state = state; triangle.bounds = get_bounds(bytes, 32);
        for (unsigned i = 0; i < 3; ++i) {
            triangle.snapped[i] = {read_i32(bytes, 80 + 8 * i), read_i32(bytes, 84 + 8 * i)};
            const auto inclusive = read_u32(bytes, 116 + 16 * i);
            if (inclusive > 1) throw std::invalid_argument("invalid edge inclusion bit");
            triangle.edges[i] = {read_i32(bytes, 104 + 16 * i), read_i32(bytes, 108 + 16 * i),
                                 read_i32(bytes, 112 + 16 * i), inclusive != 0};
        }
        const std::array<Plane*, 7> planes{&triangle.z, &triangle.r, &triangle.g, &triangle.b,
                                          &triangle.q, &triangle.u, &triangle.v};
        for (unsigned i = 0; i < planes.size(); ++i)
            *planes[i] = {read_i64(bytes, 152 + 24 * i), read_i64(bytes, 160 + 24 * i),
                           read_i64(bytes, 168 + 24 * i)};
        result.primitive = triangle;
    } else {
        const auto depth = read_u32(bytes, 64);
        if (depth > depth_max) throw std::invalid_argument("packed constant depth exceeds D24");
        if (kind == 1) {
            require_zero(bytes, 68, 76); require_zero(bytes, 80, primitive_record_bytes);
            result.primitive = Rect{get_bounds(bytes, 32), state, depth};
        } else if (kind == 2) {
            require_zero(bytes, 32, 48); require_zero(bytes, 88, primitive_record_bytes);
            result.primitive = Line{{read_i32(bytes, 68), read_i32(bytes, 72)},
                                     {read_i32(bytes, 80), read_i32(bytes, 84)}, state, depth};
        } else {
            require_zero(bytes, 80, primitive_record_bytes);
            result.primitive = Sprite{get_bounds(bytes, 32), read_i32(bytes, 68), read_i32(bytes, 72), state, depth};
        }
    }
    if (needs_texture(result.primitive) && !result.texture)
        throw std::invalid_argument("packed primitive is missing its texture descriptor");
    validate_primitive(result.primitive);
    return result;
}

std::vector<std::uint8_t> encode_pass_descriptor(const PassDescriptor& descriptor) {
    validate_pass(descriptor);
    std::vector<std::uint8_t> result(pass_descriptor_bytes, 0);
    write_u32(result, 0, pass_descriptor_magic);
    write_u32(result, 4, static_cast<std::uint32_t>(descriptor.target));
    write_u32(result, 8, (descriptor.color_load == LoadOp::Clear ? 1u : 0u) |
                          (descriptor.depth_load == LoadOp::Clear ? 2u : 0u));
    write_u32(result, 12, descriptor.clear_color); write_u32(result, 16, descriptor.clear_depth);
    write_u32(result, 20, descriptor.tile_table_address); write_u32(result, 24, tile_count);
    return result;
}
PassDescriptor decode_pass_descriptor(const std::vector<std::uint8_t>& bytes) {
    require_size(bytes, pass_descriptor_bytes);
    if (read_u32(bytes, 0) != pass_descriptor_magic || read_u32(bytes, 4) > 1 ||
        (read_u32(bytes, 8) & ~3u) != 0 || read_u32(bytes, 12) > 0xffffu ||
        read_u32(bytes, 16) > depth_max || read_u32(bytes, 24) != tile_count || read_u32(bytes, 28) != 0)
        throw std::invalid_argument("invalid pass descriptor fields");
    PassDescriptor result;
    result.target = static_cast<int>(read_u32(bytes, 4));
    result.color_load = (read_u32(bytes, 8) & 1u) ? LoadOp::Clear : LoadOp::Load;
    result.depth_load = (read_u32(bytes, 8) & 2u) ? LoadOp::Clear : LoadOp::Load;
    result.clear_color = read_u16(bytes, 12); result.clear_depth = read_u32(bytes, 16);
    result.tile_table_address = read_u32(bytes, 20);
    validate_pass(result);
    return result;
}
TileEntry decode_tile_entry(const std::vector<std::uint8_t>& bytes) {
    require_size(bytes, tile_entry_bytes);
    TileEntry result{read_u32(bytes, 0), read_u32(bytes, 4)};
    if (result.count > maximum_pass_references || (result.count == 0 && result.address != 0))
        throw std::invalid_argument("invalid tile list count or empty-list address");
    if (result.count != 0) validate_heap_range(result.address, 4ull * result.count);
    return result;
}

std::vector<std::uint8_t> encode_copy_command(const CopyCommand& command) {
    if (command.target < 0 || command.target > 1 || command.source < 0 || command.source > 1 ||
        command.width < 0 || command.height < 0 || (command.image_source && command.source != 0))
        throw std::invalid_argument("invalid copy command state");
    if (command.image_source) texture_extent(command.image);
    else if (command.image != TextureDescriptor{}) throw std::invalid_argument("framebuffer copy has image metadata");
    std::vector<std::uint8_t> result(copy_payload_bytes, 0);
    write_u32(result, 0, command.image_source ? 1u : 0u);
    write_u32(result, 4, static_cast<std::uint32_t>(command.source));
    write_u32(result, 8, static_cast<std::uint32_t>(command.target));
    write_i32(result, 12, command.source_origin.x); write_i32(result, 16, command.source_origin.y);
    write_i32(result, 20, command.destination_origin.x); write_i32(result, 24, command.destination_origin.y);
    write_i32(result, 28, command.width); write_i32(result, 32, command.height);
    if (command.image_source) put_texture(result, 36, command.image);
    return result;
}
CopyCommand decode_copy_command(const std::vector<std::uint8_t>& bytes) {
    require_size(bytes, copy_payload_bytes);
    if (read_u32(bytes, 0) > 1 || read_u32(bytes, 4) > 1 || read_u32(bytes, 8) > 1 || read_u32(bytes, 52) != 0)
        throw std::invalid_argument("invalid copy command fields");
    CopyCommand result;
    result.image_source = read_u32(bytes, 0) != 0;
    result.source = static_cast<int>(read_u32(bytes, 4)); result.target = static_cast<int>(read_u32(bytes, 8));
    result.source_origin = {read_i32(bytes, 12), read_i32(bytes, 16)};
    result.destination_origin = {read_i32(bytes, 20), read_i32(bytes, 24)};
    result.width = read_i32(bytes, 28); result.height = read_i32(bytes, 32);
    result.image = get_texture(bytes, 36);
    encode_copy_command(result); // Same bounded validation for producer and consumer.
    return result;
}

HostProgram compile_pass(const Pass& pass, std::uint32_t first_sequence) {
    if (first_sequence == 0 || pass.primitives.size() > maximum_primitive_records)
        throw std::invalid_argument("invalid first sequence or too many primitive records");
    HeapBuilder heap;
    std::unordered_map<const Image*, TextureDescriptor> textures;
    std::vector<std::optional<TextureDescriptor>> bindings(pass.primitives.size());
    for (std::size_t index = 0; index < pass.primitives.size(); ++index) {
        const auto& primitive = pass.primitives[index];
        validate_primitive(primitive);
        if (!needs_texture(primitive)) continue;
        const auto image = std::visit([](const auto& item) { return item.state.texture; }, primitive);
        if (!image || !image->texture_compatible()) throw std::invalid_argument("invalid host texture binding");
        const auto found = textures.find(image.get());
        if (found != textures.end()) { bindings[index] = found->second; continue; }
        const auto extent = static_cast<std::uint64_t>(image->pitch_bytes()) * image->height();
        const auto address = heap.allocate(extent);
        TextureDescriptor texture{address, static_cast<std::uint32_t>(image->width()),
            static_cast<std::uint32_t>(image->height()), static_cast<std::uint32_t>(image->pitch_bytes())};
        require_texture(texture);
        const auto offset = static_cast<std::size_t>(address - heap_base);
        for (int y = 0; y < image->height(); ++y) for (int x = 0; x < image->width(); ++x)
            write_u16(heap.bytes, offset + static_cast<std::size_t>(y) * image->pitch_bytes() + 2u * x, image->at(x, y));
        textures.emplace(image.get(), texture);
        bindings[index] = texture;
    }
    std::vector<std::uint32_t> records;
    records.reserve(pass.primitives.size());
    for (std::size_t index = 0; index < pass.primitives.size(); ++index) {
        const auto address = heap.allocate(primitive_record_bytes);
        heap.put(address, encode_primitive(pass.primitives[index], bindings[index]));
        records.push_back(address);
    }
    const auto bins = bin_primitives(pass.primitives);
    std::array<TileEntry, tile_count> entries{};
    HostProgram result;
    for (std::size_t tile = 0; tile < bins.size(); ++tile) {
        const auto count = bins[tile].size();
        if (count > maximum_pass_references || result.reference_count + count > maximum_pass_references)
            throw std::invalid_argument("compiled pass exceeds reference counter capacity");
        result.reference_count += count;
        if (count == 0) continue;
        entries[tile] = {heap.allocate(4ull * count), static_cast<std::uint32_t>(count)};
        const auto offset = static_cast<std::size_t>(entries[tile].address - heap_base);
        for (std::size_t i = 0; i < count; ++i) write_u32(heap.bytes, offset + 4 * i, records.at(bins[tile][i]));
    }
    const auto table = heap.allocate(tile_count * tile_entry_bytes);
    const auto table_offset = static_cast<std::size_t>(table - heap_base);
    for (std::size_t tile = 0; tile < entries.size(); ++tile) {
        write_u32(heap.bytes, table_offset + tile * tile_entry_bytes, entries[tile].address);
        write_u32(heap.bytes, table_offset + tile * tile_entry_bytes + 4, entries[tile].count);
    }
    result.descriptor_address = heap.allocate(pass_descriptor_bytes);
    heap.put(result.descriptor_address, encode_pass_descriptor({pass.target, pass.color_load, pass.depth_load,
        pass.clear_color, pass.clear_depth, table}));
    constexpr std::size_t upload_chunk = maximum_packet_payload - 8;
    const auto packet_count = (heap.bytes.size() + upload_chunk - 1) / upload_chunk + 2;
    if (packet_count > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) - first_sequence)
        throw std::overflow_error("compiled program requires a new sequence epoch");
    auto sequence = first_sequence;
    const auto append = [&](Opcode opcode, const std::vector<std::uint8_t>& payload) {
        auto packet = encode_packet(opcode, sequence++, payload);
        result.bytes.insert(result.bytes.end(), packet.begin(), packet.end());
    };
    for (std::size_t offset = 0; offset < heap.bytes.size(); offset += upload_chunk) {
        const auto length = std::min(upload_chunk, heap.bytes.size() - offset);
        std::vector<std::uint8_t> payload(8 + length);
        write_u32(payload, 0, heap_base + static_cast<std::uint32_t>(offset));
        write_u32(payload, 4, static_cast<std::uint32_t>(length));
        std::copy_n(heap.bytes.begin() + static_cast<std::ptrdiff_t>(offset), length, payload.begin() + 8);
        append(Opcode::WriteHeap, payload);
    }
    std::vector<std::uint8_t> payload(4);
    write_u32(payload, 0, result.descriptor_address); append(Opcode::SubmitPass, payload);
    result.fence_sequence = sequence; result.fence_token = sequence;
    write_u32(payload, 0, result.fence_token); append(Opcode::Fence, payload);
    result.next_sequence = sequence;
    result.upload_bytes = heap.bytes.size(); result.heap_bytes_used = heap.bytes.size();
    return result;
}

} // namespace nano_gpu::hw
