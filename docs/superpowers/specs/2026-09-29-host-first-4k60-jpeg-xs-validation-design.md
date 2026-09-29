# Host-first 4K60 YUV 4:4:4 JPEG XS Validation Design

**Date:** 2026-09-29  
**Repository:** `X-Workspace/jxs`  
**Status:** User-approved design

## 1. Objective

Determine whether the current CPU-only `jxs` reference implementation is worth advancing to an RK3588 and physical-LAN test for this target:

- 3840×2160
- 60 frames per second
- planar YUV 4:4:4, 8-bit
- JPEG XS at 4 bits per pixel
- latency measured from encode start until the complete decoded frame is available in memory
- sustained throughput of at least 60 fps
- no dropped frames or unbounded queue growth
- end-to-end latency P99 below 50 ms

This host-first run is an early feasibility gate. It does not claim to validate RK3588 performance or a physical LAN.

## 2. Existing-System Constraints

The repository currently provides a CPU-only ISO JPEG XS reference library and file-oriented encoder/decoder programs. It does not contain:

- RK3588 MPP/VPU integration;
- capture or display integration;
- UDP, RTP, TCP, or other network transport;
- live 60 fps pacing;
- per-frame end-to-end latency instrumentation.

The current command-line programs load complete input frames or codestreams, invoke the codec, and write complete output files. The existing RK3588 benchmark script measures offline subprocess wall time, including process startup and file I/O. Its reported encode and decode times must therefore be treated as a conservative smoke-test measurement rather than codec-only latency.

The software supports planar 4:4:4. For the selected 8-bit test, inputs use a recognized `.yuv` or `.yuv8p` filename with explicit width, height, and depth arguments. All three component sampling factors must remain 1×1.

## 3. Scope

### Included

1. Record the host hardware and software environment.
2. Build optimized binaries and run the repository test suite.
3. Generate deterministic 4K YUV444P8 inputs.
4. Verify 4K YUV444P8 encode/decode correctness at 4 bpp.
5. Benchmark current CLI encode and decode paths.
6. Produce raw timing data and a machine-readable/human-readable summary.
7. Decide whether to stop or proceed to a codec-only in-memory harness.

### Excluded from this run

- RK3588 execution measurements;
- Wi-Fi testing;
- physical Ethernet, switch, or NIC testing;
- UDP transport implementation;
- HDMI input or output;
- glass-to-glass latency;
- codec optimization;
- quality tuning beyond confirming valid output.

These exclusions prevent a host loopback result from being misreported as an RK3588 or LAN result.

## 4. Test Inputs

Generate three deterministic 3840×2160 YUV444P8 frame classes:

1. **Gradient:** smooth luma and chroma ramps.
2. **Natural-image-derived:** download the NASA/JPL PIA04921 source already recorded in `reports/host-final/input-manifest.json` (URL and SHA-256 are reused), center-crop/resample it to 3840×2160, then convert it to full-resolution Y, U, and V planes. Abort rather than silently substitute another image if the source hash does not match.
3. **High-frequency:** generate a seeded (`3588`) pseudo-random/checkerboard-rich frame that avoids an unrealistically easy input.

Each raw frame contains three full-resolution 8-bit planes:

```text
3840 × 2160 × 3 = 24,883,200 bytes per frame
```

Every generated input is recorded in `input-manifest.json` with its path, byte size, SHA-256 digest, dimensions, pixel format, generation method, and random seed when applicable.

## 5. Build and Correctness Gate

Build in release mode with the repository's supported optimization path. Enable OpenMP when the host toolchain supports it, but record whether it is actually active.

Before timing:

1. Run the repository test suite.
2. Encode each input at 4 bpp.
3. Probe or decode each codestream.
4. Confirm the decoded output is 3840×2160 with three full-resolution 8-bit components.
5. Confirm the encoder and decoder exit successfully and produce non-empty outputs.

Any build failure, test-suite failure, format fallback, truncated codestream, crash, or incorrect output geometry blocks performance claims and is reported as a correctness failure.

Because JPEG XS at 4 bpp is lossy, byte-for-byte equality with the source is not required. Quality metrics are not part of this latency feasibility gate.

## 6. CLI Benchmark Method

Use the same 4 bpp rate target for all comparisons. Test these two legal 3840-wide configurations:

1. **Full-width baseline:** `p=unrestricted;rate=4;bw=20;fq=8;cpih=none;cw=0;nly=2`.
2. **Column-parallel candidate:** `p=unrestricted;rate=4;bw=20;fq=8;cpih=none;cw=4;nly=1`.

Leave gains/priorities on the codec's default PSNR mode so it derives the required values for all three full-resolution components. Do not reuse the one-component explicit arrays from the existing grayscale evaluation: a 3-component YUV444 image has a different band count.

Run the serial build with one thread. Run the OpenMP build with `OMP_NUM_THREADS` set to 1, 4, 8, and the host logical-CPU count capped at 16, deduplicating repeated values. Keep `OMP_DYNAMIC=FALSE`, `OMP_MAX_ACTIVE_LEVELS=1`, and `OMP_WAIT_POLICY=PASSIVE`. Do not use `-j N` in the per-frame latency screen because across-frame workers improve batch throughput but do not reduce one frame's latency.

For every input class, configuration, build, and thread count:

1. Perform three unrecorded warm-up runs.
2. Perform ten measured encode runs.
3. Perform three unrecorded decoder warm-up runs using the resulting codestream.
4. Perform ten measured decode runs.
5. Record every sample rather than only an aggregate.

Record:

- elapsed wall time;
- user and system CPU time when available;
- exit status;
- encoded byte count;
- effective bits per pixel;
- peak resident memory when available;
- active thread/OpenMP configuration;
- host thermal or power-mode information when available.

Compute encode and decode P50, P95, P99, minimum, maximum, and mean. With only ten samples, percentile interpolation rules must be recorded, and the result is a screening measurement rather than a final statistical qualification.

The CLI combined-latency screening value is:

```text
encode P50 + decode P50
```

It is not presented as true end-to-end pipeline latency because encode and decode run as separate processes and include file I/O.

## 7. Decision Gates

### Gate A: Immediate rejection

If the fastest tested configuration has:

```text
encode P50 + decode P50 > 100 ms
```

stop before implementing UDP. Record that the current implementation has insufficient margin for a 50 ms P99 target. If the result is hundreds of milliseconds or seconds, process startup and file I/O cannot plausibly explain the entire gap.

### Gate B: In-memory measurement required

If the fastest CLI result is at most 100 ms, implement a persistent in-memory benchmark using the public library API:

- preload source frames;
- create/reuse codec context and buffers where the API permits;
- run ten warm-up frames;
- measure at least 100 frames;
- report per-frame encode, decode, and combined times;
- use a monotonic high-resolution clock;
- avoid disk I/O inside the measured region.

### Gate C: Network work justified

Only proceed to UDP transport if the in-memory codec-only path shows enough headroom that transport and queueing could still fit inside 50 ms. A practical requirement is:

```text
codec-only P99 materially below 50 ms
```

A value merely equal to 50 ms leaves no budget for packetization, scheduling, transmission, receive processing, reassembly, or jitter.

## 8. Final Acceptance Model

A later complete pipeline passes only when a continuous run of at least ten minutes demonstrates all of the following:

- input pacing at 60 fps;
- output throughput at least 60 fps;
- zero missing, duplicate, or out-of-order completed frames;
- bounded queues with no sustained growth;
- encode-start to decoded-frame-complete P99 below 50 ms;
- all measurements taken with a monotonic clock;
- no thermal throttling invalidating the run.

The host-first CLI run can only return one of these conclusions:

1. **Current implementation rejected early:** clearly too slow; do not build networking around it.
2. **In-memory measurement required:** CLI result is close enough that file/process overhead matters.
3. **Correctness blocked:** build, format, or decode failure prevents a performance conclusion.

It cannot return “RK3588 passes” or “LAN passes.”

## 9. Reporting Layout

Write new artifacts without overwriting existing reports:

```text
reports/host-current/
├── system.json
├── input-manifest.json
├── raw-timings.csv
├── summary.json
└── summary.md
```

`summary.md` must state prominently:

- this is a host-only run;
- physical LAN was not tested;
- RK3588 was not tested;
- CLI timing includes process and file-I/O overhead;
- the decision-gate result and evidence.

## 10. Expected Interpretation

Existing repository reports show multi-second 4K encode/decode times on a high-end x86 host. The expected outcome is therefore an early rejection of the current CPU reference implementation for the 4K60/P99<50ms product target. The new run is still useful because it makes the conclusion reproducible on the current host, verifies the selected YUV444P8/4 bpp path, and prevents premature effort on UDP or RK3588 deployment.
