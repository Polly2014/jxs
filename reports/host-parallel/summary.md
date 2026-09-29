# macOS OpenMP 4K YUV444P8 JPEG XS Screen

> Apple M1/macOS was tested. RK3588 and physical LAN were not tested.

## Single-frame intra-frame OpenMP latency

- Best case/config/threads: `gradient` / `columns` / 8
- Encode P50/P95/P99: 0.585934 / 0.619940 / 0.619940 s
- Decode P50/P95/P99: 0.654598 / 0.663488 / 0.663488 s
- Combined P50: 1.240532 s
- Speedup vs OpenMP 1-thread: 1.786×
- Speedup vs Apple-Clang serial baseline: 1.821×

## Aggregate 60-frame throughput

- Measurement repetitions: 1 formal run(s) after 0 warm-up run(s) per group
- Frames per run: 60
- Best encoder: `full-width`, -j 8, 2.728 fps
- Best decoder: `columns`, OMP 8, 1.428 fps
- Best compatible pipeline configuration: `columns`
- Optimistic two-stage upper bound: 1.428 fps

The throughput result is a batch average. It does not reduce one frame's measured latency.
Decoder timing includes decoded-frame file output. The two-stage value is not a simultaneous network pipeline measurement.

## Decision gates

- Status: `stop`
- Latency ≤100 ms: False
- Optimistic throughput ≥60 fps: False
- Parallel scaling improves: True
- Peak RSS <75% RAM: True
