# Host-first 4K444 Validation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Produce a reproducible host-only correctness and CLI-latency report for 3840×2160 YUV444P8 JPEG XS at 4 bpp, then stop early if the current CPU reference implementation is clearly unable to meet 60 fps and P99 < 50 ms.

**Architecture:** Add one focused Python runner beside the existing evaluation scripts. It generates deterministic planar YUV444P8 frames, invokes the existing `jxs_encoder` and `jxs_decoder`, reuses the repository's subprocess timing helper, validates decoded geometry/sampling, and writes CSV/JSON/Markdown reports. This plan covers only the CLI screening gate; an in-memory harness and UDP transport require separate follow-up plans only if the screening result is at most 100 ms.

**Tech Stack:** Python 3, `unittest`, NumPy, Pillow, psutil, CMake, existing C JPEG XS CLI binaries.

**Execution note:** Work in the current nested `X-Workspace/jxs` repository because the user did not request a worktree. Do not commit unless the user explicitly asks for a commit.

---

## File Map

- Create `extras/host_4k444.py`: deterministic input generation, codec command construction, timing, validation, aggregation, and report writing.
- Create `extras/test_host_4k444.py`: unit tests for plane order, exact frame size, percentile calculation, probe validation, and gate decisions.
- Generate `test-data/host-4k444/`: downloaded source and three raw YUV inputs; this is covered by the existing `test-data/` ignore rule.
- Generate `reports/host-current/`: host metadata, manifest, raw timings, summary JSON, and summary Markdown. Keep codec intermediates under ignored `test-data/host-4k444/work/`, not in the report directory.
- Do not modify `extras/evaluate.py`; import its proven `run()` timing helper and `prepare_media.digest()` instead.

### Task 1: Add Testable Measurement Primitives

**Files:**
- Create: `extras/test_host_4k444.py`
- Create: `extras/host_4k444.py`

- [ ] **Step 1: Write failing unit tests for frame layout, percentiles, probe parsing, and the decision gate**

Create `extras/test_host_4k444.py`:

```python
import tempfile
import unittest
from pathlib import Path

import numpy as np

from host_4k444 import (
    FRAME_BYTES,
    gate_decision,
    nearest_rank,
    validate_probe,
    write_yuv444p8,
)


class Host4K444Test(unittest.TestCase):
    def test_frame_constant_is_three_full_resolution_planes(self):
        self.assertEqual(FRAME_BYTES, 3840 * 2160 * 3)

    def test_write_yuv444p8_uses_planar_y_u_v_order(self):
        y = np.array([[1, 2], [3, 4]], dtype=np.uint8)
        u = np.array([[5, 6], [7, 8]], dtype=np.uint8)
        v = np.array([[9, 10], [11, 12]], dtype=np.uint8)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "frame.yuv8p"
            write_yuv444p8(path, y, u, v)
            self.assertEqual(path.read_bytes(), bytes(range(1, 13)))

    def test_write_yuv444p8_rejects_shape_mismatch(self):
        y = np.zeros((2, 2), dtype=np.uint8)
        u = np.zeros((1, 2), dtype=np.uint8)
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError, "same 2-D shape"):
                write_yuv444p8(Path(directory) / "bad.yuv8p", y, u, y)

    def test_nearest_rank_uses_maximum_for_p99_of_ten_samples(self):
        self.assertEqual(nearest_rank(list(range(1, 11)), 99), 10)

    def test_validate_probe_accepts_full_resolution_three_component_output(self):
        output = "Image: 3840x2160 3-comp@8bpp\nSampling: 1x1/1x1/1x1\n"
        validate_probe(output)

    def test_validate_probe_rejects_subsampled_output(self):
        output = "Image: 3840x2160 3-comp@8bpp\nSampling: 1x1/2x2/2x2\n"
        with self.assertRaisesRegex(ValueError, "sampling"):
            validate_probe(output)

    def test_gate_rejects_combined_p50_above_100ms(self):
        self.assertEqual(gate_decision(0.061, 0.041)["status"], "reject-current-cli")

    def test_gate_requests_in_memory_measurement_at_or_below_100ms(self):
        self.assertEqual(gate_decision(0.060, 0.040)["status"], "needs-in-memory")


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run the unit test and verify the new module is missing**

Run:

```bash
cd X-Workspace/jxs
python3 extras/test_host_4k444.py -v
```

Expected: FAIL with `ModuleNotFoundError: No module named 'host_4k444'`.

- [ ] **Step 3: Implement the minimal pure helpers**

Create `extras/host_4k444.py` with these definitions first:

```python
"""Host-only 4K YUV444P8 JPEG XS correctness and CLI latency screen."""

import math
from pathlib import Path

import numpy as np

WIDTH = 3840
HEIGHT = 2160
DEPTH = 8
COMPONENTS = 3
FRAME_BYTES = WIDTH * HEIGHT * COMPONENTS


def write_yuv444p8(path, y, u, v):
    planes = [np.asarray(plane) for plane in (y, u, v)]
    if any(plane.ndim != 2 for plane in planes) or len({plane.shape for plane in planes}) != 1:
        raise ValueError("Y, U, and V must have the same 2-D shape")
    if any(plane.dtype != np.uint8 for plane in planes):
        raise ValueError("Y, U, and V must use uint8 samples")
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("wb") as stream:
        for plane in planes:
            stream.write(np.ascontiguousarray(plane).tobytes())


def nearest_rank(samples, percentile):
    if not samples:
        raise ValueError("samples must not be empty")
    if not 0 < percentile <= 100:
        raise ValueError("percentile must be in (0, 100]")
    ordered = sorted(samples)
    return ordered[math.ceil(percentile / 100 * len(ordered)) - 1]


def validate_probe(output):
    if "Image: 3840x2160 3-comp@8bpp" not in output:
        raise ValueError("decoded geometry/depth/components do not match 4K YUV444P8")
    if "Sampling: 1x1/1x1/1x1" not in output:
        raise ValueError("decoded sampling is not full-resolution 4:4:4")


def gate_decision(encode_p50, decode_p50):
    combined = encode_p50 + decode_p50
    if combined > 0.100:
        return {
            "status": "reject-current-cli",
            "combined_p50_seconds": combined,
            "reason": "encode P50 + decode P50 exceeds the 100 ms screening gate",
        }
    return {
        "status": "needs-in-memory",
        "combined_p50_seconds": combined,
        "reason": "CLI timing is close enough that process and file I/O overhead must be removed",
    }
```

- [ ] **Step 4: Run the unit tests**

Run:

```bash
cd X-Workspace/jxs
python3 extras/test_host_4k444.py -v
```

Expected: 8 tests, all PASS.

- [ ] **Step 5: Record a no-commit checkpoint**

Run:

```bash
git -C X-Workspace/jxs diff --check
git -C X-Workspace/jxs status --short
```

Expected: the two new `extras/` files and the already-approved `docs/` files are listed, with no whitespace errors. Do not commit without explicit authorization.

### Task 2: Generate Reproducible 4K YUV444P8 Inputs

**Files:**
- Modify: `extras/host_4k444.py`
- Modify: `extras/test_host_4k444.py`

- [ ] **Step 1: Add failing tests for deterministic input generation**

Add imports for `digest_bytes`, `gradient_planes`, and `high_frequency_planes`, then add:

```python
    def test_gradient_planes_are_deterministic_and_full_resolution(self):
        first = gradient_planes(8, 4)
        second = gradient_planes(8, 4)
        self.assertEqual([p.shape for p in first], [(4, 8)] * 3)
        self.assertTrue(all(np.array_equal(a, b) for a, b in zip(first, second)))

    def test_high_frequency_planes_are_seeded(self):
        first = high_frequency_planes(8, 4, seed=3588)
        second = high_frequency_planes(8, 4, seed=3588)
        other = high_frequency_planes(8, 4, seed=3589)
        self.assertTrue(all(np.array_equal(a, b) for a, b in zip(first, second)))
        self.assertFalse(all(np.array_equal(a, b) for a, b in zip(first, other)))

    def test_digest_bytes_is_sha256(self):
        self.assertEqual(
            digest_bytes(b"abc"),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        )
```

- [ ] **Step 2: Run tests and verify the new helpers are missing**

Run:

```bash
cd X-Workspace/jxs
python3 extras/test_host_4k444.py -v
```

Expected: FAIL on imports for the three new helper functions.

- [ ] **Step 3: Implement deterministic synthetic generators and natural-frame conversion**

Add imports and functions to `extras/host_4k444.py`:

```python
import hashlib
import json
import shutil
import subprocess
import urllib.request

from PIL import Image, ImageOps

NATURAL_URL = "https://images-assets.nasa.gov/image/PIA04921/PIA04921~orig.jpg"
NATURAL_SHA256 = "4c2bc14dccd339b73c55cba14d3c18a63c4609041c375be18c442ee13153a274"


def digest_bytes(data):
    return hashlib.sha256(data).hexdigest()


def digest_file(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def gradient_planes(width=WIDTH, height=HEIGHT):
    x = np.arange(width, dtype=np.uint16)[None, :]
    y = np.arange(height, dtype=np.uint16)[:, None]
    luma = ((x + y) & 0xff).astype(np.uint8)
    cb = np.broadcast_to((64 + (x // 4) % 128).astype(np.uint8), (height, width)).copy()
    cr = np.broadcast_to((64 + (y // 4) % 128).astype(np.uint8), (height, width)).copy()
    return luma, cb, cr


def high_frequency_planes(width=WIDTH, height=HEIGHT, seed=3588):
    rng = np.random.default_rng(seed)
    return tuple(rng.integers(0, 256, size=(height, width), dtype=np.uint8) for _ in range(3))


def download_verified(url, sha256, destination):
    destination = Path(destination)
    if destination.exists() and digest_file(destination) == sha256:
        return destination
    partial = destination.with_suffix(destination.suffix + ".part")
    curl = shutil.which("curl")
    if curl:
        subprocess.run(
            [curl, "--fail", "--location", "--silent", "--show-error", "--max-time", "600",
             "--output", str(partial), url],
            check=True,
            timeout=620,
        )
    else:
        request = urllib.request.Request(url, headers={"User-Agent": "jxs-host-4k444/1.0"})
        with urllib.request.urlopen(request, timeout=120) as response, partial.open("wb") as out:
            shutil.copyfileobj(response, out)
    partial.replace(destination)
    if digest_file(destination) != sha256:
        raise RuntimeError(f"Source SHA-256 mismatch: {destination}")
    return destination


def natural_planes(source, width=WIDTH, height=HEIGHT):
    with Image.open(source) as image:
        fitted = ImageOps.fit(image.convert("RGB"), (width, height), method=Image.Resampling.LANCZOS)
        y, cb, cr = fitted.convert("YCbCr").split()
    return tuple(np.asarray(plane, dtype=np.uint8) for plane in (y, cb, cr))


def prepare_inputs(root):
    root = Path(root)
    root.mkdir(parents=True, exist_ok=True)
    source = download_verified(NATURAL_URL, NATURAL_SHA256, root / "pia04921.jpg")
    cases = [
        ("gradient", gradient_planes(), "deterministic gradients"),
        ("natural", natural_planes(source), "NASA/JPL PIA04921 center-crop/resample"),
        ("high-frequency", high_frequency_planes(), "NumPy PCG64 seed 3588"),
    ]
    manifest = []
    for name, planes, operation in cases:
        path = root / f"{name}.yuv8p"
        write_yuv444p8(path, *planes)
        if path.stat().st_size != FRAME_BYTES:
            raise RuntimeError(f"Unexpected raw frame size: {path}")
        manifest.append({
            "name": name,
            "input": path.name,
            "width": WIDTH,
            "height": HEIGHT,
            "depth": DEPTH,
            "format": "YUV444P8",
            "bytes": path.stat().st_size,
            "sha256": digest_file(path),
            "operation": operation,
        })
    payload = {
        "source": {"url": NATURAL_URL, "sha256": NATURAL_SHA256},
        "cases": manifest,
    }
    (root / "input-manifest.json").write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    return payload
```

- [ ] **Step 4: Run unit tests and generate the actual inputs once**

Run:

```bash
cd X-Workspace/jxs
python3 extras/test_host_4k444.py -v
python3 -c 'import sys; sys.path.insert(0, "extras"); from host_4k444 import prepare_inputs; prepare_inputs("test-data/host-4k444")'
```

Expected:

- all tests PASS;
- three `.yuv8p` files exist;
- each file is exactly `24,883,200` bytes;
- `test-data/host-4k444/input-manifest.json` records hashes and provenance.

### Task 3: Add Codec Correctness and Timing Runner

**Files:**
- Modify: `extras/host_4k444.py`
- Modify: `extras/test_host_4k444.py`

- [ ] **Step 1: Add failing command-construction tests**

Add tests that require these exact properties:

```python
    def test_encoder_command_declares_raw_geometry_and_depth(self):
        command = encoder_command(
            Path("bin/jxs_encoder"), Path("in.yuv8p"), Path("out.jxs"), "rate=4"
        )
        self.assertEqual(command[1:7], ["-w", "3840", "-h", "2160", "-d", "8"])
        self.assertEqual(command[-2:], [Path("in.yuv8p"), Path("out.jxs")])

    def test_decoder_command_requests_verbose_probe_and_planar_output(self):
        command = decoder_command(Path("bin/jxs_decoder"), Path("in.jxs"), Path("out.yuv8p"))
        self.assertEqual(command, [Path("bin/jxs_decoder"), "-v", Path("in.jxs"), Path("out.yuv8p")])
```

- [ ] **Step 2: Run tests and verify both command helpers are missing**

Run the unittest command from Task 2. Expected: import/test failure for `encoder_command` and `decoder_command`.

- [ ] **Step 3: Implement exact codec configurations, commands, and measured runs**

Add to `extras/host_4k444.py`:

```python
import csv
import os
import platform
import statistics
import time

import psutil

from evaluate import run

CONFIGURATIONS = {
    "full-width": "p=unrestricted;rate=4;bw=20;fq=8;cpih=none;cw=0;nly=2",
    "columns": "p=unrestricted;rate=4;bw=20;fq=8;cpih=none;cw=4;nly=1",
}

# Gains/priorities intentionally remain in the codec's default PSNR mode. The
# existing explicit 8/10-value arrays are for one-component grayscale cases;
# YUV444 has three transformed component band sets.


def encoder_command(executable, source, codestream, config):
    return [executable, "-w", str(WIDTH), "-h", str(HEIGHT), "-d", str(DEPTH),
            "-D", "-c", config, source, codestream]


def decoder_command(executable, codestream, decoded):
    return [executable, "-v", codestream, decoded]


def summarize_samples(samples):
    values = [item["seconds"] for item in samples]
    return {
        "count": len(values),
        "min_seconds": min(values),
        "mean_seconds": statistics.fmean(values),
        "p50_seconds": nearest_rank(values, 50),
        "p95_seconds": nearest_rank(values, 95),
        "p99_seconds": nearest_rank(values, 99),
        "max_seconds": max(values),
    }


def run_phase(command, env, warmups, repeats, timeout):
    for _ in range(warmups):
        run(command, env, timeout)
    return [run(command, env, timeout) for _ in range(repeats)]


def host_metadata():
    return {
        "platform": platform.platform(),
        "machine": platform.machine(),
        "processor": platform.processor(),
        "physical_cores": psutil.cpu_count(logical=False),
        "logical_cpus": psutil.cpu_count(logical=True),
        "ram_bytes": psutil.virtual_memory().total,
        "scope": "host-only; RK3588 and physical LAN not tested",
    }
```

Implement `benchmark(bin_dir, data_root, output_root, warmups=3, repeats=10, threads=(1,))` so that it:

1. rejects a non-empty `output_root`;
2. resolves `jxs_encoder` and `jxs_decoder` under `bin_dir`;
3. calls `prepare_inputs(data_root)`;
4. creates intermediates under `data_root / "work"`;
5. runs one correctness encode/decode per case/config/thread;
6. calls `validate_probe()` on combined decoder output;
7. verifies each decoded `.yuv8p` is exactly `FRAME_BYTES`;
8. runs encode and decode warmups separately;
9. records every measured row with case, configuration, thread count, phase, sample index, elapsed seconds, sampled RSS, sampled process threads, codestream bytes, and effective bpp;
10. summarizes every group with `summarize_samples()`;
11. finds the smallest matching encode/decode P50 pair and calls `gate_decision()`;
12. writes `system.json`, copies `input-manifest.json`, and writes `raw-timings.csv`, `summary.json`, and `summary.md`.

The Markdown report must begin exactly with:

```markdown
# Host-only 4K YUV444P8 JPEG XS CLI Screen

> RK3588 was not tested. Physical LAN was not tested. CLI timing includes process startup, file I/O, codec context setup, and codec execution.
```

The report must show the best encode P50, decode P50, combined P50, equivalent sequential fps, the 60 fps requirement (`16.667 ms/frame`), the P99 < 50 ms requirement, and the gate status.

- [ ] **Step 4: Add a command-line entry point**

Add an `argparse` entry point with:

```text
--bin-dir   required Path
--data      default test-data/host-4k444
--output    default reports/host-current
--warmups   default 3, minimum 0
--repeats   default 10, minimum 1
--threads   one or more integers, default 1
--timeout   default 300 seconds
```

Set the measured environment to `OMP_DYNAMIC=FALSE`, `OMP_MAX_ACTIVE_LEVELS=1`, and `OMP_WAIT_POLICY=PASSIVE`; set `OMP_NUM_THREADS` per variant. Exit nonzero on build/output/format/correctness failures. A performance rejection is a valid benchmark result and exits zero.

- [ ] **Step 5: Run all unit tests**

Run:

```bash
cd X-Workspace/jxs
python3 extras/test_host_4k444.py -v
```

Expected: all tests PASS.

### Task 4: Build and Verify the Existing Codec

**Files:**
- Generated: `build-host-serial/`
- Generated if supported: `build-host-parallel/`

- [ ] **Step 1: Configure and build the serial Release binary**

Run:

```bash
cd X-Workspace/jxs
cmake -S . -B build-host-serial -DCMAKE_BUILD_TYPE=Release -DJXS_ENABLE_OPENMP=OFF
cmake --build build-host-serial --parallel 8
```

Expected: `build-host-serial/bin/jxs_encoder` and `jxs_decoder` exist.

- [ ] **Step 2: Run the repository regression suite**

Run:

```bash
cd X-Workspace/jxs
ctest --test-dir build-host-serial --output-on-failure
```

Expected: `codec_regression` PASS. If it fails, stop and report correctness failure; do not publish latency conclusions.

- [ ] **Step 3: Attempt the OpenMP build without installing or changing the toolchain**

Run:

```bash
cd X-Workspace/jxs
cmake -S . -B build-host-parallel -DCMAKE_BUILD_TYPE=Release -DJXS_ENABLE_OPENMP=ON
cmake --build build-host-parallel --parallel 8
```

Expected on this Apple M1 host: configuration may fail because Apple Clang does not include an OpenMP runtime. Record the failure as `OpenMP unavailable on this host`, remove the incomplete `build-host-parallel` directory, and continue with the serial build. Do not install Homebrew packages unless the user separately authorizes dependency changes.

- [ ] **Step 4: Run a one-case correctness smoke test**

Run the runner with one repeat and no warmup into a temporary report directory:

```bash
cd X-Workspace/jxs
rm -rf reports/host-smoke
PYTHONPATH=extras python3 extras/host_4k444.py \
  --bin-dir build-host-serial/bin \
  --data test-data/host-4k444 \
  --output reports/host-smoke \
  --warmups 0 --repeats 1 --threads 1
```

Expected: successful encode/decode for all three inputs and both configurations, decoded size `24,883,200`, and probe sampling `1x1/1x1/1x1`. Delete `reports/host-smoke` after inspection so it cannot be confused with the formal run.

### Task 5: Execute the Formal Host Screen and Apply the Gate

**Files:**
- Generate: `reports/host-current/system.json`
- Generate: `reports/host-current/input-manifest.json`
- Generate: `reports/host-current/raw-timings.csv`
- Generate: `reports/host-current/summary.json`
- Generate: `reports/host-current/summary.md`

- [ ] **Step 1: Ensure the formal output directory is absent**

Run:

```bash
cd X-Workspace/jxs
rm -rf reports/host-current
```

This is safe only because the directory is created by this plan; inspect it first if it unexpectedly exists.

- [ ] **Step 2: Run the formal serial benchmark**

Run:

```bash
cd X-Workspace/jxs
PYTHONPATH=extras python3 extras/host_4k444.py \
  --bin-dir build-host-serial/bin \
  --data test-data/host-4k444 \
  --output reports/host-current \
  --warmups 3 --repeats 10 --threads 1 \
  --timeout 300
```

Expected: 120 measured rows (`3 cases × 2 configurations × 2 phases × 10 repeats`), plus correctness runs and warmups.

- [ ] **Step 3: Validate report integrity**

Run:

```bash
cd X-Workspace/jxs
python3 - <<'PY'
import csv, json
from pathlib import Path
root = Path("reports/host-current")
summary = json.loads((root / "summary.json").read_text())
rows = list(csv.DictReader((root / "raw-timings.csv").open()))
assert len(rows) == 120, len(rows)
assert summary["host_scope"] == "host-only"
assert summary["rk3588_tested"] is False
assert summary["physical_lan_tested"] is False
assert summary["decision"]["status"] in {"reject-current-cli", "needs-in-memory"}
print(summary["decision"])
PY
```

Expected: assertions pass and the decision prints.

- [ ] **Step 4: Apply the stop rule**

If `decision.status` is `reject-current-cli`, stop. Do not implement UDP or an in-memory harness in this plan. Report:

- fastest encode/decode P50/P95/P99;
- combined P50 and equivalent fps;
- factor above the 50 ms target;
- factor below 60 fps;
- explicit statement that RK3588 and physical LAN were not tested;
- conclusion that the current CPU reference implementation is not a viable 4K60/P99<50ms implementation.

If `decision.status` is `needs-in-memory`, write a new design/plan for a persistent library-API harness. Do not infer success from CLI timing.

- [ ] **Step 5: Run final verification**

Run:

```bash
cd X-Workspace/jxs
python3 extras/test_host_4k444.py -v
ctest --test-dir build-host-serial --output-on-failure
git diff --check
git status --short
```

Expected: unit tests and codec regression pass; no whitespace errors; only intended source/docs/report files are modified or untracked. Do not commit unless explicitly requested.

## Recorded Execution Environment Adjustments

The 2026-09-29 Apple M1 execution required two reversible, ignored build-environment adjustments:

1. `build-host-venv/` was created with `--system-site-packages`, then the repository-declared `psutil` and `imageio-ffmpeg` dependencies were installed locally. Global Python environments were not modified.
2. Apple Clang lacks `<malloc.h>`, while this reference source includes it. A generated `build-host-compat/malloc.h` shim containing only `#include <stdlib.h>` was added to `CMAKE_C_FLAGS`; codec source files were not changed.

The OpenMP configure attempt failed because CMake could not find `OpenMP_C_FLAGS`/`OpenMP_C_LIB_NAMES`. The formal report therefore describes an Apple M1 serial Release build and records the failed OpenMP attempt explicitly. The reviewed formal command also passes:

```bash
--openmp-status "attempted; unavailable on AppleClang 21 because FindOpenMP could not find OpenMP_C_FLAGS/OpenMP_C_LIB_NAMES"
```
