#include "jpeg444_udp_transport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        return 1; \
    } \
} while (0)

static JxsUdpHeader header_for(uint64_t frame_id, uint8_t tile_id,
                               uint32_t tile_bytes, uint16_t index)
{
    JxsUdpHeader h;
    const size_t count = jxs_udp_fragment_count(tile_bytes);
    const size_t offset = (size_t)index * JXS_UDP_PAYLOAD_BYTES;
    const size_t remaining = tile_bytes - offset;
    memset(&h, 0, sizeof(h));
    h.frame_id = frame_id;
    h.tile_id = tile_id;
    h.tile_bytes = tile_bytes;
    h.fragment_index = index;
    h.fragment_count = (uint16_t)count;
    h.fragment_offset = (uint32_t)offset;
    h.payload_bytes = (uint16_t)(remaining < JXS_UDP_PAYLOAD_BYTES ?
                                  remaining : JXS_UDP_PAYLOAD_BYTES);
    return h;
}

static int test_header_literal_and_round_trip(void)
{
    const uint8_t expected[JXS_UDP_HEADER_BYTES] = {
        0x4a, 0x58, 0x53, 0x34, 0x01, 0x02, 0x00, 0x20,
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x00, 0x00, 0x07, 0xd0, 0x00, 0x00, 0x05, 0x58,
        0x00, 0x01, 0x00, 0x02, 0x02, 0x78, 0x00, 0x00,
    };
    JxsUdpHeader input = header_for(UINT64_C(0x0102030405060708), 2, 2000, 1);
    JxsUdpHeader output;
    uint8_t datagram[JXS_UDP_DATAGRAM_BYTES] = {0};
    size_t i;

    CHECK(jxs_udp_header_encode(datagram, &input) == 0);
    if (memcmp(datagram, expected, sizeof(expected)) != 0) {
        fprintf(stderr, "actual header:");
        for (i = 0; i < sizeof(expected); i++)
            fprintf(stderr, " %02x", datagram[i]);
        fputc('\n', stderr);
    }
    CHECK(memcmp(datagram, expected, sizeof(expected)) == 0);
    CHECK(jxs_udp_header_decode(&output, datagram,
                                JXS_UDP_HEADER_BYTES + input.payload_bytes) == 0);
    CHECK(output.frame_id == input.frame_id);
    CHECK(output.tile_id == input.tile_id);
    CHECK(output.tile_bytes == input.tile_bytes);
    CHECK(output.fragment_offset == input.fragment_offset);
    CHECK(output.fragment_index == input.fragment_index);
    CHECK(output.fragment_count == input.fragment_count);
    CHECK(output.payload_bytes == input.payload_bytes);
    return 0;
}

static int test_fragment_count_boundaries(void)
{
    CHECK(jxs_udp_fragment_count(0) == 0);
    CHECK(jxs_udp_fragment_count(1) == 1);
    CHECK(jxs_udp_fragment_count(JXS_UDP_PAYLOAD_BYTES) == 1);
    CHECK(jxs_udp_fragment_count(JXS_UDP_PAYLOAD_BYTES + 1) == 2);
    CHECK(jxs_udp_fragment_count(JXS_UDP_PAYLOAD_BYTES * 2) == 2);
    return 0;
}

static int test_reverse_order_reassembly(void)
{
    enum { TILE_BYTES = 3000 };
    uint8_t source[TILE_BYTES], destination[TILE_BYTES], seen[3];
    JxsUdpReassembly state;
    int index;
    size_t i;

    for (i = 0; i < sizeof(source); i++)
        source[i] = (uint8_t)((i * 17u + 3u) & 0xffu);
    memset(destination, 0, sizeof(destination));
    CHECK(jxs_udp_reassembly_reset(&state, 77, 3, TILE_BYTES,
                                   destination, sizeof(destination),
                                   seen, sizeof(seen)) == 0);
    for (index = 2; index >= 0; index--) {
        JxsUdpHeader h = header_for(77, 3, TILE_BYTES, (uint16_t)index);
        CHECK(jxs_udp_reassembly_push(&state, &h,
                                      source + h.fragment_offset) ==
              JXS_UDP_PUSH_ACCEPTED);
    }
    CHECK(jxs_udp_reassembly_complete(&state) == 1);
    CHECK(state.received_bytes == TILE_BYTES);
    CHECK(memcmp(source, destination, TILE_BYTES) == 0);
    return 0;
}

static int test_duplicate_and_conflict(void)
{
    enum { TILE_BYTES = 1500 };
    uint8_t source[TILE_BYTES], altered[JXS_UDP_PAYLOAD_BYTES];
    uint8_t destination[TILE_BYTES], seen[2];
    JxsUdpReassembly state;
    JxsUdpHeader h = header_for(5, 1, TILE_BYTES, 0);

    memset(source, 0x5a, sizeof(source));
    memcpy(altered, source, sizeof(altered));
    altered[11] ^= 0xff;
    CHECK(jxs_udp_reassembly_reset(&state, 5, 1, TILE_BYTES,
                                   destination, sizeof(destination),
                                   seen, sizeof(seen)) == 0);
    CHECK(jxs_udp_reassembly_push(&state, &h, source) == JXS_UDP_PUSH_ACCEPTED);
    CHECK(jxs_udp_reassembly_push(&state, &h, source) == JXS_UDP_PUSH_DUPLICATE);
    CHECK(state.received_fragments == 1);
    CHECK(jxs_udp_reassembly_push(&state, &h, altered) == JXS_UDP_ERR_CONFLICT);
    CHECK(state.received_fragments == 1);
    return 0;
}

static int test_malformed_headers(void)
{
    JxsUdpHeader h = header_for(9, 0, 2000, 0);
    JxsUdpHeader decoded;
    uint8_t datagram[JXS_UDP_DATAGRAM_BYTES] = {0};
    uint8_t saved;

    CHECK(jxs_udp_header_encode(datagram, &h) == 0);

    saved = datagram[0]; datagram[0] = 0;
    CHECK(jxs_udp_header_decode(&decoded, datagram,
                                JXS_UDP_HEADER_BYTES + h.payload_bytes) < 0);
    datagram[0] = saved;
    saved = datagram[4]; datagram[4] = 2;
    CHECK(jxs_udp_header_decode(&decoded, datagram,
                                JXS_UDP_HEADER_BYTES + h.payload_bytes) < 0);
    datagram[4] = saved;
    saved = datagram[5]; datagram[5] = JXS_UDP_TILE_COUNT;
    CHECK(jxs_udp_header_decode(&decoded, datagram,
                                JXS_UDP_HEADER_BYTES + h.payload_bytes) < 0);
    datagram[5] = saved;
    saved = datagram[7]; datagram[7] = 31;
    CHECK(jxs_udp_header_decode(&decoded, datagram,
                                JXS_UDP_HEADER_BYTES + h.payload_bytes) < 0);
    datagram[7] = saved;
    CHECK(jxs_udp_header_decode(&decoded, datagram,
                                JXS_UDP_HEADER_BYTES + h.payload_bytes - 1) < 0);

    h.fragment_count++;
    CHECK(jxs_udp_header_encode(datagram, &h) < 0);
    h = header_for(9, 0, 2000, 0);
    h.fragment_index = h.fragment_count;
    CHECK(jxs_udp_header_encode(datagram, &h) < 0);
    h = header_for(9, 0, 2000, 0);
    h.fragment_offset++;
    CHECK(jxs_udp_header_encode(datagram, &h) < 0);
    h = header_for(9, 0, 2000, 0);
    h.payload_bytes--;
    CHECK(jxs_udp_header_encode(datagram, &h) < 0);
    return 0;
}

static int test_reassembly_capacity_checks(void)
{
    uint8_t buffer[100], seen[1];
    JxsUdpReassembly state;
    CHECK(jxs_udp_reassembly_reset(&state, 1, 0, 101,
                                   buffer, sizeof(buffer),
                                   seen, sizeof(seen)) == JXS_UDP_ERR_RANGE);
    CHECK(jxs_udp_reassembly_reset(&state, 1, 4, 100,
                                   buffer, sizeof(buffer),
                                   seen, sizeof(seen)) == JXS_UDP_ERR_INVALID);
    CHECK(jxs_udp_reassembly_reset(&state, 1, 0, 0,
                                   buffer, sizeof(buffer),
                                   seen, sizeof(seen)) == JXS_UDP_ERR_INVALID);
    return 0;
}

int main(void)
{
    CHECK(test_header_literal_and_round_trip() == 0);
    CHECK(test_fragment_count_boundaries() == 0);
    CHECK(test_reverse_order_reassembly() == 0);
    CHECK(test_duplicate_and_conflict() == 0);
    CHECK(test_malformed_headers() == 0);
    CHECK(test_reassembly_capacity_checks() == 0);
    puts("PASS jpeg444_udp_transport");
    return 0;
}
