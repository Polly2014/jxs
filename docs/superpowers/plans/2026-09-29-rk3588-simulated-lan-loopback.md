# RK3588 Simulated-LAN UDP Loopback Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add and validate a real UDP/IP loopback stage with a 1 ms +/- 0.2 ms, 1 Gbit/s simulated wired-LAN profile around the passing RK3588 JPEG444 pipeline.

**Architecture:** Keep the accepted board-local probe immutable. Add a portable packet protocol/reassembler with host tests, then add a separate RK3588 probe that sends 1400-byte UDP datagrams through `lo`, reassembles them, and feeds the existing four-context MPP decoder. Use Linux `netem` for delay, jitter, and rate emulation and preserve encode-start through full-NV24-completion timing.

**Tech Stack:** C11, POSIX UDP sockets and pthreads, libjpeg-turbo, OpenMP, Rockchip MPP, Linux `tc netem`, shell/CSV evidence tooling.

**Spec:** `docs/superpowers/specs/2026-09-29-rk3588-simulated-lan-loopback-design.md`

## Global Constraints

- Physical NIC, cable, switch, and remote-host behavior remain explicitly untested.
- UDP datagrams are at most 1400 bytes including the application header.
- Profile is one-way 1 ms delay, 0.2 ms jitter, normal distribution, 1 Gbit/s rate, and zero injected loss.
- The original `rk3588_jpeg444_pipeline_probe.c` is not modified.
- CPU and DMC governors must be restored and `lo` qdisc removed after every board run.
- Acceptance requires 1000 measured paced frames, output at least 60.000 fps, end-to-end P99 below 50 ms, no accumulated lateness, and zero transport/decode failures.

## Review Focus

- Reordered UDP fragments must complete the correct tile without corrupting offsets; Task 1 tests reverse-order delivery.
- Duplicate fragments must not inflate completion counts; Task 1 tests an identical duplicate and conflicting duplicate.
- Malformed frame/tile/offset/length metadata must be rejected before memory access; Task 1 table-tests each invalid boundary.
- A receiver or codec failure must unblock both threads and restore host configuration; Task 2 exercises injected malformed input and Task 3 uses a trap-backed runner.
- Kernel/socket buffering must not conceal unbounded queue growth; Task 2 preserves a three-frame application queue matching the encode/network/decode stages and reports blocked frames and schedule lateness.

---

### Task 1: Portable UDP protocol and reassembler

**Files:**
- Create: `benchmark-results/rk3588-jpeg444-udp-loopback-20260929/probe/jpeg444_udp_transport.h`
- Create: `benchmark-results/rk3588-jpeg444-udp-loopback-20260929/probe/jpeg444_udp_transport.c`
- Create: `benchmark-results/rk3588-jpeg444-udp-loopback-20260929/probe/test_jpeg444_udp_transport.c`

**Interfaces:**
- Produces: `jxs_udp_header_encode`, `jxs_udp_header_decode`, `jxs_udp_fragment_count`, `jxs_udp_reassembly_reset`, `jxs_udp_reassembly_push`, and `jxs_udp_reassembly_complete`.
- Consumes: fixed protocol limits `JXS_UDP_DATAGRAM_BYTES`, `JXS_UDP_HEADER_BYTES`, four tiles, and a caller-owned tile buffer/capacity.

- [ ] **Step 1: Write the failing protocol tests**

  Add literal tests for header byte layout and round-trip, exact fragment-count boundaries, reversed fragment order, identical and conflicting duplicates, and invalid IDs/offsets/lengths.

- [ ] **Step 2: Run the tests and verify RED**

  Run: `cc -std=c11 -Wall -Wextra -Werror probe/test_jpeg444_udp_transport.c probe/jpeg444_udp_transport.c -o /tmp/test-jxs-udp && /tmp/test-jxs-udp`

  Expected: compilation fails because the transport implementation does not yet define the declared API.

- [ ] **Step 3: Implement the minimal portable protocol/reassembler**

  Use explicit byte-order helpers and field-wise serialization rather than a packed C struct. Validate all metadata before any copy. Track fragments with a caller-allocated bitmap and reject a conflicting duplicate.

- [ ] **Step 4: Run the tests and verify GREEN**

  Run the Step 2 command.

  Expected: `PASS jpeg444_udp_transport` with zero compiler warnings.

### Task 2: Integrated RK3588 UDP loopback probe

**Files:**
- Create: `benchmark-results/rk3588-jpeg444-udp-loopback-20260929/probe/rk3588_jpeg444_udp_loopback_probe.c`
- Reuse read-only: `benchmark-results/rk3588-jpeg444-pipeline-20260929/probe/rk3588_jpeg444_pipeline_probe.c`

**Interfaces:**
- Consumes: Task 1 transport API and the accepted encoder/MPP/NV24 assembly logic.
- Produces: CLI `INPUT FRAMES WARMUPS FPS QUALITY QUEUE_CAP PORT CSV`, per-frame CSV, one machine-readable `RESULT` line, and nonzero exit on transport/codec/checksum failure.

- [ ] **Step 1: Add a failing integration smoke expectation**

  Compile the named probe on RK3588 and run 3 measured frames without `netem`; the expected pre-implementation failure is a missing source/binary.

- [ ] **Step 2: Implement sender, receiver/reassembly, and timing**

  Allocate separate transmit and receive JPEG buffers per bounded slot. Send every tile as <=1400-byte datagrams. Receive and validate by frame/tile/fragment, tolerate reordering, and decode only when all four tiles are complete. Record `encode_ms`, `network_ms`, `decode_assemble_ms`, total latency, datagrams, bytes, and error counters.

- [ ] **Step 3: Build and run the smoke test**

  Expected: three frames complete, format is NV24/4:4:4, checksums agree, and sent/received packet and byte counts match.

- [ ] **Step 4: Run a malformed-packet failure test**

  Set `JXS_UDP_INJECT_MALFORMED_FRAME=1` for a three-frame run.

  Expected: nonzero exit, a transport validation error, no hang, and both threads terminate.

### Task 3: Simulated-LAN formal runs and independent evaluation

**Files:**
- Create: `benchmark-results/rk3588-jpeg444-udp-loopback-20260929/run-simulated-lan.sh`
- Create: `benchmark-results/rk3588-jpeg444-udp-loopback-20260929/evaluate.py`
- Create: raw files under `data/`, `logs/`, and `system/`

**Interfaces:**
- Consumes: Task 2 probe and stored board SSH settings without printing secrets.
- Produces: paced and saturated CSV/log evidence, temperature/governor/qdisc evidence, and independently recomputed JSON metrics.

- [ ] **Step 1: Write a failing evaluator test fixture**

  Use a literal three-row CSV fixture with hand-derived P99/max/count values and assert rejection of missing rows, non-monotonic frame IDs, errors, and latency >=50 ms.

- [ ] **Step 2: Verify RED, implement evaluator, and verify GREEN**

  Run: `python3 -m unittest benchmark-results/rk3588-jpeg444-udp-loopback-20260929/test_evaluate.py -v`

  Expected before implementation: import failure. Expected afterward: all tests pass.

- [ ] **Step 3: Run the trap-backed board test harness**

  Apply `netem delay 1ms 0.2ms distribution normal rate 1000mbit`, lock CPU/DMC to `performance`, run 1000-frame paced and saturated tests, sample temperatures, then remove the qdisc and restore governors in `trap cleanup EXIT`.

- [ ] **Step 4: Copy raw evidence back and independently evaluate**

  Expected: row count and frame IDs match, packet/byte counters match, qdisc evidence names the selected profile, and acceptance is based on recomputed rather than copied probe metrics.

### Task 4: Report the simulated-LAN verdict

**Files:**
- Create: `benchmark-results/rk3588-jpeg444-udp-loopback-20260929/summary.md`
- Modify: `reports/4k60-yuv444-rk3588-feasibility-20260929.md`

**Interfaces:**
- Consumes: Task 3 raw evidence and independent evaluation.
- Produces: an evidence-linked verdict that distinguishes real UDP/kernel processing from simulated physical propagation.

- [ ] **Step 1: Write the result summary from measured evidence**

  Report configuration, P50/P95/P99/max, sustained throughput/lateness, drops/errors, temperatures, and restoration checks. Do not describe the test as physical LAN.

- [ ] **Step 2: Update the top-level feasibility boundary**

  Add a simulated-LAN row and explicitly preserve the outstanding physical-LAN limitation.

- [ ] **Step 3: Verify all evidence references and final wording**

  Run protocol tests, evaluator tests, independent CSV checks, `git diff --check`, and a search ensuring every LAN claim is qualified as simulated unless it explicitly says physical LAN was not tested.
