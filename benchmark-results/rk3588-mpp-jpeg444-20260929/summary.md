# RK3588 4K60 YUV 4:4:4 local codec feasibility

Date: 2026-09-29

Board: Orange Pi 5 Plus, RK3588, 16 GiB RAM, Ubuntu Jammy, kernel `6.1.99-rockchip-rk3588`

Scope: local memory-to-memory encode/decode. Network time is excluded.

## Result

The original CPU JPEG XS implementation is not viable on this board. A conditional RK3588-only route was found:

1. Split each 3840x2160 YUV444P8 frame into four horizontal tiles.
2. Encode the four tiles in parallel on the four Cortex-A76 cores with libjpeg-turbo, baseline JPEG, 4:4:4 sampling.
3. Decode the four JPEG tiles concurrently with the RK3588 hardware JPEG decoder.

For the natural-image case at JPEG quality 50, this route satisfies the local 4K60 stage-throughput and 50 ms codec-latency targets:

| Measurement | Result |
|---|---:|
| Encoded bytes per 4K frame | 768,311 bytes |
| Payload at 60 fps | 368.8 Mbit/s |
| Encode median, concurrent decoder load | 15.079 ms |
| Encode P95, concurrent decoder load | 15.194 ms |
| Four-tile hardware decode throughput | 82.1 full frames/s |
| Approx. four-tile decode time | 12.18 ms/frame |
| Approx. encode + decode latency | 27.3 ms |
| JPEG sampling | 1x1 / 1x1 / 1x1 (4:4:4) |

The concurrent-load test used 180 encode iterations while four hardware decoder instances each decoded 500 tiles. CPU frequency policy was `performance` during the benchmark and was restored to `ondemand` afterward.

A final repeatability run used 60 encode iterations and 120 decode iterations per tile. It measured 14.993 ms encode median, 15.142 ms encode P95, and 82.52--83.44 full frames/s across the four decoder instances. The JPEG headers again reported 1x1 sampling for Y, U, and V. After the run, all three CPU policies (`policy0`, `policy4`, and `policy6`) were verified as `ondemand`.

## Direct loopback latency check

A separate per-frame loopback ran both the four-tile encode and four-tile decode in the same process, with 20 warmups and 120 measured frames. This used libjpeg-turbo for both stages so that the complete codec path could be timestamped directly:

| Measurement | Result |
|---|---:|
| Encode median / P95 | 16.043 / 16.314 ms |
| Decode median / P95 | 11.723 / 11.964 ms |
| Encode + decode median / P95 | 27.778 / 28.204 ms |
| Sequential throughput | 36.0 frames/s |

This directly confirms that codec latency below 50 ms is achievable for the natural-image workload. It does not by itself meet 60 fps because the two CPU stages run sequentially. The proposed 4K60 route instead pipelines the measured CPU encoder (over 60 fps under concurrent hardware-decoder load) with the independent RK3588 hardware JPEG decoder (over 82 fps). The 27.3 ms hybrid figure remains a stage-sum estimate until the integrated hybrid streaming prototype timestamps each frame end to end.

## Fidelity checks

Natural-image quality 50, libjpeg-turbo software round trip:

- Y PSNR: 36.479 dB; MAE: 2.719
- U PSNR: 41.686 dB; MAE: 1.342
- V PSNR: 44.914 dB; MAE: 0.935

Hardware-decoded quality 80 tile:

- Natural image: Y/U/V PSNR = 39.121 / 44.956 / 48.512 dB
- High-frequency image: Y/U/V PSNR = 30.463 / 27.895 / 27.893 dB
- High-frequency U/V horizontal-edge retention ratio = 1.008 / 1.008

The high-frequency edge check confirms that the hardware decoder preserves per-pixel chroma variation rather than silently reducing the image to 4:2:0.

## Important boundary

This is a conditional feasibility result, not a universal 4K60 guarantee.

- The natural test image is NASA/JPL PIA04921, converted to full-range YUV444P8.
- The adversarial high-frequency frame is much harder for JPEG. Even at quality 10, encode P95 was 24.382 ms and therefore could not sustain 60 fps; its codec latency was 48.786 ms and visual quality was poor.
- Four JPEG tiles per video frame require a small framing/reassembly protocol. This is not a single standard MJPEG frame and is not JPEG XS.
- Physical LAN transport and queueing were intentionally excluded from this run.
- A real integrated streaming application still needs bounded queues, frame IDs, tile assembly, dropped-frame accounting, and timestamp-based end-to-end measurement.

## Rejected hardware-encoder route

The RK3588 VEPU2 JPEG encoder does not accept YUV444P/NV24 input. MPP can write a JPEG header declaring 4:4:4, but the hardware reports an invalid input format and the decoded pixels are corrupt. At quality 90, the forced output had only 6.66 / 17.21 / 17.30 dB Y/U/V PSNR against the source. It must not be treated as genuine working 4:4:4 encoding.

## Reproduction assets

- `../../extras/turbojpeg_444_bench.c`
- `../../extras/mpp-jpeg444-4k.json`
- `natural-4k444-q80.jpg`
- `natural-4k444-q90.jpg`
- `libjpeg-q80-tile-00.nv24`
- `libjpeg-high-q80-tile-00.nv24`
- `logs/final-recheck-q50.log`
- `logs/direct-cpu-loopback-q50.log`
