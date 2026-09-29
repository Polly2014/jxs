# macOS Parallel 4K444 JPEG XS Validation Design

**Date:** 2026-09-29  
**Repository:** `X-Workspace/jxs`  
**Status:** User-approved design

## 1. Objective

Measure how the current CPU-only JPEG XS reference implementation scales on an 8-core Apple M1 when built with Homebrew LLVM/OpenMP, then decide whether it is worth proceeding to RK3588 integration.

The product target remains:

- 3840×2160 YUV444P8;
- JPEG XS at 4 bits per pixel;
- sustained throughput of at least 60 fps;
- no dropped frames or unbounded queue growth in a later real pipeline;
- encode-start to decoded-frame-complete P99 below 50 ms.

This macOS run is a screening test, not RK3588 qualification and not physical-LAN qualification.

## 2. Parallelism Models

The report must keep these models separate:

1. **Intra-frame OpenMP:** parallel DWT rows/columns, linear NLT, reversible color transform, and encoder precinct-column analysis/quantization. This can reduce one frame's encode/decode latency, although serial entropy packing limits scaling.
2. **Across-frame encoder workers (`-j N`):** multiple frames use independent encoder contexts. This can increase batch throughput but does not reduce one frame's latency.
3. **Two-stage encode/decode pipeline bound:** the optimistic codec-only throughput bound is `min(encoder_fps, decoder_fps)`. It is not a measured simultaneous network pipeline and does not include packetization, transport, reassembly, or queueing.

A throughput result from model 2 or 3 must never be reported as single-frame latency.

## 3. Build Design

Use the already-installed Homebrew toolchain:

```text
/opt/homebrew/opt/llvm/bin/clang
/opt/homebrew/opt/llvm/bin/clang++
/opt/homebrew/opt/libomp
```

Maintain two builds:

- `build-host-serial`: existing Apple Clang Release build with OpenMP disabled;
- `build-host-openmp`: Homebrew LLVM Release build with `JXS_ENABLE_OPENMP=ON`.

The macOS reference source includes `<malloc.h>`, which is unavailable on macOS. Both builds may use the existing generated compatibility include `build-host-compat/malloc.h`, whose only content is `#include <stdlib.h>`. Codec source files remain unchanged.

The OpenMP build is valid only if all of the following hold:

1. CMake records `JXS_ENABLE_OPENMP=ON`.
2. The compiler is the Homebrew LLVM compiler, not `/usr/bin/clang`.
3. The binary links against Homebrew `libomp`.
4. A measured run with `OMP_NUM_THREADS > 1` observes more than one process thread.
5. The repository CTest regression passes.

## 4. Fixed Codec Inputs and Configurations

Reuse the exact inputs and hashes from `reports/host-current/input-manifest.json`:

- `gradient`;
- `natural` (NASA/JPL PIA04921-derived frame);
- `high-frequency` (NumPy PCG64 seed 3588).

Each input is exactly 24,883,200 bytes and represents three full-resolution 8-bit planes.

Use the same two 4 bpp configurations as the serial run:

```text
full-width: p=unrestricted;rate=4;bw=20;fq=8;cpih=none;cw=0;nly=2
columns:    p=unrestricted;rate=4;bw=20;fq=8;cpih=none;cw=4;nly=1
```

Record both the requested string and encoder-resolved configuration. Do not compare results if the resolved codec settings differ beyond thread-related execution behavior.

## 5. Intra-frame Latency Matrix

For every input/configuration pair, run the OpenMP binary with:

```text
OMP_NUM_THREADS = 1, 2, 4, 8
OMP_DYNAMIC = FALSE
OMP_MAX_ACTIVE_LEVELS = 1
OMP_WAIT_POLICY = PASSIVE
```

For each group:

- perform three unrecorded warm-ups;
- collect ten encode samples;
- collect ten decode samples;
- report nearest-rank P50/P95/P99, min, max, mean;
- record requested threads, sampled peak process threads, and sampled peak RSS;
- compute speedup relative to the same OpenMP binary at one thread;
- compare OpenMP-one-thread against the serial baseline to expose compiler/runtime changes.

The primary single-frame screening value is:

```text
encode P50 + decode P50
```

The separate encode/decode P99 sum is reported only as a conservative bound, not a measured end-to-end P99 distribution.

## 6. Across-frame Throughput Matrix

Create a 60-frame symbolic-link sequence under ignored `test-data/host-4k444/parallel-sequence/`, cycling through gradient, natural, and high-frequency inputs. Symlinks prevent 1.49 GB of duplicate raw input.

### Encoder worker test

Run:

```text
-j = 1, 2, 4, 8
OMP_NUM_THREADS = 1
-f 1 -n 60
```

Using one OpenMP thread per worker avoids nested teams and isolates across-frame scaling. Record total wall time, average fps, peak threads, peak RSS, output count, total bytes, and any failed/missing outputs.

### Decoder test

Decode the resulting 60 codestreams sequentially while varying:

```text
OMP_NUM_THREADS = 1, 2, 4, 8
```

Record total wall time and average fps. Verify all 60 decoded outputs exist and have the expected YUV444P8 byte size.

For every compatible encoder/decoder pairing, compute:

```text
optimistic_two_stage_fps = min(encoder_batch_fps, decoder_batch_fps)
```

Do not call this a real simultaneous pipeline measurement.

## 7. Decision Gates

Proceed to RK3588 implementation only if the best macOS configuration satisfies all of the following:

1. `encode P50 + decode P50 ≤ 100 ms`;
2. optimistic two-stage codec-only throughput is at least 60 fps;
3. at least one `OMP_NUM_THREADS > 1` configuration improves combined single-frame P50 over the same OpenMP binary at one thread;
4. the 60-frame run completes without OOM and sampled peak RSS remains below 75% of physical host RAM;
5. correctness/regression checks pass.

If any gate fails, stop before UDP implementation and report that this reference implementation is not a viable basis for the RK3588 4K60/P99<50ms product path.

If all gates pass, the result only authorizes an RK3588 test. Final acceptance still requires RK3588-native measurements with CPU affinity, frequency/temperature monitoring, sustained pacing, queue instrumentation, real network transport, and end-to-end timestamps.

## 8. Reporting

Write a new report without overwriting the serial baseline:

```text
reports/host-parallel/
├── system.json
├── input-manifest.json
├── raw-latency.csv
├── raw-throughput.csv
├── summary.json
└── summary.md
```

The report must state prominently:

- Apple M1/macOS was tested;
- RK3588 was not tested;
- physical LAN was not tested;
- intra-frame latency and across-frame throughput are different measurements;
- percentile method and ten-sample limitation;
- compiler, OpenMP runtime, Git commit, CMake settings, binary hashes, and exact codec configurations;
- the decision-gate outcome and evidence.
