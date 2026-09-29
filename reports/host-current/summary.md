# Host-only 4K YUV444P8 JPEG XS CLI Screen

> RK3588 was not tested. Physical LAN was not tested. CLI timing includes process startup, file I/O, codec context setup, and codec execution.

## Target

- 3840×2160 YUV444P8 at 4 bpp
- sustained throughput ≥ 60 fps (16.667 ms frame interval)
- encode-start to decoded-frame-complete P99 < 50 ms

## Fastest CLI configuration

- Case: `gradient`
- Configuration: `full-width`
- Requested config: `p=unrestricted;rate=4;bw=20;fq=8;cpih=none;cw=0;nly=2`
- Resolved config: `p=unrestricted;l=4k-1;s=sublev6bpp;size=4147200;cpih=none;fq=8;`
- Threads requested: 1
- Encode P50/P95/P99: 1.211082 / 1.236089 / 1.236089 s
- Decode P50/P95/P99: 0.835982 / 1.521588 / 1.521588 s
- Encode + decode P50: 2.047064 s
- Equivalent sequential throughput: 0.489 fps
- Optimistic two-stage upper bound: 0.826 fps
- Combined P50 / 50 ms target: 40.9×
- 60 fps / sequential throughput: 122.8×

## Statistical scope

- Percentiles use nearest-rank order statistics.
- With 10 samples nearest-rank P95/P99 may collapse to the maximum; this is a screening run, not tail-latency qualification.
- Sustained 60 fps pacing, queue growth, RK3588 behavior, and physical-LAN behavior were not measured.

## Screening decision

- Status: `reject-current-cli`
- Reason: encode P50 + decode P50 exceeds the 100 ms screening gate

This report cannot establish RK3588 performance or physical-LAN latency.
