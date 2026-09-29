/*
 * THROWAWAY FEASIBILITY PROBE — not product code.
 *
 * Measures a two-stage RK3588-only 4K YUV444 pipeline:
 *   four Cortex-A76 libjpeg-turbo encoders -> bounded queue ->
 *   four persistent Rockchip MPP MJPEG decoders -> full-frame NV24 assembly.
 */

#define _GNU_SOURCE
#define MODULE_TAG "jpeg444_pipeline_probe"

#include <errno.h>
#include <math.h>
#include <omp.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <turbojpeg.h>

#include "rk_mpi.h"
#include "mpp_buffer.h"
#include "mpp_dec_cfg.h"
#include "mpp_frame.h"
#include "mpp_packet.h"
#include "mpp_task.h"

enum {
    WIDTH = 3840,
    HEIGHT = 2160,
    TILE_COUNT = 4,
    TILE_HEIGHT = HEIGHT / TILE_COUNT,
};

typedef enum {
    SLOT_FREE = 0,
    SLOT_ENCODING,
    SLOT_READY,
    SLOT_DECODING,
} SlotState;

typedef struct {
    uint64_t frame_id;
    int measured_index;
    SlotState state;
    unsigned char *jpeg[TILE_COUNT];
    unsigned long jpeg_size[TILE_COUNT];
    double encode_start_ms;
    double encode_end_ms;
} FrameSlot;

typedef struct {
    double encode_ms;
    double queue_ms;
    double decode_assemble_ms;
    double latency_ms;
    double schedule_late_ms;
    double complete_ms;
    uint64_t checksum;
    int checksum_sampled;
} FrameResult;

typedef struct {
    MppCtx ctx;
    MppApi *mpi;
    MppDecCfg cfg;
    MppBufferGroup group;
    MppBuffer input_buffer;
    MppBuffer buffer;
    MppFrame frame;
    MppPacket packet;
    MppTask input_task;
    int submitted;
} Decoder;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    FrameSlot *slots;
    int capacity;
    int warmups;
    int frames;
    int total_frames;
    int next_consume;
    int completed;
    int producer_done;
    int failed;
    int max_inflight;
    int inflight;
    int blocked_frames;
    double producer_wait_ms;
    FrameResult *results;
    Decoder decoders[TILE_COUNT];
    unsigned char *assembled_nv24;
    uint64_t first_checksum;
    uint64_t last_checksum;
    int checksum_every;
    int checksum_samples;
    int checksum_mismatch;
} Pipeline;

typedef struct {
    const unsigned char *planes[TILE_COUNT][3];
    int strides[3];
    tjhandle compressors[TILE_COUNT];
    unsigned long jpeg_capacity;
    int quality;
} Encoder;

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

static struct timespec monotonic_after_ms(double delta_ms)
{
    struct timespec ts;
    long add_ns;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    add_ns = (long)(delta_ms * 1000000.0);
    ts.tv_sec += add_ns / 1000000000L;
    ts.tv_nsec += add_ns % 1000000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    return ts;
}

static void sleep_until(const struct timespec *deadline)
{
    int ret;
    do {
        ret = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, deadline, NULL);
    } while (ret == EINTR);
}

static int pin_current_thread(int cpu)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

static int cmp_double(const void *a, const void *b)
{
    const double da = *(const double *)a;
    const double db = *(const double *)b;
    return (da > db) - (da < db);
}

static double percentile(const double *values, int count, double p)
{
    double *copy;
    int index;
    double value;

    copy = malloc((size_t)count * sizeof(*copy));
    if (!copy)
        return NAN;
    memcpy(copy, values, (size_t)count * sizeof(*copy));
    qsort(copy, (size_t)count, sizeof(*copy), cmp_double);
    index = (int)ceil(p * count) - 1;
    if (index < 0)
        index = 0;
    if (index >= count)
        index = count - 1;
    value = copy[index];
    free(copy);
    return value;
}

static double maximum(const double *values, int count)
{
    double result = values[0];
    int i;
    for (i = 1; i < count; i++)
        if (values[i] > result)
            result = values[i];
    return result;
}

static uint64_t sparse_checksum(const unsigned char *data, size_t size)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    size_t i;
    for (i = 0; i < size; i += 4093) {
        hash ^= data[i];
        hash *= UINT64_C(1099511628211);
    }
    if (size) {
        hash ^= data[size - 1];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static unsigned char *read_input(const char *path)
{
    const size_t expected = (size_t)WIDTH * HEIGHT * 3;
    unsigned char *data = malloc(expected);
    FILE *fp;
    size_t got;

    if (!data)
        return NULL;
    fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "open %s failed: %s\n", path, strerror(errno));
        free(data);
        return NULL;
    }
    got = fread(data, 1, expected, fp);
    fclose(fp);
    if (got != expected) {
        fprintf(stderr, "input size mismatch: expected %zu got %zu\n", expected, got);
        free(data);
        return NULL;
    }
    return data;
}

static int encoder_init(Encoder *enc, const unsigned char *input, int quality)
{
    const size_t plane_size = (size_t)WIDTH * HEIGHT;
    int t, c;

    memset(enc, 0, sizeof(*enc));
    enc->quality = quality;
    enc->jpeg_capacity = tjBufSize(WIDTH, TILE_HEIGHT, TJSAMP_444);
    enc->strides[0] = enc->strides[1] = enc->strides[2] = WIDTH;
    for (t = 0; t < TILE_COUNT; t++) {
        const int y0 = t * TILE_HEIGHT;
        for (c = 0; c < 3; c++)
            enc->planes[t][c] = input + (size_t)c * plane_size + (size_t)y0 * WIDTH;
        enc->compressors[t] = tjInitCompress();
        if (!enc->compressors[t])
            return -1;
    }
    return 0;
}

static void encoder_deinit(Encoder *enc)
{
    int t;
    for (t = 0; t < TILE_COUNT; t++)
        if (enc->compressors[t])
            tjDestroy(enc->compressors[t]);
}

static int encode_slot(Encoder *enc, FrameSlot *slot)
{
    int errors = 0;
    int t;

#pragma omp parallel for schedule(static) num_threads(TILE_COUNT) reduction(+:errors)
    for (t = 0; t < TILE_COUNT; t++) {
        static _Thread_local int pinned;
        if (!pinned) {
            if (pin_current_thread(4 + t) != 0)
                errors++;
            else
                pinned = 1;
        }
        slot->jpeg_size[t] = enc->jpeg_capacity;
        if (tjCompressFromYUVPlanes(enc->compressors[t], enc->planes[t], WIDTH,
                                    enc->strides, TILE_HEIGHT, TJSAMP_444,
                                    &slot->jpeg[t], &slot->jpeg_size[t],
                                    enc->quality,
                                    TJFLAG_FASTDCT | TJFLAG_NOREALLOC) != 0) {
            fprintf(stderr, "encode tile %d failed: %s\n", t,
                    tjGetErrorStr2(enc->compressors[t]));
            errors++;
        }
    }
    return errors ? -1 : 0;
}

static int decoder_init(Decoder *dec, size_t jpeg_capacity)
{
    const int h_stride = (WIDTH + 15) & ~15;
    const int v_stride = (TILE_HEIGHT + 15) & ~15;
    const size_t buffer_size = (size_t)h_stride * v_stride * 4;
    MPP_RET ret;

    memset(dec, 0, sizeof(*dec));
    ret = mpp_frame_init(&dec->frame);
    if (ret)
        return -1;
    ret = mpp_buffer_group_get_internal(&dec->group,
                                        MPP_BUFFER_TYPE_DRM |
                                        MPP_BUFFER_FLAGS_CACHABLE);
    if (ret)
        return -1;
    ret = mpp_buffer_get(dec->group, &dec->input_buffer, jpeg_capacity);
    if (ret)
        return -1;
    ret = mpp_buffer_get(dec->group, &dec->buffer, buffer_size);
    if (ret)
        return -1;
    mpp_frame_set_buffer(dec->frame, dec->buffer);

    ret = mpp_create(&dec->ctx, &dec->mpi);
    if (ret)
        return -1;
    ret = mpp_init(dec->ctx, MPP_CTX_DEC, MPP_VIDEO_CodingMJPEG);
    if (ret)
        return -1;

    ret = mpp_dec_cfg_init(&dec->cfg);
    if (ret)
        return -1;
    ret = dec->mpi->control(dec->ctx, MPP_DEC_GET_CFG, dec->cfg);
    if (ret)
        return -1;
    ret = mpp_dec_cfg_set_u32(dec->cfg, "base:split_parse", 1);
    if (ret)
        return -1;
    ret = dec->mpi->control(dec->ctx, MPP_DEC_SET_CFG, dec->cfg);
    if (ret)
        return -1;
    return 0;
}

static void decoder_deinit(Decoder *dec)
{
    if (dec->packet)
        mpp_packet_deinit(&dec->packet);
    if (dec->ctx) {
        dec->mpi->reset(dec->ctx);
        mpp_destroy(dec->ctx);
    }
    if (dec->frame)
        mpp_frame_deinit(&dec->frame);
    if (dec->buffer)
        mpp_buffer_put(dec->buffer);
    if (dec->input_buffer)
        mpp_buffer_put(dec->input_buffer);
    if (dec->group)
        mpp_buffer_group_put(dec->group);
    if (dec->cfg)
        mpp_dec_cfg_deinit(dec->cfg);
    memset(dec, 0, sizeof(*dec));
}

static int decoder_submit(Decoder *dec, unsigned char *jpeg, unsigned long size)
{
    MppTask task = NULL;
    unsigned char *input;
    MPP_RET ret;

    if (size > mpp_buffer_get_size(dec->input_buffer))
        return -1;
    input = mpp_buffer_get_ptr(dec->input_buffer);
    if (!input)
        return -1;
    mpp_buffer_sync_begin(dec->input_buffer);
    memcpy(input, jpeg, size);
    mpp_buffer_sync_end(dec->input_buffer);

    ret = mpp_packet_init_with_buffer(&dec->packet, dec->input_buffer);
    if (ret)
        return -1;
    mpp_packet_set_pos(dec->packet, input);
    mpp_packet_set_size(dec->packet, size);
    mpp_packet_set_length(dec->packet, size);
    mpp_packet_set_eos(dec->packet);
    ret = dec->mpi->poll(dec->ctx, MPP_PORT_INPUT, MPP_POLL_BLOCK);
    if (ret)
        return -1;
    ret = dec->mpi->dequeue(dec->ctx, MPP_PORT_INPUT, &task);
    if (ret || !task)
        return -1;
    mpp_task_meta_set_packet(task, KEY_INPUT_PACKET, dec->packet);
    mpp_task_meta_set_frame(task, KEY_OUTPUT_FRAME, dec->frame);
    ret = dec->mpi->enqueue(dec->ctx, MPP_PORT_INPUT, task);
    if (ret)
        return -1;
    dec->input_task = task;
    dec->submitted = 1;
    return 0;
}

static int copy_tile_to_assembled(Decoder *dec, int tile,
                                  unsigned char *assembled)
{
    const int width = mpp_frame_get_width(dec->frame);
    const int height = mpp_frame_get_height(dec->frame);
    const int h_stride = mpp_frame_get_hor_stride(dec->frame);
    const int v_stride = mpp_frame_get_ver_stride(dec->frame);
    const MppFrameFormat fmt = mpp_frame_get_fmt(dec->frame);
    const int y0 = tile * TILE_HEIGHT;
    const size_t full_y_size = (size_t)WIDTH * HEIGHT;
    unsigned char *base;
    unsigned char *src_y;
    unsigned char *src_uv;
    unsigned char *dst_y;
    unsigned char *dst_uv;
    int row;

    if (width != WIDTH || height != TILE_HEIGHT ||
        (fmt & MPP_FRAME_FMT_MASK) != MPP_FMT_YUV444SP ||
        h_stride < WIDTH || v_stride < TILE_HEIGHT) {
        fprintf(stderr, "tile %d bad output w=%d h=%d stride=%dx%d fmt=%d\n",
                tile, width, height, h_stride, v_stride,
                fmt & MPP_FRAME_FMT_MASK);
        return -1;
    }
    base = mpp_buffer_get_ptr(dec->buffer);
    if (!base)
        return -1;
    src_y = base;
    src_uv = base + (size_t)h_stride * v_stride;
    dst_y = assembled + (size_t)y0 * WIDTH;
    dst_uv = assembled + full_y_size + (size_t)y0 * WIDTH * 2;

    mpp_buffer_sync_ro_begin(dec->buffer);
    if (h_stride == WIDTH) {
        memcpy(dst_y, src_y, (size_t)WIDTH * TILE_HEIGHT);
        memcpy(dst_uv, src_uv, (size_t)WIDTH * TILE_HEIGHT * 2);
    } else {
        for (row = 0; row < TILE_HEIGHT; row++) {
            memcpy(dst_y + (size_t)row * WIDTH,
                   src_y + (size_t)row * h_stride, WIDTH);
            memcpy(dst_uv + (size_t)row * WIDTH * 2,
                   src_uv + (size_t)row * h_stride * 2, WIDTH * 2);
        }
    }
    mpp_buffer_sync_ro_end(dec->buffer);
    return 0;
}

static int decoder_wait_and_reclaim(Decoder *dec, int tile,
                                    unsigned char *assembled)
{
    MppTask task = NULL;
    MppFrame frame_out = NULL;
    MppPacket packet_out = NULL;
    MPP_RET ret;
    int failed = 0;
    const int diag = getenv("JXS_DIAG") != NULL;
    double t0 = now_ms(), t_poll, t_dequeue, t_copy, t_output, t_input;

    if (!dec->submitted)
        return -1;
    ret = dec->mpi->poll(dec->ctx, MPP_PORT_OUTPUT, MPP_POLL_BLOCK);
    if (ret)
        return -1;
    t_poll = now_ms();
    ret = dec->mpi->dequeue(dec->ctx, MPP_PORT_OUTPUT, &task);
    if (ret || !task)
        return -1;
    t_dequeue = now_ms();
    mpp_task_meta_get_frame(task, KEY_OUTPUT_FRAME, &frame_out);
    if (!frame_out || mpp_frame_get_errinfo(frame_out) ||
        mpp_frame_get_discard(frame_out)) {
        fprintf(stderr, "tile %d decode error err=%u discard=%u\n", tile,
                frame_out ? mpp_frame_get_errinfo(frame_out) : 0,
                frame_out ? mpp_frame_get_discard(frame_out) : 0);
        failed = 1;
    } else if (copy_tile_to_assembled(dec, tile, assembled) != 0) {
        failed = 1;
    }
    t_copy = now_ms();
    ret = dec->mpi->enqueue(dec->ctx, MPP_PORT_OUTPUT, task);
    if (ret)
        failed = 1;
    t_output = now_ms();

    ret = dec->mpi->dequeue(dec->ctx, MPP_PORT_INPUT, &task);
    if (ret || !task) {
        failed = 1;
    } else {
        mpp_task_meta_get_packet(task, KEY_INPUT_PACKET, &packet_out);
        if (!packet_out || packet_out != dec->packet)
            failed = 1;
        if (packet_out)
            mpp_packet_deinit(&packet_out);
        dec->packet = NULL;
        ret = dec->mpi->enqueue(dec->ctx, MPP_PORT_INPUT, task);
        if (ret)
            failed = 1;
    }
    t_input = now_ms();
    if (diag)
        fprintf(stderr,
                "DIAG tile=%d poll_ms=%.3f dequeue_ms=%.3f copy_ms=%.3f "
                "output_enqueue_ms=%.3f input_reclaim_ms=%.3f total_ms=%.3f\n",
                tile, t_poll - t0, t_dequeue - t_poll, t_copy - t_dequeue,
                t_output - t_copy, t_input - t_output, t_input - t0);
    dec->submitted = 0;
    return failed ? -1 : 0;
}

static int decode_assemble(Pipeline *pipeline, FrameSlot *slot)
{
    int t, errors = 0;
    const int diag = getenv("JXS_DIAG") != NULL;
    double start = now_ms();

#pragma omp parallel for schedule(static) num_threads(TILE_COUNT) reduction(+:errors)
    for (t = 0; t < TILE_COUNT; t++)
    {
        if (pin_current_thread(t) != 0)
            errors++;
        if (decoder_submit(&pipeline->decoders[t], slot->jpeg[t],
                           slot->jpeg_size[t]) != 0)
            errors++;
    }
    if (errors)
        return -1;
    if (diag)
        fprintf(stderr, "DIAG frame=%llu submit_all_ms=%.3f\n",
                (unsigned long long)slot->frame_id, now_ms() - start);

#pragma omp parallel for schedule(static) num_threads(TILE_COUNT) reduction(+:errors)
    for (t = 0; t < TILE_COUNT; t++)
    {
        if (pin_current_thread(t) != 0)
            errors++;
        if (decoder_wait_and_reclaim(&pipeline->decoders[t], t,
                                     pipeline->assembled_nv24) != 0)
            errors++;
    }
    return errors ? -1 : 0;
}

static void pipeline_fail(Pipeline *pipeline)
{
    pthread_mutex_lock(&pipeline->mutex);
    pipeline->failed = 1;
    pthread_cond_broadcast(&pipeline->changed);
    pthread_mutex_unlock(&pipeline->mutex);
}

static void *decoder_thread(void *opaque)
{
    Pipeline *pipeline = opaque;
    int expected = 0;

    if (pin_current_thread(0) != 0)
        fprintf(stderr, "warning: could not pin decoder coordinator to cpu0\n");

    while (expected < pipeline->total_frames) {
        FrameSlot *slot;
        double decode_start;
        double complete;
        uint64_t checksum = 0;
        int checksum_sampled = 0;

        pthread_mutex_lock(&pipeline->mutex);
        slot = &pipeline->slots[expected % pipeline->capacity];
        while (slot->state != SLOT_READY && !pipeline->failed)
            pthread_cond_wait(&pipeline->changed, &pipeline->mutex);
        if (pipeline->failed) {
            pthread_mutex_unlock(&pipeline->mutex);
            return NULL;
        }
        if ((int)slot->frame_id != expected) {
            fprintf(stderr, "frame order mismatch expected=%d got=%llu\n",
                    expected, (unsigned long long)slot->frame_id);
            pthread_mutex_unlock(&pipeline->mutex);
            pipeline_fail(pipeline);
            return NULL;
        }
        slot->state = SLOT_DECODING;
        pthread_mutex_unlock(&pipeline->mutex);

        decode_start = now_ms();
        if (decode_assemble(pipeline, slot) != 0) {
            pipeline_fail(pipeline);
            return NULL;
        }
        complete = now_ms();
        if (slot->measured_index >= 0 &&
            (slot->measured_index == 0 ||
             slot->measured_index == pipeline->frames - 1 ||
             (pipeline->checksum_every > 0 &&
              slot->measured_index % pipeline->checksum_every == 0))) {
            checksum = sparse_checksum(pipeline->assembled_nv24,
                                       (size_t)WIDTH * HEIGHT * 3);
            checksum_sampled = 1;
        }

        pthread_mutex_lock(&pipeline->mutex);
        if (slot->measured_index >= 0) {
            FrameResult *result = &pipeline->results[slot->measured_index];
            result->encode_ms = slot->encode_end_ms - slot->encode_start_ms;
            result->queue_ms = decode_start - slot->encode_end_ms;
            result->decode_assemble_ms = complete - decode_start;
            result->latency_ms = complete - slot->encode_start_ms;
            result->complete_ms = complete;
            result->checksum = checksum;
            result->checksum_sampled = checksum_sampled;
            pipeline->checksum_samples += checksum_sampled;
            if (slot->measured_index == 0)
                pipeline->first_checksum = checksum;
            if (slot->measured_index == pipeline->frames - 1)
                pipeline->last_checksum = checksum;
            if (checksum_sampled && pipeline->first_checksum &&
                checksum != pipeline->first_checksum)
                pipeline->checksum_mismatch = 1;
        }
        slot->state = SLOT_FREE;
        pipeline->inflight--;
        pipeline->completed++;
        expected++;
        pthread_cond_broadcast(&pipeline->changed);
        pthread_mutex_unlock(&pipeline->mutex);
    }
    return NULL;
}

static int pipeline_init(Pipeline *pipeline, int capacity, int warmups,
                         int frames, unsigned long jpeg_capacity)
{
    int i, t;
    memset(pipeline, 0, sizeof(*pipeline));
    pipeline->capacity = capacity;
    pipeline->warmups = warmups;
    pipeline->frames = frames;
    pipeline->total_frames = warmups + frames;
    pipeline->checksum_every = 1;
    if (getenv("JXS_CHECKSUM_EVERY")) {
        pipeline->checksum_every = atoi(getenv("JXS_CHECKSUM_EVERY"));
        if (pipeline->checksum_every < 0)
            pipeline->checksum_every = 0;
    }
    pipeline->slots = calloc((size_t)capacity, sizeof(*pipeline->slots));
    pipeline->results = calloc((size_t)frames, sizeof(*pipeline->results));
    pipeline->assembled_nv24 = malloc((size_t)WIDTH * HEIGHT * 3);
    if (!pipeline->slots || !pipeline->results || !pipeline->assembled_nv24)
        return -1;
    pthread_mutex_init(&pipeline->mutex, NULL);
    pthread_cond_init(&pipeline->changed, NULL);
    for (i = 0; i < capacity; i++) {
        for (t = 0; t < TILE_COUNT; t++) {
            pipeline->slots[i].jpeg[t] = tjAlloc((int)jpeg_capacity);
            if (!pipeline->slots[i].jpeg[t])
                return -1;
        }
    }
    for (t = 0; t < TILE_COUNT; t++)
        if (decoder_init(&pipeline->decoders[t], jpeg_capacity) != 0)
            return -1;
    return 0;
}

static void pipeline_deinit(Pipeline *pipeline)
{
    int i, t;
    for (t = 0; t < TILE_COUNT; t++)
        decoder_deinit(&pipeline->decoders[t]);
    if (pipeline->slots) {
        for (i = 0; i < pipeline->capacity; i++)
            for (t = 0; t < TILE_COUNT; t++)
                if (pipeline->slots[i].jpeg[t])
                    tjFree(pipeline->slots[i].jpeg[t]);
    }
    pthread_cond_destroy(&pipeline->changed);
    pthread_mutex_destroy(&pipeline->mutex);
    free(pipeline->assembled_nv24);
    free(pipeline->results);
    free(pipeline->slots);
}

static int acquire_slot(Pipeline *pipeline, int frame_id, FrameSlot **out,
                        double *wait_ms)
{
    FrameSlot *slot = &pipeline->slots[frame_id % pipeline->capacity];
    double start = now_ms();

    pthread_mutex_lock(&pipeline->mutex);
    while (slot->state != SLOT_FREE && !pipeline->failed)
        pthread_cond_wait(&pipeline->changed, &pipeline->mutex);
    *wait_ms = now_ms() - start;
    if (pipeline->failed) {
        pthread_mutex_unlock(&pipeline->mutex);
        return -1;
    }
    slot->state = SLOT_ENCODING;
    slot->frame_id = (uint64_t)frame_id;
    slot->measured_index = frame_id - pipeline->warmups;
    pthread_mutex_unlock(&pipeline->mutex);
    *out = slot;
    return 0;
}

static void publish_slot(Pipeline *pipeline, FrameSlot *slot)
{
    pthread_mutex_lock(&pipeline->mutex);
    slot->state = SLOT_READY;
    pipeline->inflight++;
    if (pipeline->inflight > pipeline->max_inflight)
        pipeline->max_inflight = pipeline->inflight;
    pthread_cond_broadcast(&pipeline->changed);
    pthread_mutex_unlock(&pipeline->mutex);
}

static void wait_completed(Pipeline *pipeline, int count)
{
    pthread_mutex_lock(&pipeline->mutex);
    while (pipeline->completed < count && !pipeline->failed)
        pthread_cond_wait(&pipeline->changed, &pipeline->mutex);
    pthread_mutex_unlock(&pipeline->mutex);
}

static int write_csv(const char *path, const Pipeline *pipeline)
{
    FILE *fp = fopen(path, "w");
    int i;
    if (!fp)
        return -1;
    fprintf(fp, "frame,encode_ms,queue_ms,decode_assemble_ms,latency_ms,schedule_late_ms,complete_ms,checksum,checksum_sampled\n");
    for (i = 0; i < pipeline->frames; i++) {
        const FrameResult *r = &pipeline->results[i];
        fprintf(fp, "%d,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%016llx,%d\n",
                i, r->encode_ms, r->queue_ms, r->decode_assemble_ms,
                r->latency_ms, r->schedule_late_ms, r->complete_ms,
                (unsigned long long)r->checksum, r->checksum_sampled);
    }
    fclose(fp);
    return 0;
}

int main(int argc, char **argv)
{
    const char *input_path;
    const char *csv_path;
    int frames, warmups, quality, queue_capacity;
    double target_fps;
    double frame_interval_ms;
    unsigned char *input = NULL;
    Encoder encoder;
    Pipeline pipeline;
    pthread_t consumer;
    struct timespec pace_origin = {0};
    double *enc = NULL, *dec = NULL, *lat = NULL, *queue = NULL, *late = NULL;
    double output_fps;
    double first_complete, last_complete;
    double jpeg_bytes = 0.0;
    int frame_id, i, t;
    int ret = 1;

    if (argc != 8) {
        fprintf(stderr, "usage: %s INPUT.yuv444p FRAMES WARMUPS FPS QUALITY QUEUE_CAP CSV\n", argv[0]);
        return 2;
    }
    input_path = argv[1];
    frames = atoi(argv[2]);
    warmups = atoi(argv[3]);
    target_fps = atof(argv[4]);
    quality = atoi(argv[5]);
    queue_capacity = atoi(argv[6]);
    csv_path = argv[7];
    if (frames < 2 || warmups < 0 || target_fps < 0.0 || quality < 1 ||
        quality > 100 || queue_capacity < 1 || queue_capacity > 8) {
        fprintf(stderr, "invalid arguments\n");
        return 2;
    }
    frame_interval_ms = target_fps > 0.0 ? 1000.0 / target_fps : 0.0;

    input = read_input(input_path);
    if (!input || encoder_init(&encoder, input, quality) != 0)
        goto cleanup_input;
    if (pipeline_init(&pipeline, queue_capacity, warmups, frames,
                      encoder.jpeg_capacity) != 0)
        goto cleanup_encoder;
    omp_set_dynamic(0);

    if (pthread_create(&consumer, NULL, decoder_thread, &pipeline) != 0) {
        fprintf(stderr, "create decoder thread failed\n");
        goto cleanup_pipeline;
    }

    for (frame_id = 0; frame_id < pipeline.total_frames; frame_id++) {
        FrameSlot *slot;
        double wait_ms;

        if (frame_id == warmups) {
            wait_completed(&pipeline, warmups);
            pace_origin = monotonic_after_ms(20.0);
        }
        if (frame_id >= warmups && target_fps > 0.0) {
            struct timespec deadline = pace_origin;
            const long add_ns = (long)((frame_id - warmups) * frame_interval_ms * 1000000.0);
            deadline.tv_sec += add_ns / 1000000000L;
            deadline.tv_nsec += add_ns % 1000000000L;
            if (deadline.tv_nsec >= 1000000000L) {
                deadline.tv_sec++;
                deadline.tv_nsec -= 1000000000L;
            }
            sleep_until(&deadline);
        }
        if (acquire_slot(&pipeline, frame_id, &slot, &wait_ms) != 0)
            break;
        if (frame_id >= warmups) {
            pipeline.producer_wait_ms += wait_ms;
            if (wait_ms > 0.1)
                pipeline.blocked_frames++;
        }
        slot->encode_start_ms = now_ms();
        if (frame_id >= warmups && target_fps > 0.0) {
            struct timespec now_ts;
            double now_clock_ms, deadline_ms;
            clock_gettime(CLOCK_MONOTONIC, &now_ts);
            now_clock_ms = now_ts.tv_sec * 1000.0 + now_ts.tv_nsec / 1000000.0;
            deadline_ms = pace_origin.tv_sec * 1000.0 + pace_origin.tv_nsec / 1000000.0 +
                          (frame_id - warmups) * frame_interval_ms;
            pipeline.results[frame_id - warmups].schedule_late_ms =
                fmax(0.0, now_clock_ms - deadline_ms);
        }
        if (encode_slot(&encoder, slot) != 0) {
            pipeline_fail(&pipeline);
            break;
        }
        slot->encode_end_ms = now_ms();
        if (frame_id == warmups) {
            for (t = 0; t < TILE_COUNT; t++)
                jpeg_bytes += slot->jpeg_size[t];
        }
        publish_slot(&pipeline, slot);
    }

    pthread_mutex_lock(&pipeline.mutex);
    pipeline.producer_done = 1;
    pthread_cond_broadcast(&pipeline.changed);
    pthread_mutex_unlock(&pipeline.mutex);
    wait_completed(&pipeline, pipeline.total_frames);
    pthread_join(consumer, NULL);
    if (pipeline.failed) {
        fprintf(stderr, "pipeline failed\n");
        goto cleanup_pipeline;
    }

    enc = malloc((size_t)frames * sizeof(*enc));
    dec = malloc((size_t)frames * sizeof(*dec));
    lat = malloc((size_t)frames * sizeof(*lat));
    queue = malloc((size_t)frames * sizeof(*queue));
    late = malloc((size_t)frames * sizeof(*late));
    if (!enc || !dec || !lat || !queue || !late)
        goto cleanup_pipeline;
    for (i = 0; i < frames; i++) {
        enc[i] = pipeline.results[i].encode_ms;
        dec[i] = pipeline.results[i].decode_assemble_ms;
        lat[i] = pipeline.results[i].latency_ms;
        queue[i] = pipeline.results[i].queue_ms;
        late[i] = pipeline.results[i].schedule_late_ms;
    }
    first_complete = pipeline.results[0].complete_ms;
    last_complete = pipeline.results[frames - 1].complete_ms;
    output_fps = (frames - 1) * 1000.0 / (last_complete - first_complete);

    if (write_csv(csv_path, &pipeline) != 0) {
        fprintf(stderr, "write csv failed: %s\n", csv_path);
        goto cleanup_pipeline;
    }

    printf("RESULT mode=%s frames=%d warmups=%d target_fps=%.3f quality=%d queue_capacity=%d "
           "jpeg_bytes=%.0f payload_mbps_60=%.3f output_fps=%.3f "
           "encode_p50_ms=%.3f encode_p95_ms=%.3f encode_p99_ms=%.3f encode_max_ms=%.3f "
           "decode_assemble_p50_ms=%.3f decode_assemble_p95_ms=%.3f decode_assemble_p99_ms=%.3f decode_assemble_max_ms=%.3f "
           "queue_p50_ms=%.3f queue_p95_ms=%.3f queue_p99_ms=%.3f queue_max_ms=%.3f "
           "latency_p50_ms=%.3f latency_p95_ms=%.3f latency_p99_ms=%.3f latency_max_ms=%.3f "
           "schedule_late_p99_ms=%.3f schedule_late_max_ms=%.3f "
           "max_inflight=%d blocked_frames=%d producer_wait_ms=%.3f drops=0 decode_errors=0 "
           "checksum_every=%d checksum_samples=%d checksum_first=%016llx checksum_last=%016llx "
           "checksum_mismatch=%d format=NV24_444 csv=%s\n",
           target_fps > 0.0 ? "paced" : "saturated", frames, warmups,
           target_fps, quality, queue_capacity, jpeg_bytes,
           jpeg_bytes * 8.0 * 60.0 / 1000000.0, output_fps,
           percentile(enc, frames, 0.50), percentile(enc, frames, 0.95),
           percentile(enc, frames, 0.99), maximum(enc, frames),
           percentile(dec, frames, 0.50), percentile(dec, frames, 0.95),
           percentile(dec, frames, 0.99), maximum(dec, frames),
           percentile(queue, frames, 0.50), percentile(queue, frames, 0.95),
           percentile(queue, frames, 0.99), maximum(queue, frames),
           percentile(lat, frames, 0.50), percentile(lat, frames, 0.95),
           percentile(lat, frames, 0.99), maximum(lat, frames),
           percentile(late, frames, 0.99), maximum(late, frames),
           pipeline.max_inflight, pipeline.blocked_frames,
           pipeline.producer_wait_ms,
           pipeline.checksum_every, pipeline.checksum_samples,
           (unsigned long long)pipeline.first_checksum,
           (unsigned long long)pipeline.last_checksum,
           pipeline.checksum_mismatch, csv_path);
    ret = pipeline.checksum_mismatch ? 1 : 0;

cleanup_pipeline:
    free(late);
    free(queue);
    free(lat);
    free(dec);
    free(enc);
    pipeline_deinit(&pipeline);
cleanup_encoder:
    encoder_deinit(&encoder);
cleanup_input:
    free(input);
    return ret;
}
