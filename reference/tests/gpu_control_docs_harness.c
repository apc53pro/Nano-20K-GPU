/* Compile the guide itself as C99, not a second implementation of its helpers. */
#include "gpu_control_example.inc"

#include <stdio.h>

extern int gpu_docs_packet_matches(const uint8_t *, size_t, uint16_t, uint32_t,
                                  const uint8_t *, size_t);

static char test_error[256];
#define CHECK(condition) do { if (!(condition)) { \
    (void)snprintf(test_error, sizeof(test_error), "line %d: %s", __LINE__, #condition); \
    return 0; } } while (0)

const char *gpu_docs_test_error(void) { return test_error; }

typedef struct {
    uint8_t written[8192], response[1024], registers[128];
    size_t written_count, response_count, response_cursor, register_reads;
    size_t write_limit;
} Mock;

static void mock_reset(Mock *m) {
    memset(m, 0, sizeof(*m));
    m->write_limit = sizeof(m->written);
}
static bool mock_write(void *user, uint8_t byte) {
    Mock *m = (Mock *)user;
    if (m->written_count >= m->write_limit) return false;
    m->written[m->written_count++] = byte;
    return true;
}
static bool mock_read(void *user, uint8_t *byte) {
    Mock *m = (Mock *)user;
    if (m->response_cursor == m->response_count) return false;
    *byte = m->response[m->response_cursor++];
    return true;
}
static bool mock_register(void *user, uint8_t address, uint8_t *byte) {
    Mock *m = (Mock *)user;
    if (!address || address >= 128) return false;
    ++m->register_reads;
    *byte = m->registers[address];
    return true;
}
static GpuTransport mock_transport(Mock *m) {
    GpuTransport result = {m, mock_write, mock_read, mock_register};
    return result;
}
static void mock_reply(Mock *m, uint16_t opcode, uint32_t sequence,
                       uint32_t epoch, uint32_t token, uint32_t extra) {
    memset(m->response, 0, sizeof(m->response));
    m->response_count = 28 + extra;
    m->response_cursor = 0;
    gpu_put32(m->response, UINT32_C(0x5550474e));
    m->response[5] = 2;
    gpu_put16(m->response + 6, (uint16_t)(opcode | UINT16_C(0x8000)));
    gpu_put32(m->response + 8, 12 + extra);
    gpu_put32(m->response + 12, sequence);
    gpu_put32(m->response + 16, epoch);
    gpu_put32(m->response + 24, token);
}

static int packet_tests(void) {
    Mock m;
    Gpu g;
    uint32_t sequence, offset, index;
    uint8_t data[2052], expected[GPU_MAX_PAYLOAD];
    const uint8_t payload[4] = {1, 0x23, 0x45, 0x67};
    const uint8_t golden[20] = {'N', 'G', 'P', 'U', 0, 2, 3, 0, 4, 0, 0, 0,
                               0x78, 0x56, 0x34, 0x12, 1, 0x23, 0x45, 0x67};
    mock_reset(&m);
    gpu_init(&g, mock_transport(&m));
    g.next_sequence = UINT32_C(0x12345678);
    CHECK(gpu_send(&g, GPU_WRITE_HEAP, payload, 4, &sequence));
    CHECK(sequence == UINT32_C(0x12345678) && g.next_sequence == sequence + 1);
    CHECK(m.written_count == sizeof(golden) && memcmp(m.written, golden, sizeof(golden)) == 0);
    CHECK(gpu_docs_packet_matches(m.written, m.written_count, GPU_WRITE_HEAP, sequence, payload, 4));

    mock_reset(&m);
    gpu_init(&g, mock_transport(&m));
    for (index = 0; index < sizeof(data); ++index) data[index] = (uint8_t)(index * 37u + 11u);
    CHECK(gpu_upload(&g, GPU_TEXTURE_ADDR, data, sizeof(data)));
    offset = 0;
    for (index = 0; index < 3; ++index) {
        uint32_t cursor = index * GPU_MAX_UPLOAD_DECODED;
        uint32_t chunk = (uint32_t)sizeof(data) - cursor;
        uint32_t encoded_length, consumed = 0, produced = 0;
        const uint8_t *encoded;
        if (chunk > GPU_MAX_UPLOAD_DECODED) chunk = GPU_MAX_UPLOAD_DECODED;
        CHECK(offset + 24u <= m.written_count);
        encoded_length = gpu_get32(m.written + offset + 8);
        CHECK(encoded_length >= 9 && encoded_length <= GPU_MAX_PAYLOAD &&
              offset + 16u + encoded_length <= m.written_count);
        if (index < 2) CHECK(encoded_length == GPU_MAX_PAYLOAD);
        encoded = m.written + offset + 16;
        CHECK(gpu_get32(encoded) == GPU_TEXTURE_ADDR + cursor && gpu_get32(encoded + 4) == chunk);
        CHECK(gpu_docs_packet_matches(m.written + offset, 16u + encoded_length,
              GPU_WRITE_HEAP, index + 1, encoded, encoded_length));
        /* Decode independently instead of duplicating the guide's encoder. */
        consumed = 8;
        while (produced < chunk) {
            unsigned slot;
            uint8_t descriptor;
            CHECK(consumed < encoded_length);
            descriptor = encoded[consumed++];
            for (slot = 0; slot < 4 && produced < chunk; ++slot) {
                uint32_t word = 0;
                unsigned code = descriptor & 3u, width = code == 3 ? 4 : code, b;
                descriptor >>= 2;
                CHECK(consumed + width <= encoded_length);
                for (b = 0; b < width; ++b) word |= (uint32_t)encoded[consumed++] << (8u * b);
                if (code == 1 && (word & UINT32_C(0x80))) word |= UINT32_C(0xffffff00);
                if (code == 2 && (word & UINT32_C(0x8000))) word |= UINT32_C(0xffff0000);
                for (b = 0; b < 4 && produced < chunk; ++b) {
                    CHECK((uint8_t)(word >> (8u * b)) == data[cursor + produced]);
                    ++produced;
                }
            }
            CHECK(produced < chunk || descriptor == 0);
        }
        CHECK(consumed == encoded_length);
        offset += 16u + encoded_length;
    }
    CHECK(offset == m.written_count && g.next_sequence == 4);
    CHECK(gpu_upload(&g, GPU_TEXTURE_ADDR, NULL, 0) && offset == m.written_count);
    {
        const uint8_t mixed[17] = {0, 0, 0, 0, 0x80, 0xff, 0xff, 0xff,
                                  0x80, 1, 0, 0, 0x78, 0x56, 0x34, 0x12, 0x7f};
        const uint8_t coded[10] = {0xe4, 0x80, 0x80, 1, 0x78, 0x56, 0x34, 0x12, 1, 0x7f};
        mock_reset(&m); gpu_init(&g, mock_transport(&m));
        CHECK(gpu_upload(&g, GPU_TEXTURE_ADDR + 1, mixed, sizeof(mixed)));
        gpu_put32(expected, GPU_TEXTURE_ADDR + 1); gpu_put32(expected + 4, sizeof(mixed));
        memcpy(expected + 8, coded, sizeof(coded));
        CHECK(gpu_docs_packet_matches(m.written, m.written_count, GPU_WRITE_HEAP, 1,
                                      expected, 8u + sizeof(coded)));
    }
    return 1;
}

static int reply_tests(void) {
    Mock m;
    Gpu g;
    GpuReply reply;
    unsigned change;
    uint8_t result[4];
    mock_reset(&m);
    gpu_init(&g, mock_transport(&m));
    mock_reply(&m, GPU_FENCE, 7, 19, 123, 0);
    CHECK(gpu_reply(&g, GPU_FENCE, 7, 123, 0, &reply));
    CHECK(g.epoch == 19 && reply.payload_bytes == 12 && m.response_cursor == 28);
    for (change = 0; change < 10; ++change) {
        mock_reset(&m);
        gpu_init(&g, mock_transport(&m));
        g.epoch = 19;
        mock_reply(&m, GPU_FENCE, 7, 19, 123, 0);
        switch (change) {
        case 0: m.response[0] ^= 1; break;
        case 1: m.response[4] = 1; break;
        case 2: m.response[5] = 1; break;
        case 3: m.response[6] = GPU_PRESENT; break;
        case 4: gpu_put32(m.response + 12, 8); break;
        case 5: gpu_put32(m.response + 8, UINT32_MAX); break;
        case 6: gpu_put32(m.response + 16, 0); break;
        case 7: gpu_put32(m.response + 16, 20); break;
        case 8: gpu_put32(m.response + 20, 1); break;
        default: gpu_put32(m.response + 24, 124); break;
        }
        CHECK(!gpu_reply(&g, GPU_FENCE, 7, 123, 0, &reply) && g.failed);
        CHECK(m.response_cursor <= 28);
        if (change <= 5) CHECK(m.response_cursor == 16);
    }
    mock_reset(&m);
    gpu_init(&g, mock_transport(&m));
    mock_reply(&m, GPU_FENCE, 7, 19, 123, 0);
    m.response_count = 9;
    CHECK(!gpu_reply(&g, GPU_FENCE, 7, 123, 0, &reply) && g.failed);
    CHECK(m.response_cursor == 9);
    CHECK(!gpu_reply(&g, GPU_FENCE, 7, 123, 0, &reply) && m.response_cursor == 9);

    /* Correct envelope but mismatched READ_MEMORY metadata must not copy data. */
    for (change = 0; change < 2; ++change) {
        mock_reset(&m);
        gpu_init(&g, mock_transport(&m));
        mock_reply(&m, GPU_READ_MEMORY, 1, 19, 222, 12);
        gpu_put32(m.response + 28, GPU_HEAP_BASE + (change == 0 ? 4 : 0));
        gpu_put32(m.response + 32, change == 1 ? 3 : 4);
        memset(result, 0xa5, sizeof(result));
        CHECK(!gpu_read_memory(&g, GPU_HEAP_BASE, 4, 222, result) && g.failed);
        CHECK(result[0] == 0xa5 && result[3] == 0xa5);
    }
    return 1;
}

static int invalid_input_tests(void) {
    Mock m;
    Gpu g;
    GpuReply reply;
    uint8_t bytes[320] = {0}, caps[64], status[64];
    uint32_t sequence;
    GpuBounds bounds = {0, 0, 4, 4};
    mock_reset(&m);
    gpu_init(&g, mock_transport(&m));
    CHECK(!gpu_send(&g, 0, NULL, 0, &sequence));
    CHECK(!gpu_send(&g, 9, NULL, 0, &sequence));
    CHECK(!gpu_send(&g, GPU_FENCE, bytes, 1025, &sequence));
    CHECK(!gpu_send(&g, GPU_FENCE, NULL, 4, &sequence));
    CHECK(!gpu_send(&g, GPU_FENCE, bytes, 4, NULL));
    g.next_sequence = UINT32_MAX;
    CHECK(!gpu_send(&g, GPU_FENCE, bytes, 4, &sequence));
    g.next_sequence = 0;
    CHECK(!gpu_send(&g, GPU_FENCE, bytes, 4, &sequence));
    g.next_sequence = 1;
    CHECK(!gpu_upload(&g, GPU_HEAP_BASE - 1, bytes, 1));
    CHECK(!gpu_upload(&g, GPU_MEMORY_BYTES - 4, bytes, 8));
    CHECK(!gpu_upload(&g, UINT32_MAX - 3, bytes, 8));
    CHECK(!gpu_upload(&g, GPU_HEAP_BASE, NULL, 4));
    CHECK(!gpu_read_memory(&g, 0, 0, 1, bytes));
    CHECK(!gpu_read_memory(&g, 0, 513, 1, bytes));
    CHECK(!gpu_read_memory(&g, GPU_MEMORY_BYTES - 1, 2, 1, bytes));
    CHECK(!gpu_read_memory(&g, 0, 4, 1, NULL));
    CHECK(!gpu_present(&g, 2, 1));
    CHECK(!gpu_submit_one(&g, 2, 0, bytes, bounds));
    CHECK(!gpu_patch_bounds(&g, GPU_RECORD_ADDR + 1, bounds));
    CHECK(!gpu_patch_bounds(&g, GPU_MEMORY_BYTES - 4, bounds));
    CHECK(!gpu_query(&g, GPU_FENCE, caps));
    CHECK(!gpu_reply(&g, GPU_FENCE, 1, 1, GPU_MAX_REPLY_PAYLOAD, &reply));
    CHECK(!gpu_make_sprite(bytes, bounds, 0, 0, GPU_TEXTURE_ADDR, 3, 4, 8, false, 0));
    CHECK(!gpu_make_sprite(bytes, bounds, -1, 0, GPU_TEXTURE_ADDR, 4, 4, 8, false, 0));
    CHECK(!gpu_make_sprite(bytes, bounds, 1, 0, GPU_TEXTURE_ADDR, 4, 4, 8, false, 0));
    CHECK(!gpu_make_sprite(bytes, bounds, 0, 0, GPU_TEXTURE_ADDR + 1, 4, 4, 8, false, 0));
    CHECK(!gpu_make_sprite(bytes, bounds, 0, 0, GPU_TEXTURE_ADDR, 4, 4, 6, false, 0));
    CHECK(!gpu_make_sprite(bytes, bounds, 0, 0, GPU_MEMORY_BYTES - 4, 4, 4, 8, false, 0));
    CHECK(m.written_count == 0 && m.response_cursor == 0);
    CHECK(!gpu_start_session(&g, mock_transport(&m), 0, caps, status));
    CHECK(m.register_reads == 0);
    CHECK(!gpu_start_session(&g, mock_transport(&m), 3, caps, status));
    CHECK(m.register_reads == 3u * 63u && m.written_count == 0);
    mock_reset(&m);
    gpu_init(&g, mock_transport(&m));
    m.write_limit = 3;
    CHECK(!gpu_send(&g, GPU_FENCE, bytes, 4, &sequence) && g.failed);
    CHECK(m.written_count == 3);
    CHECK(!gpu_send(&g, GPU_FENCE, bytes, 4, &sequence) && m.written_count == 3);
    return 1;
}

int gpu_docs_mock_tests(void) {
    return packet_tests() && reply_tests() && invalid_input_tests();
}

static uint32_t pixel_address(uint32_t target, uint32_t x, uint32_t y) {
    return target * UINT32_C(0x96000) + 2u * (y * 640u + x);
}
static bool read_pixel(Gpu *g, uint32_t target, uint32_t x, uint32_t y, uint16_t expected) {
    uint8_t bytes[2];
    return gpu_read_memory(g, pixel_address(target, x, y), 2, 900, bytes) &&
           ((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8)) == expected;
}
static bool read_depth(Gpu *g, uint32_t x, uint32_t y, uint32_t expected) {
    uint8_t bytes[4];
    return gpu_read_memory(g, UINT32_C(0x12c000) + 4u * (y * 640u + x), 4, 901, bytes) &&
           gpu_get32(bytes) == expected;
}
static bool read_bin(Gpu *g, uint32_t tile, bool occupied) {
    uint8_t bytes[8];
    if (!gpu_read_memory(g, GPU_TABLE_ADDR + 8u * tile, 8, 902, bytes)) return false;
    return gpu_get32(bytes) == (occupied ? GPU_REFS_ADDR + 4u * tile : 0) &&
           gpu_get32(bytes + 4) == (occupied ? 1u : 0u);
}

int gpu_docs_model_tests(void *user, bool (*write_byte)(void *, uint8_t),
                         bool (*read_byte)(void *, uint8_t *),
                         bool (*read_register)(void *, uint8_t, uint8_t *)) {
    GpuTransport transport = {user, write_byte, read_byte, read_register};
    Gpu g;
    uint8_t caps[64], status[64], record[320], pixels[32], bytes[32], copy[56] = {0};
    uint8_t maximum_readback[512];
    uint32_t target, index, sequence;
    GpuBounds empty = {0, 0, 0, 0};
    GpuBounds rectangle = {30, 30, 35, 35};
    GpuBounds triangle = {0, 0, 16, 16};
    GpuBounds sprite = {30, 30, 34, 34};
    GpuBounds moved = {95, 63, 99, 67};

    CHECK(gpu_start_session(&g, transport, 1000, caps, status));
    CHECK(g.epoch != 0 && gpu_get32(caps + 52) == 640 && gpu_get32(caps + 56) == 480);
    CHECK(gpu_query(&g, GPU_GET_STATUS, status));
    target = status[14] ^ 1u;
    CHECK(target <= 1);

    CHECK(gpu_submit_one(&g, target, UINT16_C(0x001f), NULL, empty));
    CHECK(gpu_fence(&g, 100));
    CHECK(read_pixel(&g, target, 0, 0, UINT16_C(0x001f)));
    CHECK(read_pixel(&g, target, 639, 479, UINT16_C(0x001f)));
    CHECK(read_depth(&g, 639, 479, UINT32_C(0xffffff)));
    CHECK(gpu_read_memory(&g, pixel_address(target, 0, 0), 512, 904, maximum_readback));
    for (index = 0; index < 256; ++index)
        CHECK(maximum_readback[2u * index] == 0x1f && maximum_readback[2u * index + 1] == 0);
    /* WRITE_HEAP and READ_MEMORY are byte granular, unlike heap structures. */
    for (index = 0; index < 7; ++index) bytes[index] = (uint8_t)(0x31u + index);
    CHECK(gpu_upload(&g, GPU_TEXTURE_ADDR + 65, bytes, 7));
    CHECK(gpu_fence(&g, 99));
    CHECK(gpu_read_memory(&g, GPU_TEXTURE_ADDR + 65, 7, 905, maximum_readback));
    CHECK(memcmp(bytes, maximum_readback, 7) == 0);
    CHECK(gpu_present(&g, target, 101));
    CHECK(gpu_snapshot(&g, status) && status[14] == target);
    target ^= 1u;

    gpu_make_rect(record, rectangle, UINT32_C(0x0080ff));
    CHECK(gpu_submit_one(&g, target, 0, record, rectangle));
    CHECK(gpu_fence(&g, 102));
    CHECK(read_pixel(&g, target, 29, 30, 0));
    CHECK(read_pixel(&g, target, 30, 30, UINT16_C(0xfc00)));
    CHECK(read_pixel(&g, target, 31, 31, UINT16_C(0xfc00)));
    CHECK(read_pixel(&g, target, 32, 32, UINT16_C(0xfc00)));
    CHECK(read_pixel(&g, target, 34, 34, UINT16_C(0xfc00)));
    CHECK(read_pixel(&g, target, 35, 34, 0));
    CHECK(read_bin(&g, 0, true) && read_bin(&g, 1, true));
    CHECK(read_bin(&g, 20, true) && read_bin(&g, 21, true) && read_bin(&g, 2, false));

    gpu_make_example_triangle(record);
    CHECK(gpu_submit_one(&g, target, 0, record, triangle));
    CHECK(gpu_fence(&g, 104));
    CHECK(read_pixel(&g, target, 0, 0, UINT16_C(0xfa04)));
    CHECK(read_pixel(&g, target, 7, 7, UINT16_C(0xfa04)));
    CHECK(read_pixel(&g, target, 8, 8, 0));
    CHECK(read_pixel(&g, target, 15, 0, 0));
    CHECK(read_depth(&g, 0, 0, UINT32_C(0x400000)));
    CHECK(read_depth(&g, 8, 8, UINT32_C(0xffffff)));

    for (index = 0; index < 16; ++index)
        gpu_put16(pixels + 2u * index, (index & 1) ? UINT16_C(0x07e0) : UINT16_C(0xf81f));
    CHECK(gpu_upload(&g, GPU_TEXTURE_ADDR, pixels, sizeof(pixels)));
    CHECK(gpu_make_sprite(record, sprite, 0, 0, GPU_TEXTURE_ADDR, 4, 4, 8, true, UINT16_C(0xf81f)));
    CHECK(gpu_submit_one(&g, target, UINT16_C(0x001f), record, sprite));
    CHECK(gpu_fence(&g, 106));
    CHECK(read_pixel(&g, target, 30, 30, UINT16_C(0x001f)));
    CHECK(read_pixel(&g, target, 31, 30, UINT16_C(0x07e0)));
    CHECK(read_pixel(&g, target, 32, 30, UINT16_C(0x001f)));
    CHECK(read_pixel(&g, target, 33, 33, UINT16_C(0x07e0)));
    CHECK(read_pixel(&g, target, 34, 33, UINT16_C(0x001f)));

    /* A resident bounds patch is real memory traffic, but moving to other
       tiles also requires rebuilding the CPU's bins before the next pass. */
    CHECK(gpu_patch_bounds(&g, GPU_RECORD_ADDR, moved));
    CHECK(gpu_fence(&g, 108));
    CHECK(gpu_read_memory(&g, GPU_RECORD_ADDR + 32, 16, 903, bytes));
    CHECK(gpu_get32(bytes) == 95 && gpu_get32(bytes + 4) == 63 &&
          gpu_get32(bytes + 8) == 99 && gpu_get32(bytes + 12) == 67);
    gpu_put_bounds(record + 32, moved);
    CHECK(gpu_submit_one(&g, target, UINT16_C(0x001f), record, moved));
    CHECK(gpu_fence(&g, 110));
    CHECK(read_pixel(&g, target, 31, 30, UINT16_C(0x001f)));
    CHECK(read_pixel(&g, target, 95, 63, UINT16_C(0x001f)));
    CHECK(read_pixel(&g, target, 96, 63, UINT16_C(0x07e0)));
    CHECK(read_pixel(&g, target, 98, 66, UINT16_C(0x07e0)));
    CHECK(read_bin(&g, 0, false) && read_bin(&g, 1, false));
    CHECK(read_bin(&g, 20, false) && read_bin(&g, 21, false));
    CHECK(read_bin(&g, 22, true) && read_bin(&g, 23, true));
    CHECK(read_bin(&g, 42, true) && read_bin(&g, 43, true));

    /* COPY_RECT uses the same generic packet API, and copies raw RGB565. */
    gpu_put32(copy, 1); gpu_put32(copy + 8, target);
    gpu_put32(copy + 20, 200); gpu_put32(copy + 24, 100);
    gpu_put32(copy + 28, 4); gpu_put32(copy + 32, 4);
    gpu_put32(copy + 36, GPU_TEXTURE_ADDR); gpu_put32(copy + 40, 4);
    gpu_put32(copy + 44, 4); gpu_put32(copy + 48, 8);
    CHECK(gpu_send(&g, GPU_COPY_RECT, copy, sizeof(copy), &sequence));
    CHECK(gpu_fence(&g, 112));
    CHECK(read_pixel(&g, target, 200, 100, UINT16_C(0xf81f)));
    CHECK(read_pixel(&g, target, 201, 100, UINT16_C(0x07e0)));
    CHECK(gpu_present(&g, target, 113));
    CHECK(gpu_snapshot(&g, status) && status[14] == target);
    CHECK((status[15] & 7) == 7 && !g.failed);
    return 1;
}
