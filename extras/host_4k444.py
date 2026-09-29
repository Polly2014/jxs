"""Host-only 4K YUV444P8 JPEG XS correctness and CLI latency screen."""

import argparse
import csv
import hashlib
import json
import math
import os
import platform
import re
import shutil
import statistics
import subprocess
import urllib.request
from pathlib import Path

import numpy as np
import psutil
from PIL import Image, ImageOps, __version__ as PILLOW_VERSION

from evaluate import run

WIDTH = 3840
HEIGHT = 2160
DEPTH = 8
COMPONENTS = 3
FRAME_BYTES = WIDTH * HEIGHT * COMPONENTS
NATURAL_URL = "https://images-assets.nasa.gov/image/PIA04921/PIA04921~orig.jpg"
NATURAL_SHA256 = "4c2bc14dccd339b73c55cba14d3c18a63c4609041c375be18c442ee13153a274"
CONFIGURATIONS = {
    "full-width": "p=unrestricted;rate=4;bw=20;fq=8;cpih=none;cw=0;nly=2",
    "columns": "p=unrestricted;rate=4;bw=20;fq=8;cpih=none;cw=4;nly=1",
}


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
            [
                curl,
                "--fail",
                "--location",
                "--silent",
                "--show-error",
                "--max-time",
                "600",
                "--output",
                str(partial),
                url,
            ],
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
        entry = {
            "name": name,
            "input": path.name,
            "width": WIDTH,
            "height": HEIGHT,
            "depth": DEPTH,
            "format": "YUV444P8",
            "bytes": path.stat().st_size,
            "sha256": digest_file(path),
            "operation": operation,
        }
        if name == "high-frequency":
            entry["seed"] = 3588
            entry["generator"] = "numpy.random.default_rng (PCG64)"
        if name == "natural":
            entry["conversion"] = "Pillow RGB to YCbCr (JPEG/JFIF convention), full-range uint8 planes"
        manifest.append(entry)
    payload = {
        "source": {"url": NATURAL_URL, "sha256": NATURAL_SHA256},
        "libraries": {"numpy": np.__version__, "pillow": PILLOW_VERSION},
        "cases": manifest,
    }
    (root / "input-manifest.json").write_text(
        json.dumps(payload, indent=2) + "\n", encoding="utf-8"
    )
    return payload


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


def parse_resolved_config(output):
    match = re.search(r'^Configuration: "([^"]+)"$', output, flags=re.MULTILINE)
    if not match:
        raise ValueError("encoder configuration dump is missing")
    return match.group(1)


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


def encoder_command(executable, source, codestream, config):
    return [
        executable,
        "-w",
        str(WIDTH),
        "-h",
        str(HEIGHT),
        "-d",
        str(DEPTH),
        "-D",
        "-c",
        config,
        source,
        codestream,
    ]


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


def _command_output(command):
    try:
        return subprocess.check_output(command, text=True, stderr=subprocess.STDOUT).strip()
    except (OSError, subprocess.CalledProcessError):
        return None


def _cmake_settings(build_dir):
    cache = Path(build_dir) / "CMakeCache.txt"
    keys = {
        "CMAKE_BUILD_TYPE",
        "CMAKE_C_COMPILER",
        "CMAKE_C_FLAGS",
        "CMAKE_C_FLAGS_RELEASE",
        "CMAKE_GENERATOR",
        "JXS_ENABLE_OPENMP",
        "OpenMP_C_FLAGS",
    }
    settings = {}
    if cache.is_file():
        for line in cache.read_text(encoding="utf-8").splitlines():
            if ":" in line and "=" in line:
                key = line.split(":", 1)[0]
                if key in keys:
                    settings[key] = line.split("=", 1)[1]
    return settings


def host_metadata(build_dir, openmp_status):
    repo_root = Path(__file__).resolve().parents[1]
    settings = _cmake_settings(build_dir)
    compiler = settings.get("CMAKE_C_COMPILER")
    compiler_version = _command_output([compiler, "--version"]) if compiler else None
    cpu_model = _command_output(["sysctl", "-n", "machdep.cpu.brand_string"])
    metadata = {
        "platform": platform.platform(),
        "machine": platform.machine(),
        "processor": platform.processor(),
        "cpu_model": cpu_model,
        "physical_cores": psutil.cpu_count(logical=False),
        "logical_cpus": psutil.cpu_count(logical=True),
        "ram_bytes": psutil.virtual_memory().total,
        "scope": "host-only; RK3588 and physical LAN not tested",
        "git_commit": _command_output(["git", "-C", str(repo_root), "rev-parse", "HEAD"]),
        "cmake": settings,
        "compiler_version": compiler_version,
        "openmp_status": openmp_status,
    }
    if "build-host-compat" in settings.get("CMAKE_C_FLAGS", ""):
        metadata["compatibility_note"] = (
            "macOS build used a generated malloc.h shim that includes stdlib.h; codec source was unchanged"
        )
    return metadata


def _write_csv(path, rows):
    fields = [
        "case",
        "configuration",
        "requested_config",
        "resolved_config",
        "threads",
        "phase",
        "sample",
        "seconds",
        "sampled_peak_rss_bytes",
        "sampled_peak_threads",
        "codestream_bytes",
        "effective_bpp",
    ]
    with Path(path).open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def _markdown_report(summary):
    best = summary["best"]
    decision = summary["decision"]
    return "\n".join(
        [
            "# Host-only 4K YUV444P8 JPEG XS CLI Screen",
            "",
            "> RK3588 was not tested. Physical LAN was not tested. CLI timing includes process startup, file I/O, codec context setup, and codec execution.",
            "",
            "## Target",
            "",
            "- 3840×2160 YUV444P8 at 4 bpp",
            "- sustained throughput ≥ 60 fps (16.667 ms frame interval)",
            "- encode-start to decoded-frame-complete P99 < 50 ms",
            "",
            "## Fastest CLI configuration",
            "",
            f"- Case: `{best['case']}`",
            f"- Configuration: `{best['configuration']}`",
            f"- Requested config: `{best['requested_config']}`",
            f"- Resolved config: `{best['resolved_config']}`",
            f"- Threads requested: {best['threads']}",
            f"- Encode P50/P95/P99: {best['encode']['p50_seconds']:.6f} / {best['encode']['p95_seconds']:.6f} / {best['encode']['p99_seconds']:.6f} s",
            f"- Decode P50/P95/P99: {best['decode']['p50_seconds']:.6f} / {best['decode']['p95_seconds']:.6f} / {best['decode']['p99_seconds']:.6f} s",
            f"- Encode + decode P50: {best['combined_p50_seconds']:.6f} s",
            f"- Equivalent sequential throughput: {best['equivalent_fps']:.3f} fps",
            f"- Optimistic two-stage upper bound: {best['optimistic_two_stage_fps']:.3f} fps",
            f"- Combined P50 / 50 ms target: {best['p50_vs_50ms_factor']:.1f}×",
            f"- 60 fps / sequential throughput: {best['throughput_shortfall_factor']:.1f}×",
            "",
            "## Statistical scope",
            "",
            "- Percentiles use nearest-rank order statistics.",
            f"- {summary['statistics']['caveat']}.",
            "- Sustained 60 fps pacing, queue growth, RK3588 behavior, and physical-LAN behavior were not measured.",
            "",
            "## Screening decision",
            "",
            f"- Status: `{decision['status']}`",
            f"- Reason: {decision['reason']}",
            "",
            "This report cannot establish RK3588 performance or physical-LAN latency.",
            "",
        ]
    )


def benchmark(
    bin_dir,
    data_root,
    output_root,
    warmups=3,
    repeats=10,
    threads=(1,),
    timeout=300,
    openmp_status="not evaluated",
):
    bin_dir = Path(bin_dir).resolve()
    data_root = Path(data_root).resolve()
    output_root = Path(output_root).resolve()
    if output_root.exists() and any(output_root.iterdir()):
        raise ValueError(f"Output directory must be new or empty: {output_root}")
    output_root.mkdir(parents=True, exist_ok=True)

    encoder = bin_dir / "jxs_encoder"
    decoder = bin_dir / "jxs_decoder"
    if not encoder.is_file() or not decoder.is_file():
        raise FileNotFoundError(f"Missing encoder/decoder under {bin_dir}")

    manifest = prepare_inputs(data_root)
    work = data_root / "work"
    work.mkdir(parents=True, exist_ok=True)
    env_base = dict(os.environ)
    env_base.update(OMP_DYNAMIC="FALSE", OMP_MAX_ACTIVE_LEVELS="1", OMP_WAIT_POLICY="PASSIVE")

    raw_rows = []
    groups = []
    for case in manifest["cases"]:
        source = data_root / case["input"]
        for configuration, config in CONFIGURATIONS.items():
            for thread_count in dict.fromkeys(threads):
                tag = f"{case['name']}-{configuration}-t{thread_count}"
                codestream = work / f"{tag}.jxs"
                decoded = work / f"{tag}.decoded.yuv8p"
                env = dict(env_base, OMP_NUM_THREADS=str(thread_count))
                enc_command = encoder_command(encoder, source, codestream, config)
                dec_command = decoder_command(decoder, codestream, decoded)

                print(f"Correctness: {tag}", flush=True)
                encoder_probe = run(enc_command, env, timeout)
                resolved_config = parse_resolved_config(encoder_probe["output"])
                probe = run(dec_command, env, timeout)
                validate_probe(probe["output"])
                if decoded.stat().st_size != FRAME_BYTES:
                    raise RuntimeError(f"Unexpected decoded frame size: {decoded}")

                codestream_bytes = codestream.stat().st_size
                effective_bpp = codestream_bytes * 8 / (WIDTH * HEIGHT)
                print(f"Timing: {tag}", flush=True)
                encode_samples = run_phase(enc_command, env, warmups, repeats, timeout)
                decode_samples = run_phase(dec_command, env, warmups, repeats, timeout)
                encode_summary = summarize_samples(encode_samples)
                decode_summary = summarize_samples(decode_samples)

                for phase, samples in (("encode", encode_samples), ("decode", decode_samples)):
                    for sample_index, sample in enumerate(samples, start=1):
                        raw_rows.append(
                            {
                                "case": case["name"],
                                "configuration": configuration,
                                "requested_config": config,
                                "resolved_config": resolved_config,
                                "threads": thread_count,
                                "phase": phase,
                                "sample": sample_index,
                                "seconds": sample["seconds"],
                                "sampled_peak_rss_bytes": sample["sampled_peak_rss_bytes"],
                                "sampled_peak_threads": sample["sampled_peak_threads"],
                                "codestream_bytes": codestream_bytes,
                                "effective_bpp": effective_bpp,
                            }
                        )
                combined = encode_summary["p50_seconds"] + decode_summary["p50_seconds"]
                sequential_fps = 1 / combined
                optimistic_two_stage_fps = 1 / max(
                    encode_summary["p50_seconds"], decode_summary["p50_seconds"]
                )
                groups.append(
                    {
                        "case": case["name"],
                        "configuration": configuration,
                        "requested_config": config,
                        "resolved_config": resolved_config,
                        "threads": thread_count,
                        "codestream_bytes": codestream_bytes,
                        "effective_bpp": effective_bpp,
                        "encode": encode_summary,
                        "decode": decode_summary,
                        "combined_p50_seconds": combined,
                        "combined_p99_bound_seconds": (
                            encode_summary["p99_seconds"] + decode_summary["p99_seconds"]
                        ),
                        "equivalent_fps": sequential_fps,
                        "optimistic_two_stage_fps": optimistic_two_stage_fps,
                        "p50_vs_50ms_factor": combined / 0.050,
                        "throughput_shortfall_factor": 60 / sequential_fps,
                    }
                )

    best = min(groups, key=lambda item: item["combined_p50_seconds"])
    decision = gate_decision(best["encode"]["p50_seconds"], best["decode"]["p50_seconds"])
    system = host_metadata(bin_dir.parent, openmp_status)
    system.update(
        {
            "encoder_sha256": digest_file(encoder),
            "decoder_sha256": digest_file(decoder),
            "warmups": warmups,
            "repeats": repeats,
            "threads": list(dict.fromkeys(threads)),
            "timing_scope": "separate CLI wall times including startup, file I/O, context setup, and codec",
        }
    )
    summary = {
        "host_scope": "host-only",
        "rk3588_tested": False,
        "physical_lan_tested": False,
        "target": {
            "width": WIDTH,
            "height": HEIGHT,
            "format": "YUV444P8",
            "rate_bpp": 4,
            "fps": 60,
            "end_to_end_p99_limit_ms": 50,
        },
        "statistics": {
            "percentile_method": "nearest-rank",
            "samples_per_phase": repeats,
            "caveat": (
                f"With {repeats} samples nearest-rank P95/P99 may collapse to the maximum; "
                "this is a screening run, not tail-latency qualification"
            ),
        },
        "measurement_limits": {
            "sustained_pacing_tested": False,
            "queue_growth_tested": False,
            "rk3588_tested": False,
            "physical_lan_tested": False,
        },
        "groups": groups,
        "best": best,
        "decision": decision,
    }

    (output_root / "system.json").write_text(json.dumps(system, indent=2) + "\n", encoding="utf-8")
    shutil.copy2(data_root / "input-manifest.json", output_root / "input-manifest.json")
    _write_csv(output_root / "raw-timings.csv", raw_rows)
    (output_root / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    (output_root / "summary.md").write_text(_markdown_report(summary), encoding="utf-8")
    return summary


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin-dir", type=Path, required=True)
    parser.add_argument("--data", type=Path, default=Path("test-data/host-4k444"))
    parser.add_argument("--output", type=Path, default=Path("reports/host-current"))
    parser.add_argument("--warmups", type=int, default=3)
    parser.add_argument("--repeats", type=int, default=10)
    parser.add_argument("--threads", type=int, nargs="+", default=[1])
    parser.add_argument("--timeout", type=float, default=300)
    parser.add_argument("--openmp-status", default="not evaluated")
    args = parser.parse_args()
    if args.warmups < 0:
        parser.error("--warmups must be nonnegative")
    if args.repeats < 1:
        parser.error("--repeats must be positive")
    if any(thread < 1 for thread in args.threads):
        parser.error("--threads values must be positive")
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    return args


def main():
    args = parse_args()
    summary = benchmark(
        args.bin_dir,
        args.data,
        args.output,
        warmups=args.warmups,
        repeats=args.repeats,
        threads=args.threads,
        timeout=args.timeout,
        openmp_status=args.openmp_status,
    )
    print(json.dumps(summary["decision"], indent=2))


if __name__ == "__main__":
    main()
