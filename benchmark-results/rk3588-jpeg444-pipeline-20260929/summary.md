# RK3588 integrated JPEG444 pipeline validation

Date: 2026-09-29

Board: Orange Pi 5 Plus, RK3588, 16 GiB RAM, Ubuntu Jammy, kernel `6.1.99-rockchip-rk3588`

Scope: board-local memory-to-memory processing. Network, capture, display, and network queueing are excluded. The workload repeats one 3840x2160 natural YUV444P8 frame at JPEG quality 50.

## Final verdict

**CONDITIONAL PASS for the combined requirement.**

With the CPU policies and RK3588 DMC memory controller locked to `performance`, the integrated route sustains 60 Hz, preserves true 4:4:4 output, and keeps encode-start-to-full-NV24-assembly latency below 50 ms. The pass is limited to this repeated natural-frame, Q50, board-local workload; it is not yet a guarantee for arbitrary video content, networking, or long-duration thermal operation.

## Pipeline under test

`3840x2160 YUV444P8 -> four 3840x540 JPEG 4:4:4 tiles -> bounded queue -> four persistent MPP MJPEG contexts -> four decoded NV24 tiles -> one contiguous 3840x2160 NV24 frame`

- CPU4-7: four libjpeg-turbo encoders.
- CPU0-3: MPP submission/wait and parallel tile assembly.
- Queue capacity: two frames.
- A frame completes only after all four decoded tiles have been copied into the contiguous NV24 output.
- MPP output is checked as `MPP_FMT_YUV444SP`.
- One earlier 1000-frame run checksummed every assembled output. Final capacity runs sampled frame 0, frame 999, and every 100th frame so validation work did not distort codec throughput.

## Formal results

| Metric | 1000-frame paced 60 Hz | 1000-frame saturated | Requirement |
|---|---:|---:|---:|
| Output throughput | **60.001 fps** | **61.942 fps** | >=60 fps |
| Encode P99 | 15.035 ms | 15.056 ms | diagnostic |
| Decode + full NV24 assembly P99 | 16.197 ms | 16.604 ms | diagnostic |
| End-to-end latency P99 | **32.561 ms** | **34.383 ms** | <50 ms |
| End-to-end maximum | 34.864 ms | 36.494 ms | <50 ms |
| Schedule lateness maximum | **1.594 ms** | n/a | must not accumulate |
| Producer-blocked frames | **7 / 1000** | 993 / 1000 | paced run must not sustain backlog |
| Drops / decode errors | 0 / 0 | 0 / 0 | 0 / 0 |
| Checksum mismatch | 0 | 0 | 0 |
| Output format | NV24 / 4:4:4 | NV24 / 4:4:4 | true 4:4:4 |

Independent CSV recomputation found 1000 rows in each run, frame IDs 0-999, 11 identical sampled checksums, and the same P99 values printed by the probe. Across nine consecutive 100-frame windows, paced throughput stayed between 59.994 and 60.003 fps; saturated throughput stayed between 61.786 and 62.041 fps. Paced lateness began at 0.053 ms and ended at 0.054 ms, so it did not accumulate.

## Acceptance decision

| Criterion | Result |
|---|---|
| At least 1000 measured frames in one integrated program | PASS |
| Steady-state output >=60 fps | PASS: 60.001 fps paced; 61.942 fps saturated capacity |
| End-to-end P99 <50 ms | PASS |
| No sustained accumulation | PASS: lateness 0.053 ms first / 0.054 ms last; max 1.594 ms |
| No drops or decode errors | PASS |
| True NV24 / 4:4:4 output | PASS |
| All four tiles assembled before completion timestamp | PASS |

All specified criteria pass under the tested CPU/DMC performance configuration, so the overall verdict is **CONDITIONAL PASS**.

## Optimization findings

1. MPP advanced task mode requires an MppBuffer-backed JPEG packet; ordinary heap-backed packets fail with `Get no buffer from input packet`.
2. Plain DRM output buffers make CPU reads roughly 10x slower. `MPP_BUFFER_FLAGS_CACHABLE` reduced each tile copy from about 77 ms to about 4-6 ms.
3. Parallel CPU0-3 assembly plus two contiguous plane copies per tile reduced decode+assembly to about 16 ms.
4. RK3588 RGA was tested as a hardware assembly path. The board runtime (`rga_api version 1.10.6_[3]`) rejected NV24 with `Unsupported function: src unsupported YUV444 semi-planner 8bit format`. Converting through a supported subsampled format would violate the 4:4:4 requirement.
5. With DMC left at `dmc_ondemand`, the same paced core test reached only 58.572 fps and accumulated 404.297 ms lateness. Locking DMC to `performance` raised paced output to 60.001 fps and saturated capacity to 61.942 fps. This configuration is therefore part of the passing implementation, not an optional benchmark tweak.
6. A four-frame queue did not fix the low-DMC service rate and raised latency P99 to 71.641 ms. The passing configuration uses queue capacity two.

## Required runtime configuration

- CPU policies `policy0`, `policy4`, and `policy6`: `performance` while running.
- DMC governor: `performance` (observed 2.112 GHz).
- Queue capacity: two frames.
- After the test, CPU governors were restored to `ondemand` and DMC to `dmc_ondemand`.
- Expect higher power and heat than ondemand operation; long-duration thermal validation remains required before a product claim.

## Reproduction

Probe source:

- `probe/rk3588_jpeg444_pipeline_probe.c`
- SHA-256: `4df1ff7c4d1e67a9035ca0fa21f81565777d3945f223d6da2d065e161bb8bcd3`

Build command on the board:

```sh
gcc -O3 -DNDEBUG -fopenmp -pthread -Wall -Wextra \
  -Iinc -Impp/inc -Iosal/inc -Iutils \
  rk3588_jpeg444_pipeline_probe.c \
  -Lbuild-rk3588/mpp -lrockchip_mpp -lturbojpeg -lm \
  -Wl,-rpath,/home/orangepi/mpp-eval-upload-20260929/build-rk3588/mpp \
  -o rk3588_jpeg444_pipeline_probe
```

Evidence:

- `logs/formal-core-1000f-paced-dmc-performance.log`
- `logs/formal-core-1000f-saturated-dmc-performance.log`
- `data/formal-core-1000f-paced-dmc-performance.csv`
- `data/formal-core-1000f-saturated-dmc-performance.csv`
- `logs/formal-1000f-paced.log` and `data/formal-1000f-paced.csv` (every-frame checksum correctness run)
- `logs/smoke-rga.log`

The three CPU governors and DMC governor were explicitly verified as restored after both final runs.
