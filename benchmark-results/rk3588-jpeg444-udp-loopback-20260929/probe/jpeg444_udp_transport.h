#ifndef JPEG444_UDP_TRANSPORT_H
#define JPEG444_UDP_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

#define JXS_UDP_MAGIC UINT32_C(0x4a585334)
#define JXS_UDP_VERSION 1u
#define JXS_UDP_TILE_COUNT 4u
#define JXS_UDP_HEADER_BYTES 32u
#define JXS_UDP_DATAGRAM_BYTES 1400u
#define JXS_UDP_PAYLOAD_BYTES (JXS_UDP_DATAGRAM_BYTES - JXS_UDP_HEADER_BYTES)

typedef struct {
    uint64_t frame_id;
    uint32_t tile_bytes;
    uint32_t fragment_offset;
    uint16_t fragment_index;
    uint16_t fragment_count;
    uint16_t payload_bytes;
    uint8_t tile_id;
} JxsUdpHeader;

typedef struct {
    uint64_t frame_id;
    uint32_t tile_bytes;
    uint16_t fragment_count;
    uint16_t received_fragments;
    size_t received_bytes;
    uint8_t tile_id;
    uint8_t initialized;
    uint8_t *buffer;
    size_t buffer_capacity;
    uint8_t *seen;
    size_t seen_capacity;
} JxsUdpReassembly;

enum {
    JXS_UDP_PUSH_ACCEPTED = 1,
    JXS_UDP_PUSH_DUPLICATE = 0,
    JXS_UDP_ERR_INVALID = -1,
    JXS_UDP_ERR_RANGE = -2,
    JXS_UDP_ERR_CONFLICT = -3,
};

size_t jxs_udp_fragment_count(size_t tile_bytes);
int jxs_udp_header_encode(uint8_t out[JXS_UDP_HEADER_BYTES],
                          const JxsUdpHeader *header);
int jxs_udp_header_decode(JxsUdpHeader *header, const uint8_t *datagram,
                          size_t datagram_bytes);
int jxs_udp_reassembly_reset(JxsUdpReassembly *state, uint64_t frame_id,
                             uint8_t tile_id, uint32_t tile_bytes,
                             uint8_t *buffer, size_t buffer_capacity,
                             uint8_t *seen, size_t seen_capacity);
int jxs_udp_reassembly_push(JxsUdpReassembly *state,
                            const JxsUdpHeader *header,
                            const uint8_t *payload);
int jxs_udp_reassembly_complete(const JxsUdpReassembly *state);

#endif
