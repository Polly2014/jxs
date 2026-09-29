# RK3588 simulated-LAN UDP loopback validation

Date: 2026-09-29

Board: Orange Pi 5 Plus / RK3588 / 16 GiB / Ubuntu Jammy / Linux 6.1.99

## Verdict

**FAIL for sustained 4K60 and P99 below 50 ms under the board's current
cooling condition.**

The test used real UDP/IP sockets, real 1400-byte application datagrams,
packet validation/reassembly, MPP JPEG decode, and full NV24 assembly on the
RK3588. Linux `netem` on `lo` simulated a 1 Gbit/s wired LAN with 1 ms one-way
delay and 0.2 ms jitter. No physical NIC, cable, switch, or second receiver was
used, so this is not physical-LAN qualification.

The optimized pipeline can follow 60 Hz for roughly the first minute, but it
does not sustain the target as the board heats. In the five-minute 18,000-frame
run, schedule lateness began growing rapidly around frame 4129 (about 69 s),
when the big-core temperature was about 75.8 C. The SoC exposes a 75 C passive
thermal trip. Performance then settled near 50-51 fps while temperatures rose
to 84.1 C.

## Pipeline and simulated network

`4K YUV444P8 -> 4 x JPEG444 tiles -> independent UDP sender -> 127.0.0.1 / netem -> UDP receiver and reassembly -> 4 x MPP JPEG decode -> contiguous NV24`

- JPEG quality: 50.
- Four Cortex-A76 libjpeg-turbo encoders.
- Four persistent RK3588 MPP MJPEG decoders.
- Three bounded frame slots for the encode, network, and decode stages.
- CPU and DMC governors: `performance` during each run.
- DMC frequency observed at test start: 2.112 GHz.
- UDP application datagram: at most 1400 bytes, including a 32-byte header.
- `netem`: `limit 10000 delay 1ms 200us rate 1Gbit`; zero injected loss.
- Socket rmem/wmem maxima temporarily raised to 16 MiB; observed socket
  buffers were 32 MiB under Linux's accounting.
- All governors, socket maxima, and the loopback qdisc were restored afterward.

The default 212,992-byte socket maxima were independently shown to drop the
next 0.77 MB frame while the receiver was decoding. The probe now rejects that
configuration rather than timing out silently.

## Formal results

| Metric | 1000-frame paced | 1000-frame saturated | 18,000-frame paced soak | Requirement |
|---|---:|---:|---:|---:|
| Output throughput | 59.997 fps | 50.029 fps | 52.473 fps | >=60 fps |
| Encode P99 | 16.104 ms | 16.262 ms | 16.372 ms | diagnostic |
| Simulated-network P99 | 11.662 ms | 10.571 ms | 11.138 ms | diagnostic |
| Decode + NV24 assembly P99 | 18.057 ms | 23.554 ms | 23.661 ms | diagnostic |
| End-to-end P99 | 43.245 ms | 69.373 ms | 69.476 ms | <50 ms |
| End-to-end maximum | 46.317 ms | 71.081 ms | 72.387 ms | diagnostic |
| Final schedule lateness | 0.099 ms | n/a | 43,014.019 ms | must not accumulate |
| Transport/decode errors | 0 | 0 | 0 | 0 |
| Output | NV24 / 4:4:4 | NV24 / 4:4:4 | NV24 / 4:4:4 | true 4:4:4 |

The 1000-frame paced run was borderline rather than accepted: it preserved
latency and had no accumulated lateness, but the predeclared strict throughput
gate was 60.000 fps and the independent endpoint calculation was 59.997 fps.
Its regression estimate was 59.998 fps. This short run must not be promoted to
a pass because the longer run exposed sustained thermal failure.

The unpaced saturated run is intentionally preserved as a negative result.
With no admission pacing, encode, networking, and decode contend and capacity
collapses to about 50 fps; the configuration has no burst headroom.

## Five-minute failure evidence

The first four consecutive 1000-frame windows were 59.994, 59.998, 60.005,
and 59.997 fps. Window 4000-4999 fell to 50.882 fps; all later windows remained
between 50.130 and 51.222 fps. Lateness first exceeded 10 ms at frame 4129 and
100 ms at frame 4151.

Temperatures rose as follows:

| Sensor | Start | Maximum | End |
|---|---:|---:|---:|
| bigcore0 / bigcore1 | 42.5 C | 84.1 C | 83.2 C |
| littlecore | 42.5 C | 82.2 C | 82.2 C |
| SoC | 41.6 C | 81.3 C | 81.3 C |

At about 69 s, when sustained lateness started, bigcore0/1 measured 75.8 C.
The kernel reports passive SoC thermal trips at 75 C and 85 C. This timing,
the abrupt persistent throughput step, and the later high temperature are
consistent with thermal throttling. CPU-frequency sampling was not included in
this run, so the exact per-policy throttled frequencies remain to be captured
in the cooling re-test.

The soak transported and reassembled 10,152,000 UDP datagrams and
14,154,462,000 application bytes with matching sent/received counts, zero
malformed packets, zero conflicting duplicates, zero decoder errors, and one
stable sampled NV24 checksum across 181 samples. The failure is performance,
not data corruption.

## What is and is not proved

Proved:

- The custom four-tile JPEG444 payload can traverse real UDP/IP packetization,
  kernel queueing, reassembly, MPP decoding, and NV24 assembly without data
  loss in this loopback model.
- Short, cool-state 60 Hz operation stays below 50 ms.
- Current cooling does not sustain that operating point beyond about one
  minute.

Not proved:

- Physical Ethernet/NIC/switch latency, loss, contention, or remote clocks.
- Long-duration 4K60 after adequate active cooling.
- Diverse real-video content or product image-quality acceptance.

## Required next action

Add or verify active cooling on the RK3588 SoC, then repeat the same five-minute
run with CPU frequency logged each second. Promotion to a conditional pass
requires the full run to maintain 60 Hz, P99 below 50 ms, no accumulated
lateness, no errors, and no thermal frequency reduction. A subsequent 30-60
minute run is required before any product-level sustained claim.

## Evidence

- `data/formal-paced.csv` and `data/formal-paced-evaluation.json`
- `data/formal-saturated.csv` and `data/formal-saturated-evaluation.json`
- `data/soak-5m-paced.csv` and `data/soak-5m-paced-evaluation.json`
- `logs/formal-paced.log`, `logs/formal-saturated.log`, and
  `logs/soak-5m-paced.log`
- `system/formal-active-state.txt`, `system/formal-restored-state.txt`,
  `system/soak-5m-active-state.txt`, `system/soak-5m-restored-state.txt`, and
  `system/soak-5m-temperatures.csv`
- `probe/rk3588_jpeg444_udp_loopback_probe.c`
- `probe/jpeg444_udp_transport.c` and protocol tests

