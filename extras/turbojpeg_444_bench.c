#define _GNU_SOURCE

#include <errno.h>
#include <math.h>
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <turbojpeg.h>

typedef struct {
    int y0;
    int height;
    const unsigned char *src[3];
    unsigned char *dst[3];
    int stride[3];
    tjhandle comp;
    tjhandle dec;
    unsigned char *jpeg;
    unsigned long capacity;
    unsigned long size;
    int error;
} Tile;

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

static int cmp_double(const void *a, const void *b)
{
    double da = *(const double *)a;
    double db = *(const double *)b;
    return (da > db) - (da < db);
}

static double percentile(const double *values, int count, double p)
{
    double *copy = malloc((size_t)count * sizeof(*copy));
    int index;
    if (!copy)
        return -1.0;
    memcpy(copy, values, (size_t)count * sizeof(*copy));
    qsort(copy, (size_t)count, sizeof(*copy), cmp_double);
    index = (int)ceil(p * count) - 1;
    if (index < 0)
        index = 0;
    if (index >= count)
        index = count - 1;
    p = copy[index];
    free(copy);
    return p;
}

static unsigned char *read_file(const char *path, size_t expected)
{
    FILE *fp = fopen(path, "rb");
    unsigned char *buf;
    size_t got;
    if (!fp) {
        fprintf(stderr, "open %s failed: %s\n", path, strerror(errno));
        return NULL;
    }
    buf = malloc(expected);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    got = fread(buf, 1, expected, fp);
    fclose(fp);
    if (got != expected) {
        fprintf(stderr, "read %s: expected %zu, got %zu\n", path, expected, got);
        free(buf);
        return NULL;
    }
    return buf;
}

static void run_encode(Tile *tiles, int tile_count, int width, int quality, int flags)
{
    int t;
#pragma omp parallel for schedule(static) num_threads(tile_count)
    for (t = 0; t < tile_count; t++) {
        Tile *tile = &tiles[t];
        tile->size = tile->capacity;
        if (tjCompressFromYUVPlanes(tile->comp, tile->src, width, tile->stride,
                                    tile->height, TJSAMP_444, &tile->jpeg,
                                    &tile->size, quality,
                                    flags | TJFLAG_NOREALLOC) != 0) {
            fprintf(stderr, "encode tile %d: %s\n", t, tjGetErrorStr2(tile->comp));
            tile->error = 1;
        }
    }
}

static void run_decode(Tile *tiles, int tile_count, int width, int flags)
{
    int t;
#pragma omp parallel for schedule(static) num_threads(tile_count)
    for (t = 0; t < tile_count; t++) {
        Tile *tile = &tiles[t];
        if (tjDecompressToYUVPlanes(tile->dec, tile->jpeg, tile->size,
                                    tile->dst, width, tile->stride,
                                    tile->height, flags) != 0) {
            fprintf(stderr, "decode tile %d: %s\n", t, tjGetErrorStr2(tile->dec));
            tile->error = 1;
        }
    }
}

int main(int argc, char **argv)
{
    const int width = 3840;
    const int height = 2160;
    int tile_count;
    int quality;
    int iterations;
    int warmups;
    int flags = TJFLAG_FASTDCT;
    int encode_only = getenv("JXS_ENCODE_ONLY") != NULL;
    size_t plane_size = (size_t)width * height;
    size_t frame_size = plane_size * 3;
    unsigned char *input;
    unsigned char *decoded;
    Tile *tiles;
    double *enc_ms;
    double *dec_ms;
    double *total_ms;
    unsigned long jpeg_bytes = 0;
    double squared_error[3] = {0.0, 0.0, 0.0};
    double absolute_error[3] = {0.0, 0.0, 0.0};
    int i, t, c;
    int failed = 0;

    if (argc != 6 && argc != 7) {
        fprintf(stderr, "usage: %s INPUT.yuv444p TILES QUALITY ITERATIONS WARMUPS [OUTPUT_PREFIX]\n", argv[0]);
        return 2;
    }
    tile_count = atoi(argv[2]);
    quality = atoi(argv[3]);
    iterations = atoi(argv[4]);
    warmups = atoi(argv[5]);
    if (tile_count < 1 || tile_count > 8 || quality < 1 || quality > 100 ||
        iterations < 1 || warmups < 0) {
        fprintf(stderr, "invalid arguments\n");
        return 2;
    }

    input = read_file(argv[1], frame_size);
    decoded = calloc(1, frame_size);
    tiles = calloc((size_t)tile_count, sizeof(*tiles));
    enc_ms = calloc((size_t)iterations, sizeof(*enc_ms));
    dec_ms = calloc((size_t)iterations, sizeof(*dec_ms));
    total_ms = calloc((size_t)iterations, sizeof(*total_ms));
    if (!input || !decoded || !tiles || !enc_ms || !dec_ms || !total_ms)
        return 2;

    for (t = 0; t < tile_count; t++) {
        Tile *tile = &tiles[t];
        int y1;
        tile->y0 = height * t / tile_count;
        y1 = height * (t + 1) / tile_count;
        tile->height = y1 - tile->y0;
        tile->stride[0] = tile->stride[1] = tile->stride[2] = width;
        for (c = 0; c < 3; c++) {
            tile->src[c] = input + (size_t)c * plane_size + (size_t)tile->y0 * width;
            tile->dst[c] = decoded + (size_t)c * plane_size + (size_t)tile->y0 * width;
        }
        tile->comp = tjInitCompress();
        tile->dec = tjInitDecompress();
        tile->capacity = tjBufSize(width, tile->height, TJSAMP_444);
        tile->jpeg = tjAlloc((int)tile->capacity);
        if (!tile->comp || !tile->dec || !tile->jpeg) {
            fprintf(stderr, "tile %d initialization failed\n", t);
            return 2;
        }
    }

    omp_set_dynamic(0);
    for (i = 0; i < warmups; i++) {
        run_encode(tiles, tile_count, width, quality, flags);
        if (!encode_only)
            run_decode(tiles, tile_count, width, flags);
    }

    for (i = 0; i < iterations; i++) {
        double start = now_ms();
        double middle;
        run_encode(tiles, tile_count, width, quality, flags);
        middle = now_ms();
        if (!encode_only)
            run_decode(tiles, tile_count, width, flags);
        enc_ms[i] = middle - start;
        dec_ms[i] = now_ms() - middle;
        total_ms[i] = enc_ms[i] + dec_ms[i];
    }

    for (t = 0; t < tile_count; t++) {
        int w = 0, h = 0, subsamp = -1, cs = -1;
        jpeg_bytes += tiles[t].size;
        if (tiles[t].error || tjDecompressHeader3(tiles[t].dec, tiles[t].jpeg,
                                                  tiles[t].size, &w, &h,
                                                  &subsamp, &cs) != 0 ||
            w != width || h != tiles[t].height || subsamp != TJSAMP_444) {
            fprintf(stderr, "tile %d header validation failed: %dx%d subsamp=%d\n",
                    t, w, h, subsamp);
            failed = 1;
        }
    }

    if (argc == 7) {
        for (t = 0; t < tile_count; t++) {
            char path[1024];
            FILE *fp;
            snprintf(path, sizeof(path), "%s-%02d.jpg", argv[6], t);
            fp = fopen(path, "wb");
            if (!fp || fwrite(tiles[t].jpeg, 1, tiles[t].size, fp) != tiles[t].size) {
                fprintf(stderr, "write %s failed\n", path);
                failed = 1;
            }
            if (fp)
                fclose(fp);
        }
    }

    for (c = 0; c < 3 && !encode_only; c++) {
        size_t off = (size_t)c * plane_size;
        size_t p;
        for (p = 0; p < plane_size; p++) {
            double delta = (double)input[off + p] - decoded[off + p];
            squared_error[c] += delta * delta;
            absolute_error[c] += fabs(delta);
        }
    }

    printf("RESULT mode=%s tiles=%d quality=%d iterations=%d jpeg_bytes=%lu "
           "enc_median_ms=%.3f enc_p95_ms=%.3f "
           "dec_median_ms=%.3f dec_p95_ms=%.3f "
           "total_median_ms=%.3f total_p95_ms=%.3f "
           "fps_from_median=%.3f sampling=444 ",
           encode_only ? "encode_only" : "encode_decode",
           tile_count, quality, iterations, jpeg_bytes,
           percentile(enc_ms, iterations, 0.50), percentile(enc_ms, iterations, 0.95),
           percentile(dec_ms, iterations, 0.50), percentile(dec_ms, iterations, 0.95),
           percentile(total_ms, iterations, 0.50), percentile(total_ms, iterations, 0.95),
           1000.0 / percentile(total_ms, iterations, 0.50));
    for (c = 0; c < 3 && !encode_only; c++) {
        double mse = squared_error[c] / plane_size;
        double psnr = mse == 0.0 ? INFINITY : 10.0 * log10(255.0 * 255.0 / mse);
        printf("%c_psnr=%.3f %c_mae=%.3f%s", "YUV"[c], psnr,
               "YUV"[c], absolute_error[c] / plane_size,
               c == 2 ? "\n" : " ");
    }
    if (encode_only)
        putchar('\n');

    for (t = 0; t < tile_count; t++) {
        tjDestroy(tiles[t].comp);
        tjDestroy(tiles[t].dec);
        tjFree(tiles[t].jpeg);
    }
    free(input);
    free(decoded);
    free(tiles);
    free(enc_ms);
    free(dec_ms);
    free(total_ms);
    return failed ? 1 : 0;
}
