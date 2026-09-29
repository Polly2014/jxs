#include "jpeg444_udp_transport.h"

#include <limits.h>
#include <string.h>

static void put_u16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value >> 8);
    out[1] = (uint8_t)value;
}

static void put_u32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

static void put_u64(uint8_t *out, uint64_t value)
{
    put_u32(out, (uint32_t)(value >> 32));
    put_u32(out + 4, (uint32_t)value);
}

static uint16_t get_u16(const uint8_t *in)
{
    return (uint16_t)(((uint16_t)in[0] << 8) | in[1]);
}

static uint32_t get_u32(const uint8_t *in)
{
    return ((uint32_t)in[0] << 24) |
           ((uint32_t)in[1] << 16) |
           ((uint32_t)in[2] << 8) |
           (uint32_t)in[3];
}

static uint64_t get_u64(const uint8_t *in)
{
    return ((uint64_t)get_u32(in) << 32) | get_u32(in + 4);
}

size_t jxs_udp_fragment_count(size_t tile_bytes)
{
    if (tile_bytes == 0)
        return 0;
    return 1 + (tile_bytes - 1) / JXS_UDP_PAYLOAD_BYTES;
}

static int header_fields_valid(const JxsUdpHeader *header)
{
    size_t expected_count;
    size_t expected_offset;
    size_t expected_payload;

    if (!header || header->tile_id >= JXS_UDP_TILE_COUNT ||
        header->tile_bytes == 0)
        return 0;
    expected_count = jxs_udp_fragment_count(header->tile_bytes);
    if (expected_count == 0 || expected_count > UINT16_MAX ||
        header->fragment_count != expected_count ||
        header->fragment_index >= header->fragment_count)
        return 0;
    expected_offset = (size_t)header->fragment_index * JXS_UDP_PAYLOAD_BYTES;
    if (header->fragment_offset != expected_offset ||
        expected_offset >= header->tile_bytes)
        return 0;
    expected_payload = header->tile_bytes - expected_offset;
    if (expected_payload > JXS_UDP_PAYLOAD_BYTES)
        expected_payload = JXS_UDP_PAYLOAD_BYTES;
    return header->payload_bytes == expected_payload;
}

int jxs_udp_header_encode(uint8_t out[JXS_UDP_HEADER_BYTES],
                          const JxsUdpHeader *header)
{
    if (!out || !header_fields_valid(header))
        return JXS_UDP_ERR_INVALID;
    put_u32(out, JXS_UDP_MAGIC);
    out[4] = JXS_UDP_VERSION;
    out[5] = header->tile_id;
    put_u16(out + 6, JXS_UDP_HEADER_BYTES);
    put_u64(out + 8, header->frame_id);
    put_u32(out + 16, header->tile_bytes);
    put_u32(out + 20, header->fragment_offset);
    put_u16(out + 24, header->fragment_index);
    put_u16(out + 26, header->fragment_count);
    put_u16(out + 28, header->payload_bytes);
    put_u16(out + 30, 0);
    return 0;
}

int jxs_udp_header_decode(JxsUdpHeader *header, const uint8_t *datagram,
                          size_t datagram_bytes)
{
    if (!header || !datagram || datagram_bytes < JXS_UDP_HEADER_BYTES ||
        datagram_bytes > JXS_UDP_DATAGRAM_BYTES)
        return JXS_UDP_ERR_INVALID;
    if (get_u32(datagram) != JXS_UDP_MAGIC ||
        datagram[4] != JXS_UDP_VERSION ||
        get_u16(datagram + 6) != JXS_UDP_HEADER_BYTES ||
        get_u16(datagram + 30) != 0)
        return JXS_UDP_ERR_INVALID;
    memset(header, 0, sizeof(*header));
    header->tile_id = datagram[5];
    header->frame_id = get_u64(datagram + 8);
    header->tile_bytes = get_u32(datagram + 16);
    header->fragment_offset = get_u32(datagram + 20);
    header->fragment_index = get_u16(datagram + 24);
    header->fragment_count = get_u16(datagram + 26);
    header->payload_bytes = get_u16(datagram + 28);
    if (!header_fields_valid(header) ||
        datagram_bytes != JXS_UDP_HEADER_BYTES + header->payload_bytes)
        return JXS_UDP_ERR_INVALID;
    return 0;
}

int jxs_udp_reassembly_reset(JxsUdpReassembly *state, uint64_t frame_id,
                             uint8_t tile_id, uint32_t tile_bytes,
                             uint8_t *buffer, size_t buffer_capacity,
                             uint8_t *seen, size_t seen_capacity)
{
    const size_t fragment_count = jxs_udp_fragment_count(tile_bytes);
    if (!state || !buffer || !seen || tile_id >= JXS_UDP_TILE_COUNT ||
        tile_bytes == 0)
        return JXS_UDP_ERR_INVALID;
    if (tile_bytes > buffer_capacity || fragment_count > seen_capacity ||
        fragment_count > UINT16_MAX)
        return JXS_UDP_ERR_RANGE;
    memset(state, 0, sizeof(*state));
    memset(seen, 0, fragment_count);
    state->frame_id = frame_id;
    state->tile_id = tile_id;
    state->tile_bytes = tile_bytes;
    state->fragment_count = (uint16_t)fragment_count;
    state->buffer = buffer;
    state->buffer_capacity = buffer_capacity;
    state->seen = seen;
    state->seen_capacity = seen_capacity;
    state->initialized = 1;
    return 0;
}

int jxs_udp_reassembly_push(JxsUdpReassembly *state,
                            const JxsUdpHeader *header,
                            const uint8_t *payload)
{
    uint8_t *destination;
    if (!state || !state->initialized || !header || !payload ||
        !header_fields_valid(header))
        return JXS_UDP_ERR_INVALID;
    if (header->frame_id != state->frame_id ||
        header->tile_id != state->tile_id ||
        header->tile_bytes != state->tile_bytes ||
        header->fragment_count != state->fragment_count ||
        header->fragment_index >= state->seen_capacity ||
        (size_t)header->fragment_offset + header->payload_bytes >
            state->buffer_capacity)
        return JXS_UDP_ERR_RANGE;
    destination = state->buffer + header->fragment_offset;
    if (state->seen[header->fragment_index]) {
        if (memcmp(destination, payload, header->payload_bytes) != 0)
            return JXS_UDP_ERR_CONFLICT;
        return JXS_UDP_PUSH_DUPLICATE;
    }
    memcpy(destination, payload, header->payload_bytes);
    state->seen[header->fragment_index] = 1;
    state->received_fragments++;
    state->received_bytes += header->payload_bytes;
    return JXS_UDP_PUSH_ACCEPTED;
}

int jxs_udp_reassembly_complete(const JxsUdpReassembly *state)
{
    return state && state->initialized &&
           state->received_fragments == state->fragment_count &&
           state->received_bytes == state->tile_bytes;
}
