/*
 * THROWAWAY FEASIBILITY PROBE — not product code.
 *
 * Extends the accepted RK3588 JPEG444 board-local probe with real UDP/IP
 * packetization and reassembly. Physical-LAN propagation is not measured.
 */

#ifndef JXS_BOARDLOCAL_PROBE_SOURCE
#define JXS_BOARDLOCAL_PROBE_SOURCE \
    "../../rk3588-jpeg444-pipeline-20260929/probe/rk3588_jpeg444_pipeline_probe.c"
#endif

#define main rk3588_boardlocal_unused_main
#include JXS_BOARDLOCAL_PROBE_SOURCE
#undef main

#include "jpeg444_udp_transport.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

enum { UDP_SLOT_ENCODED = 4 };

typedef struct {
    double encode_ms;
    double send_ms;
    double network_ms;
    double decode_assemble_ms;
    double latency_ms;
    double schedule_late_ms;
    double complete_ms;
    uint64_t datagrams_sent;
    uint64_t datagrams_received;
    uint64_t app_bytes_sent;
    uint64_t app_bytes_received;
    uint64_t checksum;
    int checksum_sampled;
} UdpFrameResult;

typedef struct {
    FrameSlot codec_slot;
    unsigned char *received_jpeg[TILE_COUNT];
    unsigned char *seen[TILE_COUNT];
    JxsUdpReassembly reassembly[TILE_COUNT];
    int tile_started[TILE_COUNT];
    int tile_complete[TILE_COUNT];
    double send_start_ms;
    double send_end_ms;
    double receive_complete_ms;
    uint64_t datagrams_sent;
    uint64_t datagrams_received;
    uint64_t app_bytes_sent;
    uint64_t app_bytes_received;
} UdpFrameSlot;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    UdpFrameSlot *slots;
    UdpFrameResult *results;
    Decoder decoders[TILE_COUNT];
    unsigned char *assembled_nv24;
    int capacity;
    int warmups;
    int frames;
    int total_frames;
    int completed;
    int network_completed;
    int failed;
    int max_inflight;
    int inflight;
    int blocked_frames;
    int checksum_every;
    int checksum_samples;
    int checksum_mismatch;
    int transport_errors;
    int malformed_packets;
    int conflicting_duplicates;
    int inject_frame;
    uint64_t duplicate_packets;
    uint64_t first_checksum;
    uint64_t last_checksum;
    double producer_wait_ms;
    int receive_socket;
    int send_socket;
    int receive_buffer_bytes;
    int send_buffer_bytes;
    unsigned long jpeg_capacity;
} UdpPipeline;

static void udp_pipeline_fail(UdpPipeline *pipeline)
{
    pthread_mutex_lock(&pipeline->mutex);
    pipeline->failed = 1;
    pthread_cond_broadcast(&pipeline->changed);
    pthread_mutex_unlock(&pipeline->mutex);
}

static int make_udp_sockets(UdpPipeline *pipeline, int port)
{
    struct sockaddr_in address;
    struct timeval timeout;
    int requested = 16 * 1024 * 1024;
    socklen_t option_length = sizeof(int);

    pipeline->receive_socket = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    pipeline->send_socket = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (pipeline->receive_socket < 0 || pipeline->send_socket < 0)
        return -1;
    if (setsockopt(pipeline->receive_socket, SOL_SOCKET, SO_RCVBUF,
                   &requested, sizeof(requested)) != 0 ||
        setsockopt(pipeline->send_socket, SOL_SOCKET, SO_SNDBUF,
                   &requested, sizeof(requested)) != 0)
        return -1;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    if (setsockopt(pipeline->receive_socket, SOL_SOCKET, SO_RCVTIMEO,
                   &timeout, sizeof(timeout)) != 0)
        return -1;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(pipeline->receive_socket, (struct sockaddr *)&address,
             sizeof(address)) != 0)
        return -1;
    if (connect(pipeline->send_socket, (struct sockaddr *)&address,
                sizeof(address)) != 0)
        return -1;
    if (getsockopt(pipeline->receive_socket, SOL_SOCKET, SO_RCVBUF,
                   &pipeline->receive_buffer_bytes, &option_length) != 0)
        return -1;
    option_length = sizeof(int);
    if (getsockopt(pipeline->send_socket, SOL_SOCKET, SO_SNDBUF,
                   &pipeline->send_buffer_bytes, &option_length) != 0)
        return -1;
    if (pipeline->receive_buffer_bytes < 8 * 1024 * 1024 ||
        pipeline->send_buffer_bytes < 8 * 1024 * 1024) {
        fprintf(stderr,
                "UDP socket buffers too small: rcv=%d snd=%d; raise net.core.rmem_max and net.core.wmem_max to at least 16777216\n",
                pipeline->receive_buffer_bytes, pipeline->send_buffer_bytes);
        return -1;
    }
    return 0;
}

static int udp_pipeline_init(UdpPipeline *pipeline, int capacity, int warmups,
                             int frames, unsigned long jpeg_capacity, int port)
{
    const size_t seen_capacity = jxs_udp_fragment_count(jpeg_capacity);
    int i, t;

    memset(pipeline, 0, sizeof(*pipeline));
    pipeline->receive_socket = -1;
    pipeline->send_socket = -1;
    pipeline->capacity = capacity;
    pipeline->warmups = warmups;
    pipeline->frames = frames;
    pipeline->total_frames = warmups + frames;
    pipeline->jpeg_capacity = jpeg_capacity;
    pipeline->checksum_every = 1;
    if (getenv("JXS_CHECKSUM_EVERY")) {
        pipeline->checksum_every = atoi(getenv("JXS_CHECKSUM_EVERY"));
        if (pipeline->checksum_every < 0)
            pipeline->checksum_every = 0;
    }
    pipeline->slots = calloc((size_t)capacity, sizeof(*pipeline->slots));
    pipeline->results = calloc((size_t)frames, sizeof(*pipeline->results));
    pipeline->assembled_nv24 = malloc((size_t)WIDTH * HEIGHT * 3);
    if (!pipeline->slots || !pipeline->results || !pipeline->assembled_nv24 ||
        seen_capacity == 0)
        return -1;
    pthread_mutex_init(&pipeline->mutex, NULL);
    pthread_cond_init(&pipeline->changed, NULL);
    for (i = 0; i < capacity; i++) {
        for (t = 0; t < TILE_COUNT; t++) {
            pipeline->slots[i].codec_slot.jpeg[t] = tjAlloc((int)jpeg_capacity);
            pipeline->slots[i].received_jpeg[t] = tjAlloc((int)jpeg_capacity);
            pipeline->slots[i].seen[t] = calloc(seen_capacity, 1);
            if (!pipeline->slots[i].codec_slot.jpeg[t] ||
                !pipeline->slots[i].received_jpeg[t] ||
                !pipeline->slots[i].seen[t])
                return -1;
        }
    }
    for (t = 0; t < TILE_COUNT; t++)
        if (decoder_init(&pipeline->decoders[t], jpeg_capacity) != 0)
            return -1;
    return make_udp_sockets(pipeline, port);
}

static void udp_pipeline_deinit(UdpPipeline *pipeline)
{
    int i, t;
    for (t = 0; t < TILE_COUNT; t++)
        decoder_deinit(&pipeline->decoders[t]);
    if (pipeline->slots) {
        for (i = 0; i < pipeline->capacity; i++) {
            for (t = 0; t < TILE_COUNT; t++) {
                if (pipeline->slots[i].codec_slot.jpeg[t])
                    tjFree(pipeline->slots[i].codec_slot.jpeg[t]);
                if (pipeline->slots[i].received_jpeg[t])
                    tjFree(pipeline->slots[i].received_jpeg[t]);
                free(pipeline->slots[i].seen[t]);
            }
        }
    }
    if (pipeline->receive_socket >= 0)
        close(pipeline->receive_socket);
    if (pipeline->send_socket >= 0)
        close(pipeline->send_socket);
    pthread_cond_destroy(&pipeline->changed);
    pthread_mutex_destroy(&pipeline->mutex);
    free(pipeline->assembled_nv24);
    free(pipeline->results);
    free(pipeline->slots);
}

static int udp_acquire_slot(UdpPipeline *pipeline, int frame_id,
                            UdpFrameSlot **out, double *wait_ms)
{
    UdpFrameSlot *slot = &pipeline->slots[frame_id % pipeline->capacity];
    double start = now_ms();
    int t;

    pthread_mutex_lock(&pipeline->mutex);
    while (slot->codec_slot.state != SLOT_FREE && !pipeline->failed)
        pthread_cond_wait(&pipeline->changed, &pipeline->mutex);
    *wait_ms = now_ms() - start;
    if (pipeline->failed) {
        pthread_mutex_unlock(&pipeline->mutex);
        return -1;
    }
    memset(slot->tile_started, 0, sizeof(slot->tile_started));
    memset(slot->tile_complete, 0, sizeof(slot->tile_complete));
    memset(slot->reassembly, 0, sizeof(slot->reassembly));
    slot->send_start_ms = 0.0;
    slot->send_end_ms = 0.0;
    slot->receive_complete_ms = 0.0;
    slot->datagrams_sent = 0;
    slot->datagrams_received = 0;
    slot->app_bytes_sent = 0;
    slot->app_bytes_received = 0;
    slot->codec_slot.state = SLOT_ENCODING;
    slot->codec_slot.frame_id = (uint64_t)frame_id;
    slot->codec_slot.measured_index = frame_id - pipeline->warmups;
    for (t = 0; t < TILE_COUNT; t++)
        slot->codec_slot.jpeg_size[t] = 0;
    pthread_mutex_unlock(&pipeline->mutex);
    *out = slot;
    return 0;
}

static int send_udp_frame(UdpPipeline *pipeline, UdpFrameSlot *slot,
                          int inject_malformed);

static void udp_publish_slot(UdpPipeline *pipeline, UdpFrameSlot *slot)
{
    pthread_mutex_lock(&pipeline->mutex);
    slot->codec_slot.state = (SlotState)UDP_SLOT_ENCODED;
    pipeline->inflight++;
    if (pipeline->inflight > pipeline->max_inflight)
        pipeline->max_inflight = pipeline->inflight;
    pthread_cond_broadcast(&pipeline->changed);
    pthread_mutex_unlock(&pipeline->mutex);
}

static void *udp_sender_thread(void *opaque)
{
    UdpPipeline *pipeline = opaque;
    int expected;

    for (expected = 0; expected < pipeline->total_frames; expected++) {
        UdpFrameSlot *slot = &pipeline->slots[expected % pipeline->capacity];
        pthread_mutex_lock(&pipeline->mutex);
        while (!pipeline->failed &&
               (slot->codec_slot.state != (SlotState)UDP_SLOT_ENCODED ||
                (int)slot->codec_slot.frame_id != expected))
            pthread_cond_wait(&pipeline->changed, &pipeline->mutex);
        if (pipeline->failed) {
            pthread_mutex_unlock(&pipeline->mutex);
            return NULL;
        }
        slot->codec_slot.state = SLOT_READY;
        pthread_cond_broadcast(&pipeline->changed);
        pthread_mutex_unlock(&pipeline->mutex);
        if (send_udp_frame(pipeline, slot,
                           expected == pipeline->inject_frame) != 0) {
            pipeline->transport_errors++;
            udp_pipeline_fail(pipeline);
            return NULL;
        }
    }
    return NULL;
}

static int send_udp_frame(UdpPipeline *pipeline, UdpFrameSlot *slot,
                          int inject_malformed)
{
    enum { SEND_BATCH = 64 };
    uint8_t datagrams[SEND_BATCH][JXS_UDP_DATAGRAM_BYTES];
    struct iovec vectors[SEND_BATCH];
    struct mmsghdr messages[SEND_BATCH];
    const int diag = getenv("JXS_UDP_DIAG") != NULL;
    unsigned int batch_count = 0;
    int tile;

    slot->send_start_ms = now_ms();
    if (diag)
        fprintf(stderr, "UDP_DIAG send_start frame=%llu fd=%d\n",
                (unsigned long long)slot->codec_slot.frame_id,
                pipeline->send_socket);
    for (tile = 0; tile < TILE_COUNT; tile++) {
        const uint32_t tile_bytes = (uint32_t)slot->codec_slot.jpeg_size[tile];
        const size_t fragments = jxs_udp_fragment_count(tile_bytes);
        size_t fragment;
        if (slot->codec_slot.jpeg_size[tile] > UINT32_MAX ||
            fragments == 0 || fragments > UINT16_MAX)
            return -1;
        for (fragment = 0; fragment < fragments; fragment++) {
            JxsUdpHeader header;
            const size_t offset = fragment * JXS_UDP_PAYLOAD_BYTES;
            const size_t remaining = tile_bytes - offset;
            const size_t payload_bytes = remaining < JXS_UDP_PAYLOAD_BYTES ?
                                         remaining : JXS_UDP_PAYLOAD_BYTES;
            const size_t datagram_bytes = JXS_UDP_HEADER_BYTES + payload_bytes;
            uint8_t *datagram = datagrams[batch_count];

            memset(&header, 0, sizeof(header));
            header.frame_id = slot->codec_slot.frame_id;
            header.tile_id = (uint8_t)tile;
            header.tile_bytes = tile_bytes;
            header.fragment_offset = (uint32_t)offset;
            header.fragment_index = (uint16_t)fragment;
            header.fragment_count = (uint16_t)fragments;
            header.payload_bytes = (uint16_t)payload_bytes;
            if (jxs_udp_header_encode(datagram, &header) != 0)
                return -1;
            memcpy(datagram + JXS_UDP_HEADER_BYTES,
                   slot->codec_slot.jpeg[tile] + offset, payload_bytes);
            if (inject_malformed && tile == 0 && fragment == 0)
                datagram[0] ^= 0xff;
            memset(&messages[batch_count], 0, sizeof(messages[batch_count]));
            vectors[batch_count].iov_base = datagram;
            vectors[batch_count].iov_len = datagram_bytes;
            messages[batch_count].msg_hdr.msg_iov = &vectors[batch_count];
            messages[batch_count].msg_hdr.msg_iovlen = 1;
            batch_count++;
            if (diag && tile == 0 && fragment == 0)
                fprintf(stderr, "UDP_DIAG first_packet frame=%llu bytes=%zu\n",
                        (unsigned long long)slot->codec_slot.frame_id, datagram_bytes);
            if (batch_count == SEND_BATCH ||
                (tile == TILE_COUNT - 1 && fragment == fragments - 1)) {
                unsigned int sent_count = 0;
                while (sent_count < batch_count) {
                    int sent = sendmmsg(pipeline->send_socket,
                                        messages + sent_count,
                                        batch_count - sent_count,
                                        MSG_NOSIGNAL);
                    unsigned int index;
                    if (sent < 0 && errno == EINTR)
                        continue;
                    if (sent <= 0) {
                        fprintf(stderr, "sendmmsg failed frame=%llu: %s\n",
                                (unsigned long long)slot->codec_slot.frame_id,
                                strerror(errno));
                        return -1;
                    }
                    for (index = sent_count;
                         index < sent_count + (unsigned int)sent; index++) {
                        if (messages[index].msg_len != vectors[index].iov_len) {
                            fprintf(stderr, "short UDP datagram frame=%llu\n",
                                    (unsigned long long)slot->codec_slot.frame_id);
                            return -1;
                        }
                        slot->datagrams_sent++;
                        slot->app_bytes_sent += messages[index].msg_len;
                    }
                    sent_count += (unsigned int)sent;
                }
                if (diag && slot->datagrams_sent == batch_count)
                    fprintf(stderr, "UDP_DIAG first_batch frame=%llu count=%u\n",
                            (unsigned long long)slot->codec_slot.frame_id,
                            batch_count);
                batch_count = 0;
            }
        }
    }
    slot->send_end_ms = now_ms();
    return 0;
}

static int udp_decode_assemble(UdpPipeline *pipeline, UdpFrameSlot *slot)
{
    int t, errors = 0;

#pragma omp parallel for schedule(static) num_threads(TILE_COUNT) reduction(+:errors)
    for (t = 0; t < TILE_COUNT; t++) {
        if (pin_current_thread(t) != 0)
            errors++;
        if (decoder_submit(&pipeline->decoders[t], slot->received_jpeg[t],
                           slot->reassembly[t].tile_bytes) != 0)
            errors++;
    }
    if (errors)
        return -1;
#pragma omp parallel for schedule(static) num_threads(TILE_COUNT) reduction(+:errors)
    for (t = 0; t < TILE_COUNT; t++) {
        if (pin_current_thread(t) != 0)
            errors++;
        if (decoder_wait_and_reclaim(&pipeline->decoders[t], t,
                                     pipeline->assembled_nv24) != 0)
            errors++;
    }
    return errors ? -1 : 0;
}

static int slot_tiles_complete(const UdpFrameSlot *slot)
{
    int tile;
    for (tile = 0; tile < TILE_COUNT; tile++)
        if (!slot->tile_complete[tile])
            return 0;
    return 1;
}

static int consume_complete_slot(UdpPipeline *pipeline, int expected)
{
    UdpFrameSlot *slot = &pipeline->slots[expected % pipeline->capacity];
    double decode_start, complete;
    uint64_t checksum = 0;
    int checksum_sampled = 0;

    pthread_mutex_lock(&pipeline->mutex);
    if (slot->codec_slot.state != SLOT_READY ||
        (int)slot->codec_slot.frame_id != expected ||
        !slot_tiles_complete(slot)) {
        pthread_mutex_unlock(&pipeline->mutex);
        return 0;
    }
    slot->codec_slot.state = SLOT_DECODING;
    pthread_mutex_unlock(&pipeline->mutex);

    decode_start = now_ms();
    if (udp_decode_assemble(pipeline, slot) != 0) {
        fprintf(stderr, "decode failed frame=%d\n", expected);
        udp_pipeline_fail(pipeline);
        return -1;
    }
    complete = now_ms();
    if (slot->codec_slot.measured_index >= 0 &&
        (slot->codec_slot.measured_index == 0 ||
         slot->codec_slot.measured_index == pipeline->frames - 1 ||
         (pipeline->checksum_every > 0 &&
          slot->codec_slot.measured_index % pipeline->checksum_every == 0))) {
        checksum = sparse_checksum(pipeline->assembled_nv24,
                                   (size_t)WIDTH * HEIGHT * 3);
        checksum_sampled = 1;
    }

    pthread_mutex_lock(&pipeline->mutex);
    if (slot->codec_slot.measured_index >= 0) {
        UdpFrameResult *result =
            &pipeline->results[slot->codec_slot.measured_index];
        result->encode_ms = slot->codec_slot.encode_end_ms -
                            slot->codec_slot.encode_start_ms;
        result->send_ms = slot->send_end_ms - slot->send_start_ms;
        result->network_ms = slot->receive_complete_ms -
                             slot->codec_slot.encode_end_ms;
        result->decode_assemble_ms = complete - decode_start;
        result->latency_ms = complete - slot->codec_slot.encode_start_ms;
        result->complete_ms = complete;
        result->datagrams_sent = slot->datagrams_sent;
        result->datagrams_received = slot->datagrams_received;
        result->app_bytes_sent = slot->app_bytes_sent;
        result->app_bytes_received = slot->app_bytes_received;
        result->checksum = checksum;
        result->checksum_sampled = checksum_sampled;
        pipeline->checksum_samples += checksum_sampled;
        if (slot->codec_slot.measured_index == 0)
            pipeline->first_checksum = checksum;
        if (slot->codec_slot.measured_index == pipeline->frames - 1)
            pipeline->last_checksum = checksum;
        if (checksum_sampled && pipeline->first_checksum &&
            checksum != pipeline->first_checksum)
            pipeline->checksum_mismatch = 1;
    }
    slot->codec_slot.state = SLOT_FREE;
    pipeline->inflight--;
    pipeline->completed++;
    pthread_cond_broadcast(&pipeline->changed);
    pthread_mutex_unlock(&pipeline->mutex);
    return 1;
}

static void *udp_decoder_thread(void *opaque)
{
    UdpPipeline *pipeline = opaque;
    int expected = 0;

    while (expected < pipeline->total_frames) {
        UdpFrameSlot *slot = &pipeline->slots[expected % pipeline->capacity];
        int consume_result;

        pthread_mutex_lock(&pipeline->mutex);
        while (!pipeline->failed &&
               (slot->codec_slot.state != SLOT_READY ||
                (int)slot->codec_slot.frame_id != expected ||
                !slot_tiles_complete(slot)))
            pthread_cond_wait(&pipeline->changed, &pipeline->mutex);
        if (pipeline->failed) {
            pthread_mutex_unlock(&pipeline->mutex);
            return NULL;
        }
        pthread_mutex_unlock(&pipeline->mutex);
        consume_result = consume_complete_slot(pipeline, expected);
        if (consume_result < 0)
            return NULL;
        if (consume_result == 0) {
            fprintf(stderr, "decoder wake without complete frame=%d\n", expected);
            udp_pipeline_fail(pipeline);
            return NULL;
        }
        expected++;
    }
    return NULL;
}

static void *udp_receiver_thread(void *opaque)
{
    UdpPipeline *pipeline = opaque;
    uint8_t datagram[JXS_UDP_DATAGRAM_BYTES + 1];
    const int diag = getenv("JXS_UDP_DIAG") != NULL;
    int received_frames = 0;

    if (diag)
        fprintf(stderr, "UDP_DIAG receiver_start fd=%d\n",
                pipeline->receive_socket);
    if (pin_current_thread(0) != 0)
        fprintf(stderr, "warning: could not pin UDP receiver to cpu0\n");
    while (received_frames < pipeline->total_frames) {
        JxsUdpHeader header;
        UdpFrameSlot *slot;
        ssize_t received;
        int push_result;
        received = recv(pipeline->receive_socket, datagram,
                        sizeof(datagram), 0);
        if (received < 0) {
            fprintf(stderr, "receive failed/timeout network_completed=%d: %s\n",
                    received_frames, strerror(errno));
            pipeline->transport_errors++;
            udp_pipeline_fail(pipeline);
            return NULL;
        }
        if (jxs_udp_header_decode(&header, datagram, (size_t)received) != 0) {
            fprintf(stderr, "malformed UDP datagram bytes=%zd network_completed=%d\n",
                    received, received_frames);
            pipeline->malformed_packets++;
            pipeline->transport_errors++;
            udp_pipeline_fail(pipeline);
            return NULL;
        }
        if (diag && header.tile_id == 0 && header.fragment_index == 0)
            fprintf(stderr, "UDP_DIAG first_receive frame=%llu bytes=%zd\n",
                    (unsigned long long)header.frame_id, received);
        slot = &pipeline->slots[header.frame_id % (uint64_t)pipeline->capacity];
        pthread_mutex_lock(&pipeline->mutex);
        if (slot->codec_slot.state != SLOT_READY ||
            slot->codec_slot.frame_id != header.frame_id ||
            header.frame_id < (uint64_t)pipeline->completed ||
            header.frame_id >=
                (uint64_t)(pipeline->completed + pipeline->capacity)) {
            pthread_mutex_unlock(&pipeline->mutex);
            fprintf(stderr, "unexpected UDP frame=%llu decoded=%d\n",
                    (unsigned long long)header.frame_id, pipeline->completed);
            pipeline->transport_errors++;
            udp_pipeline_fail(pipeline);
            return NULL;
        }
        pthread_mutex_unlock(&pipeline->mutex);
        if (!slot->tile_started[header.tile_id]) {
            if (jxs_udp_reassembly_reset(&slot->reassembly[header.tile_id],
                                         header.frame_id, header.tile_id,
                                         header.tile_bytes,
                                         slot->received_jpeg[header.tile_id],
                                         pipeline->jpeg_capacity,
                                         slot->seen[header.tile_id],
                                         jxs_udp_fragment_count(
                                             pipeline->jpeg_capacity)) != 0) {
                pipeline->transport_errors++;
                udp_pipeline_fail(pipeline);
                return NULL;
            }
            slot->tile_started[header.tile_id] = 1;
        }
        push_result = jxs_udp_reassembly_push(
            &slot->reassembly[header.tile_id], &header,
            datagram + JXS_UDP_HEADER_BYTES);
        if (push_result < 0) {
            fprintf(stderr, "reassembly failed frame=%llu tile=%u fragment=%u error=%d\n",
                    (unsigned long long)header.frame_id, header.tile_id,
                    header.fragment_index, push_result);
            if (push_result == JXS_UDP_ERR_CONFLICT)
                pipeline->conflicting_duplicates++;
            pipeline->transport_errors++;
            udp_pipeline_fail(pipeline);
            return NULL;
        }
        if (push_result == JXS_UDP_PUSH_DUPLICATE)
            pipeline->duplicate_packets++;
        else {
            slot->datagrams_received++;
            slot->app_bytes_received += (uint64_t)received;
        }
        if (jxs_udp_reassembly_complete(&slot->reassembly[header.tile_id])) {
            pthread_mutex_lock(&pipeline->mutex);
            slot->tile_complete[header.tile_id] = 1;
            if (slot_tiles_complete(slot) &&
                slot->receive_complete_ms == 0.0) {
                slot->receive_complete_ms = now_ms();
                pipeline->network_completed++;
                received_frames++;
                pthread_cond_broadcast(&pipeline->changed);
            }
            pthread_mutex_unlock(&pipeline->mutex);
        }
    }
    return NULL;
}

static void udp_wait_completed(UdpPipeline *pipeline, int count)
{
    pthread_mutex_lock(&pipeline->mutex);
    while (pipeline->completed < count && !pipeline->failed)
        pthread_cond_wait(&pipeline->changed, &pipeline->mutex);
    pthread_mutex_unlock(&pipeline->mutex);
}

static int write_udp_csv(const char *path, const UdpPipeline *pipeline)
{
    FILE *fp = fopen(path, "w");
    int i;
    if (!fp)
        return -1;
    fprintf(fp, "frame,encode_ms,send_ms,network_ms,decode_assemble_ms,latency_ms,schedule_late_ms,complete_ms,datagrams_sent,datagrams_received,app_bytes_sent,app_bytes_received,checksum,checksum_sampled\n");
    for (i = 0; i < pipeline->frames; i++) {
        const UdpFrameResult *r = &pipeline->results[i];
        fprintf(fp, "%d,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%llu,%llu,%llu,%llu,%016llx,%d\n",
                i, r->encode_ms, r->send_ms, r->network_ms,
                r->decode_assemble_ms, r->latency_ms,
                r->schedule_late_ms, r->complete_ms,
                (unsigned long long)r->datagrams_sent,
                (unsigned long long)r->datagrams_received,
                (unsigned long long)r->app_bytes_sent,
                (unsigned long long)r->app_bytes_received,
                (unsigned long long)r->checksum, r->checksum_sampled);
    }
    fclose(fp);
    return 0;
}

int main(int argc, char **argv)
{
    const char *input_path, *csv_path;
    int frames, warmups, quality, queue_capacity, port;
    double target_fps, frame_interval_ms;
    unsigned char *input = NULL;
    Encoder encoder;
    UdpPipeline pipeline;
    pthread_t receiver, decoder_thread_id, sender_thread_id;
    struct timespec pace_origin = {0};
    double *enc = NULL, *send_values = NULL, *network = NULL;
    double *dec = NULL, *lat = NULL, *late = NULL;
    double output_fps, first_complete, last_complete;
    double jpeg_bytes = 0.0;
    int inject_frame = -1;
    int frame_id, i, t;
    int ret = 1;

    if (argc != 9) {
        fprintf(stderr, "usage: %s INPUT.yuv444p FRAMES WARMUPS FPS QUALITY QUEUE_CAP PORT CSV\n",
                argv[0]);
        return 2;
    }
    input_path = argv[1];
    frames = atoi(argv[2]);
    warmups = atoi(argv[3]);
    target_fps = atof(argv[4]);
    quality = atoi(argv[5]);
    queue_capacity = atoi(argv[6]);
    port = atoi(argv[7]);
    csv_path = argv[8];
    if (frames < 2 || warmups < 0 || target_fps < 0.0 || quality < 1 ||
        quality > 100 || queue_capacity < 1 || queue_capacity > 8 ||
        port < 1024 || port > 65535) {
        fprintf(stderr, "invalid arguments\n");
        return 2;
    }
    if (getenv("JXS_UDP_INJECT_MALFORMED_FRAME"))
        inject_frame = atoi(getenv("JXS_UDP_INJECT_MALFORMED_FRAME"));
    frame_interval_ms = target_fps > 0.0 ? 1000.0 / target_fps : 0.0;
    input = read_input(input_path);
    if (!input || encoder_init(&encoder, input, quality) != 0)
        goto cleanup_input;
    if (udp_pipeline_init(&pipeline, queue_capacity, warmups, frames,
                          encoder.jpeg_capacity, port) != 0)
        goto cleanup_pipeline;
    pipeline.inject_frame = inject_frame;
    omp_set_dynamic(0);
    if (pthread_create(&decoder_thread_id, NULL, udp_decoder_thread,
                       &pipeline) != 0) {
        fprintf(stderr, "create UDP decoder thread failed\n");
        goto cleanup_pipeline;
    }
    if (pthread_create(&receiver, NULL, udp_receiver_thread, &pipeline) != 0) {
        fprintf(stderr, "create UDP receiver thread failed\n");
        udp_pipeline_fail(&pipeline);
        pthread_join(decoder_thread_id, NULL);
        goto cleanup_pipeline;
    }
    if (pthread_create(&sender_thread_id, NULL, udp_sender_thread,
                       &pipeline) != 0) {
        fprintf(stderr, "create UDP sender thread failed\n");
        udp_pipeline_fail(&pipeline);
        pthread_join(receiver, NULL);
        pthread_join(decoder_thread_id, NULL);
        goto cleanup_pipeline;
    }

    for (frame_id = 0; frame_id < pipeline.total_frames; frame_id++) {
        UdpFrameSlot *slot;
        double wait_ms;
        if (frame_id == warmups) {
            udp_wait_completed(&pipeline, warmups);
            pace_origin = monotonic_after_ms(20.0);
        }
        if (frame_id >= warmups && target_fps > 0.0) {
            struct timespec deadline = pace_origin;
            const long add_ns = (long)((frame_id - warmups) *
                                       frame_interval_ms * 1000000.0);
            deadline.tv_sec += add_ns / 1000000000L;
            deadline.tv_nsec += add_ns % 1000000000L;
            if (deadline.tv_nsec >= 1000000000L) {
                deadline.tv_sec++;
                deadline.tv_nsec -= 1000000000L;
            }
            sleep_until(&deadline);
        }
        if (udp_acquire_slot(&pipeline, frame_id, &slot, &wait_ms) != 0)
            break;
        if (frame_id >= warmups) {
            pipeline.producer_wait_ms += wait_ms;
            if (wait_ms > 0.1)
                pipeline.blocked_frames++;
        }
        slot->codec_slot.encode_start_ms = now_ms();
        if (frame_id >= warmups && target_fps > 0.0) {
            struct timespec now_ts;
            double now_clock_ms, deadline_ms;
            clock_gettime(CLOCK_MONOTONIC, &now_ts);
            now_clock_ms = now_ts.tv_sec * 1000.0 +
                           now_ts.tv_nsec / 1000000.0;
            deadline_ms = pace_origin.tv_sec * 1000.0 +
                          pace_origin.tv_nsec / 1000000.0 +
                          (frame_id - warmups) * frame_interval_ms;
            pipeline.results[frame_id - warmups].schedule_late_ms =
                fmax(0.0, now_clock_ms - deadline_ms);
        }
        if (encode_slot(&encoder, &slot->codec_slot) != 0) {
            udp_pipeline_fail(&pipeline);
            break;
        }
        slot->codec_slot.encode_end_ms = now_ms();
        if (frame_id == warmups)
            for (t = 0; t < TILE_COUNT; t++)
                jpeg_bytes += slot->codec_slot.jpeg_size[t];
        udp_publish_slot(&pipeline, slot);
    }
    udp_wait_completed(&pipeline, pipeline.total_frames);
    pthread_join(sender_thread_id, NULL);
    pthread_join(receiver, NULL);
    pthread_join(decoder_thread_id, NULL);
    if (pipeline.failed) {
        fprintf(stderr, "UDP pipeline failed transport_errors=%d malformed=%d\n",
                pipeline.transport_errors, pipeline.malformed_packets);
        goto cleanup_pipeline;
    }

    enc = malloc((size_t)frames * sizeof(*enc));
    send_values = malloc((size_t)frames * sizeof(*send_values));
    network = malloc((size_t)frames * sizeof(*network));
    dec = malloc((size_t)frames * sizeof(*dec));
    lat = malloc((size_t)frames * sizeof(*lat));
    late = malloc((size_t)frames * sizeof(*late));
    if (!enc || !send_values || !network || !dec || !lat || !late)
        goto cleanup_pipeline;
    for (i = 0; i < frames; i++) {
        UdpFrameResult *r = &pipeline.results[i];
        enc[i] = r->encode_ms;
        send_values[i] = r->send_ms;
        network[i] = r->network_ms;
        dec[i] = r->decode_assemble_ms;
        lat[i] = r->latency_ms;
        late[i] = r->schedule_late_ms;
        if (r->datagrams_sent != r->datagrams_received ||
            r->app_bytes_sent != r->app_bytes_received) {
            fprintf(stderr, "packet count mismatch frame=%d\n", i);
            pipeline.transport_errors++;
        }
    }
    first_complete = pipeline.results[0].complete_ms;
    last_complete = pipeline.results[frames - 1].complete_ms;
    output_fps = (frames - 1) * 1000.0 / (last_complete - first_complete);
    if (write_udp_csv(csv_path, &pipeline) != 0) {
        fprintf(stderr, "write csv failed: %s\n", csv_path);
        goto cleanup_pipeline;
    }
    printf("RESULT mode=%s frames=%d warmups=%d target_fps=%.3f quality=%d queue_capacity=%d "
           "jpeg_bytes=%.0f jpeg_payload_mbps_60=%.3f output_fps=%.3f "
           "encode_p50_ms=%.3f encode_p95_ms=%.3f encode_p99_ms=%.3f encode_max_ms=%.3f "
           "send_p50_ms=%.3f send_p95_ms=%.3f send_p99_ms=%.3f send_max_ms=%.3f "
           "network_p50_ms=%.3f network_p95_ms=%.3f network_p99_ms=%.3f network_max_ms=%.3f "
           "decode_assemble_p50_ms=%.3f decode_assemble_p95_ms=%.3f decode_assemble_p99_ms=%.3f decode_assemble_max_ms=%.3f "
           "latency_p50_ms=%.3f latency_p95_ms=%.3f latency_p99_ms=%.3f latency_max_ms=%.3f "
           "schedule_late_p99_ms=%.3f schedule_late_max_ms=%.3f max_inflight=%d "
           "blocked_frames=%d producer_wait_ms=%.3f transport_errors=%d malformed_packets=%d "
           "duplicate_packets=%llu conflicting_duplicates=%d checksum_every=%d checksum_samples=%d "
           "checksum_first=%016llx checksum_last=%016llx checksum_mismatch=%d format=NV24_444 "
           "socket_rcvbuf=%d socket_sndbuf=%d simulated_lan=1 physical_lan=0 csv=%s\n",
           target_fps > 0.0 ? "paced" : "saturated", frames, warmups,
           target_fps, quality, queue_capacity, jpeg_bytes,
           jpeg_bytes * 8.0 * 60.0 / 1000000.0, output_fps,
           percentile(enc, frames, 0.50), percentile(enc, frames, 0.95),
           percentile(enc, frames, 0.99), maximum(enc, frames),
           percentile(send_values, frames, 0.50),
           percentile(send_values, frames, 0.95),
           percentile(send_values, frames, 0.99), maximum(send_values, frames),
           percentile(network, frames, 0.50), percentile(network, frames, 0.95),
           percentile(network, frames, 0.99), maximum(network, frames),
           percentile(dec, frames, 0.50), percentile(dec, frames, 0.95),
           percentile(dec, frames, 0.99), maximum(dec, frames),
           percentile(lat, frames, 0.50), percentile(lat, frames, 0.95),
           percentile(lat, frames, 0.99), maximum(lat, frames),
           percentile(late, frames, 0.99), maximum(late, frames),
           pipeline.max_inflight, pipeline.blocked_frames,
           pipeline.producer_wait_ms, pipeline.transport_errors,
           pipeline.malformed_packets,
           (unsigned long long)pipeline.duplicate_packets,
           pipeline.conflicting_duplicates, pipeline.checksum_every,
           pipeline.checksum_samples,
           (unsigned long long)pipeline.first_checksum,
           (unsigned long long)pipeline.last_checksum,
           pipeline.checksum_mismatch, pipeline.receive_buffer_bytes,
           pipeline.send_buffer_bytes, csv_path);
    ret = pipeline.transport_errors || pipeline.checksum_mismatch ? 1 : 0;

cleanup_pipeline:
    free(late);
    free(lat);
    free(dec);
    free(network);
    free(send_values);
    free(enc);
    udp_pipeline_deinit(&pipeline);
    encoder_deinit(&encoder);
cleanup_input:
    free(input);
    return ret;
}
