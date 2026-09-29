# macOS Parallel 4K444 Validation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the JPEG XS reference codec with Homebrew LLVM/OpenMP, measure 4K YUV444P8 single-frame latency and 60-frame aggregate throughput across 1/2/4/8-way parallelism, and decide whether RK3588 testing is justified.

**Architecture:** Reuse `extras/host_4k444.py` for the intra-frame latency matrix. Add one focused parallel orchestrator for symbolic-link sequence creation, `-j N` encoder throughput, sequential decoder throughput with intra-frame OpenMP, and a combined report. Keep single-frame latency and aggregate throughput as separate datasets and apply both decision gates before recommending RK3588 work.

**Tech Stack:** Homebrew LLVM 22.1.4, Homebrew libomp 22.1.4, CMake, Python 3, NumPy, Pillow, psutil, existing JPEG XS CLI binaries.

**Execution note:** Work in the current nested `X-Workspace/jxs` repository. Do not stage or modify the unrelated untracked `download/` directory. Do not commit unless the user explicitly asks.

---

## File Map

- Use existing `extras/host_4k444.py` for OpenMP intra-frame latency measurements.
- Create `extras/host_parallel_4k444.py` for sequence setup, throughput commands, measurement, report aggregation, and gate decisions.
- Create `extras/test_host_parallel_4k444.py` for sequence, command, summary, and decision-gate unit tests.
- Generate ignored `build-host-openmp/` with Homebrew LLVM/OpenMP.
- Generate ignored `test-data/host-4k444/parallel-sequence/` symlinks and `parallel-work/` intermediates.
- Generate `reports/host-parallel/` with the six files required by the approved specification.

### Task 1: Build and Prove the OpenMP Binary

**Files:**
- Generate: `build-host-openmp/`
- Reuse: `build-host-compat/malloc.h`

- [ ] **Step 1: Verify the installed toolchain before configuration**

Run:

```bash
brew list --versions llvm libomp
/opt/homebrew/opt/llvm/bin/clang --version
```

Expected: LLVM and libomp `22.1.4` are installed; the compiler target is `arm64-apple-darwin`.

- [ ] **Step 2: Configure with Homebrew LLVM and explicit OpenMP paths**

Run:

```bash
ROOT="$(pwd)"
CC=/opt/homebrew/opt/llvm/bin/clang \
CXX=/opt/homebrew/opt/llvm/bin/clang++ \
cmake -S . -B build-host-openmp \
  -DCMAKE_BUILD_TYPE=Release \
  -DJXS_ENABLE_OPENMP=ON \
  -DCMAKE_C_FLAGS="-I$ROOT/build-host-compat -I/opt/homebrew/opt/libomp/include" \
  -DOpenMP_C_FLAGS="-fopenmp" \
  -DOpenMP_C_LIB_NAMES="omp" \
  -DOpenMP_omp_LIBRARY="/opt/homebrew/opt/libomp/lib/libomp.dylib"
cmake --build build-host-openmp --parallel 8
```

Expected: `build-host-openmp/bin/jxs_encoder` and `jxs_decoder` are created.

- [ ] **Step 3: Prove the build metadata and dynamic linkage**

Run:

```bash
grep -E '^(CMAKE_C_COMPILER|CMAKE_BUILD_TYPE|JXS_ENABLE_OPENMP|OpenMP_C_FLAGS)' build-host-openmp/CMakeCache.txt
otool -L build-host-openmp/bin/jxs_encoder
otool -L build-host-openmp/bin/jxs_decoder
```

Expected:

- compiler is `/opt/homebrew/opt/llvm/bin/clang`;
- build type is `Release`;
- `JXS_ENABLE_OPENMP=ON`;
- both executables resolve `libomp.dylib` through the static `libjxs` dependency.

- [ ] **Step 4: Run the existing codec regression suite**

Run:

```bash
ctest --test-dir build-host-openmp --output-on-failure
```

Expected: `codec_regression` PASS. Stop if it fails.

### Task 2: Add Parallel Sequence and Command Primitives

**Files:**
- Create: `extras/test_host_parallel_4k444.py`
- Create: `extras/host_parallel_4k444.py`

- [ ] **Step 1: Write failing unit tests**

Create `extras/test_host_parallel_4k444.py`:

```python
import tempfile
import unittest
from pathlib import Path

from host_parallel_4k444 import (
    decoder_sequence_command,
    encoder_sequence_command,
    gate_decision,
    prepare_sequence,
)


class HostParallel4K444Test(unittest.TestCase):
    def test_prepare_sequence_cycles_sources_as_symlinks(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sources = []
            for name in ("gradient", "natural", "high-frequency"):
                path = root / f"{name}.yuv8p"
                path.write_bytes(name.encode())
                sources.append(path)
            sequence = root / "sequence"
            prepare_sequence(sources, sequence, 6)
            links = sorted(sequence.glob("frame_*.yuv8p"))
            self.assertEqual(len(links), 6)
            self.assertTrue(all(path.is_symlink() for path in links))
            self.assertEqual([path.resolve().name for path in links], [
                "gradient.yuv8p", "natural.yuv8p", "high-frequency.yuv8p",
                "gradient.yuv8p", "natural.yuv8p", "high-frequency.yuv8p",
            ])

    def test_prepare_sequence_refuses_real_file_collision(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.yuv8p"
            source.write_bytes(b"x")
            sequence = root / "sequence"
            sequence.mkdir()
            (sequence / "frame_0001.yuv8p").write_bytes(b"do not replace")
            with self.assertRaisesRegex(ValueError, "non-symlink"):
                prepare_sequence([source], sequence, 1)

    def test_encoder_sequence_command_separates_workers_from_omp_threads(self):
        command = encoder_sequence_command(
            Path("bin/jxs_encoder"), Path("frame_%04d.yuv8p"),
            Path("stream_%04d.jxs"), "rate=4", workers=4, frames=60,
        )
        self.assertIn("-j", command)
        self.assertEqual(command[command.index("-j") + 1], "4")
        self.assertEqual(command[command.index("-n") + 1], "60")

    def test_decoder_sequence_command_requests_all_frames(self):
        command = decoder_sequence_command(
            Path("bin/jxs_decoder"), Path("stream_%04d.jxs"),
            Path("decoded_%04d.yuv8p"), frames=60,
        )
        self.assertEqual(command[command.index("-n") + 1], "60")

    def test_gate_requires_both_latency_and_throughput(self):
        self.assertEqual(gate_decision(0.090, 65, True, 1_000, 16_000)["status"], "proceed-rk3588")
        self.assertEqual(gate_decision(0.101, 65, True, 1_000, 16_000)["status"], "stop")
        self.assertEqual(gate_decision(0.090, 59.9, True, 1_000, 16_000)["status"], "stop")
        self.assertEqual(gate_decision(0.090, 65, False, 1_000, 16_000)["status"], "stop")
        self.assertEqual(gate_decision(0.090, 65, True, 12_001, 16_000)["status"], "stop")


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run the test and verify the module is missing**

Run:

```bash
build-host-venv/bin/python extras/test_host_parallel_4k444.py -v
```

Expected: FAIL with `ModuleNotFoundError: No module named 'host_parallel_4k444'`.

- [ ] **Step 3: Implement the minimal pure helpers**

Create `extras/host_parallel_4k444.py` with:

```python
"""macOS OpenMP latency and aggregate-throughput screen for 4K YUV444P8."""

from pathlib import Path

from host_4k444 import DEPTH, HEIGHT, WIDTH


def prepare_sequence(sources, destination, frames=60):
    sources = [Path(source).resolve() for source in sources]
    if not sources or any(not source.is_file() for source in sources):
        raise ValueError("all sequence sources must exist")
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=True)
    for index in range(1, frames + 1):
        link = destination / f"frame_{index:04d}.yuv8p"
        if link.exists() or link.is_symlink():
            if not link.is_symlink():
                raise ValueError(f"refusing to replace non-symlink: {link}")
            link.unlink()
        link.symlink_to(sources[(index - 1) % len(sources)])
    return [destination / f"frame_{index:04d}.yuv8p" for index in range(1, frames + 1)]


def encoder_sequence_command(executable, source_pattern, output_pattern, config, workers, frames):
    return [
        executable, "-w", str(WIDTH), "-h", str(HEIGHT), "-d", str(DEPTH),
        "-j", str(workers), "-f", "1", "-n", str(frames),
        "-c", config, source_pattern, output_pattern,
    ]


def decoder_sequence_command(executable, source_pattern, output_pattern, frames):
    return [executable, "-f", "1", "-n", str(frames), source_pattern, output_pattern]


def gate_decision(combined_p50, pipeline_fps, parallel_improved, peak_rss_mb, ram_mb):
    passed = (
        combined_p50 <= 0.100
        and pipeline_fps >= 60
        and parallel_improved
        and peak_rss_mb < 0.75 * ram_mb
    )
    return {
        "status": "proceed-rk3588" if passed else "stop",
        "latency_gate": combined_p50 <= 0.100,
        "throughput_gate": pipeline_fps >= 60,
        "parallel_scaling_gate": parallel_improved,
        "memory_gate": peak_rss_mb < 0.75 * ram_mb,
    }
```

- [ ] **Step 4: Run the unit tests**

Run:

```bash
build-host-venv/bin/python extras/test_host_parallel_4k444.py -v
```

Expected: all five tests PASS.

### Task 3: Implement Measured Throughput and Combined Reporting

**Files:**
- Modify: `extras/host_parallel_4k444.py`
- Modify: `extras/test_host_parallel_4k444.py`

- [ ] **Step 1: Add failing tests for throughput summaries**

Add tests for `summarize_fps()` and `optimistic_pipeline_fps()`:

```python
    def test_summarize_fps_uses_nearest_rank_wall_time(self):
        summary = summarize_fps([{"seconds": 10}, {"seconds": 12}, {"seconds": 11}], frames=60)
        self.assertEqual(summary["p50_seconds"], 11)
        self.assertAlmostEqual(summary["p50_fps"], 60 / 11)

    def test_optimistic_pipeline_is_slower_stage(self):
        self.assertEqual(optimistic_pipeline_fps(80, 55), 55)
```

Run the unit test and expect import failures for both helpers.

- [ ] **Step 2: Implement summary and process helpers**

Reuse `evaluate.run` and `host_4k444.nearest_rank`. Implement:

```python
def summarize_fps(samples, frames):
    seconds = [sample["seconds"] for sample in samples]
    p50 = nearest_rank(seconds, 50)
    return {
        "samples": len(samples),
        "frames": frames,
        "p50_seconds": p50,
        "p95_seconds": nearest_rank(seconds, 95),
        "p50_fps": frames / p50,
        "peak_rss_bytes": max(sample["sampled_peak_rss_bytes"] for sample in samples),
        "peak_threads": max(sample["sampled_peak_threads"] for sample in samples),
    }


def optimistic_pipeline_fps(encoder_fps, decoder_fps):
    return min(encoder_fps, decoder_fps)
```

- [ ] **Step 3: Implement throughput execution**

Add a CLI with these exact options:

```text
--bin-dir       required OpenMP binary directory
--serial-report default reports/host-current
--data          default test-data/host-4k444
--output        default reports/host-parallel
--frames        default 60
--workers       default 1 2 4 8
--threads       default 1 2 4 8
--warmups       default 1
--repeats       default 3
--timeout       default 600
```

For each `CONFIGURATIONS` entry:

1. create/refresh the 60 input symlinks;
2. run `-j 1/2/4/8` with `OMP_NUM_THREADS=1`;
3. before each encode run, remove only generated `.jxs` files from `parallel-work/encoded/`;
4. verify exactly 60 non-empty codestreams after each run;
5. retain the final valid codestream set;
6. decode it with `OMP_NUM_THREADS=1/2/4/8`;
7. before each decode run, remove only generated `.yuv8p` outputs from `parallel-work/decoded/`;
8. verify exactly 60 outputs of `FRAME_BYTES` each;
9. write every sample to `raw-throughput.csv` with phase, configuration, workers/threads, sample, seconds, fps, peak RSS, peak threads, and output counts.

Run one warm-up and three measured repetitions for each group. Keep `OMP_DYNAMIC=FALSE`, `OMP_MAX_ACTIVE_LEVELS=1`, and `OMP_WAIT_POLICY=PASSIVE`.

- [ ] **Step 4: Aggregate the latency and throughput reports**

The CLI reads the completed latency report from `test-data/host-4k444/parallel-work/latency-report/`, copies its raw CSV to `reports/host-parallel/raw-latency.csv`, and writes:

- `system.json`: Git/compiler/CMake/OpenMP linkage, binary hashes, run controls, and host data;
- `input-manifest.json`: copied exact input provenance;
- `raw-throughput.csv`;
- `summary.json`: latency groups, throughput groups, speedups, best latency, best encoder/decoder throughput, optimistic pipeline bound, memory peak, and decision gates;
- `summary.md`: a human-readable report that prominently states Apple M1 was tested while RK3588 and physical LAN were not.

The report must state that decoder output file I/O is included and that the pipeline fps is an optimistic bound rather than a simultaneous pipeline measurement.

- [ ] **Step 5: Run all Python unit tests**

Run:

```bash
build-host-venv/bin/python extras/test_host_4k444.py -v
build-host-venv/bin/python extras/test_host_parallel_4k444.py -v
```

Expected: all tests PASS.

### Task 4: Run the Intra-frame OpenMP Latency Matrix

**Files:**
- Generate: `test-data/host-4k444/parallel-work/latency-report/`

- [ ] **Step 1: Confirm the temporary output is absent or empty**

Inspect before removing. Remove it only if it was created by this plan.

- [ ] **Step 2: Run the full latency matrix**

Run:

```bash
build-host-venv/bin/python extras/host_4k444.py \
  --bin-dir build-host-openmp/bin \
  --data test-data/host-4k444 \
  --output test-data/host-4k444/parallel-work/latency-report \
  --warmups 3 --repeats 10 --threads 1 2 4 8 --timeout 600 \
  --openmp-status "enabled with Homebrew LLVM 22.1.4 and libomp 22.1.4"
```

Expected: 480 measured rows (`3 inputs × 2 configurations × 4 thread counts × 2 phases × 10 repetitions`), all correctness probes pass, and groups requesting more than one thread observe more than one sampled thread.

- [ ] **Step 3: Validate the latency dataset**

Run a Python assertion script that checks:

- exactly 480 CSV rows;
- thread values are `{1,2,4,8}`;
- requested/resolved configurations are present;
- all decoded correctness checks passed;
- `system.json` records OpenMP enabled and Homebrew LLVM;
- at least one measured group with requested threads >1 has sampled peak threads >1.

Stop if any assertion fails.

### Task 5: Run the 60-frame Throughput Matrix and Apply Gates

**Files:**
- Generate: `reports/host-parallel/system.json`
- Generate: `reports/host-parallel/input-manifest.json`
- Generate: `reports/host-parallel/raw-latency.csv`
- Generate: `reports/host-parallel/raw-throughput.csv`
- Generate: `reports/host-parallel/summary.json`
- Generate: `reports/host-parallel/summary.md`

- [ ] **Step 1: Run the parallel throughput orchestrator**

Run:

```bash
build-host-venv/bin/python extras/host_parallel_4k444.py \
  --bin-dir build-host-openmp/bin \
  --serial-report reports/host-current \
  --data test-data/host-4k444 \
  --output reports/host-parallel \
  --frames 60 --workers 1 2 4 8 --threads 1 2 4 8 \
  --warmups 1 --repeats 3 --timeout 600
```

Expected: all encoder and decoder groups complete and produce verified frame counts.

- [ ] **Step 2: Validate report integrity**

Assert:

- latency CSV has 480 rows;
- throughput CSV has 48 rows (`2 configurations × (4 encoder worker groups + 4 decoder thread groups) × 3 repetitions`);
- all requested worker/thread values appear;
- all outputs passed count/size checks;
- scope flags for RK3588 and physical LAN are false;
- the decision includes all four explicit gates;
- exact codec configuration and OpenMP build metadata are present.

- [ ] **Step 3: Apply the decision**

Report both dimensions independently:

- best single-frame encode/decode P50/P95/P99 and speedups;
- best 60-frame encoder fps;
- best 60-frame decoder fps;
- optimistic two-stage fps;
- peak memory/thread counts;
- gate outcome.

Only recommend RK3588 execution if every gate passes. Never infer `<50ms` from aggregate fps.

- [ ] **Step 4: Run final verification**

Run:

```bash
build-host-venv/bin/python extras/test_host_4k444.py -v
build-host-venv/bin/python extras/test_host_parallel_4k444.py -v
ctest --test-dir build-host-openmp --output-on-failure
git diff --check
git status --short
```

Expected: Python tests and codec regression pass; no whitespace errors; only the approved spec/plan, parallel runner/tests, and `reports/host-parallel/` are new or modified. `download/` remains untouched and untracked.

## Recorded Execution Adjustment

The complete 60-frame throughput matrix was split into independent `full-width` and `columns` runs so each command stayed below the 600-second harness limit. Each of the 16 throughput groups used one formal 60-frame run with no separate warm-up (`2 configurations × 2 phases × 4 parallelism values`). The final report explicitly records this as a screening measurement rather than a stable throughput-tail estimate. The intra-frame latency matrix was not reduced: it contains all 480 planned samples with three warm-ups and ten formal samples per group.
