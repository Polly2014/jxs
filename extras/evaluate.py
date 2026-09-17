"""Compare an untouched reference build with a candidate using real-media E2E runs."""

import argparse
import json
import math
import os
import platform
import random
import statistics
import subprocess
import tempfile
import time
from pathlib import Path

import numpy as np
import psutil
from PIL import Image

from prepare_media import digest


def executable(directory, name):
    path = directory / (name + (".exe" if os.name == "nt" else ""))
    if not path.is_file():
        raise FileNotFoundError(path)
    return path


def build_settings(directory):
    cache = directory.parent / "CMakeCache.txt"
    if not cache.is_file():
        return {"available": False}
    keys = {"CMAKE_BUILD_TYPE", "CMAKE_C_COMPILER", "CMAKE_C_FLAGS", "CMAKE_C_FLAGS_RELEASE",
            "CMAKE_GENERATOR", "JXS_ENABLE_OPENMP", "OpenMP_C_FLAGS"}
    settings = {}
    for line in cache.read_text(encoding="utf-8").splitlines():
        if ":" in line and "=" in line and line.split(":", 1)[0] in keys:
            settings[line.split(":", 1)[0]] = line.split("=", 1)[1]
    return settings


def run(command, env, timeout, expect_success=True):
    # File-backed output avoids pipe deadlocks and excludes console rendering.
    with tempfile.TemporaryFile() as log:
        start = time.perf_counter()
        process = subprocess.Popen([str(arg) for arg in command], env=env, stdout=log, stderr=log)
        observed = psutil.Process(process.pid)
        peak_rss = 0
        peak_threads = 0
        while process.poll() is None:
            if time.perf_counter() - start > timeout:
                process.kill()
                process.wait()
                raise TimeoutError(f"Command exceeded {timeout}s: {command}")
            try:
                peak_rss = max(peak_rss, observed.memory_info().rss)
                peak_threads = max(peak_threads, observed.num_threads())
            except psutil.NoSuchProcess:
                break
            time.sleep(0.002)
        code = process.wait()
        elapsed = time.perf_counter() - start
        log.seek(0)
        output = log.read().decode("utf-8", errors="replace")
    if (code == 0) != expect_success:
        raise RuntimeError(f"Unexpected return code {code}: {command}\n{output}")
    return {"seconds": elapsed, "sampled_peak_rss_bytes": peak_rss,
            "sampled_peak_threads": peak_threads, "returncode": code, "output": output}


def pixels(path):
    with Image.open(path) as image:
        return np.asarray(image).astype(np.int32)


def decode_files(directory, seq_args, codestream, image, env, timeout, individual):
    if not individual:
        return run([executable(directory, "jxs_decoder"), *seq_args, codestream, image], env, timeout)
    # The original sequence decoder stops silently when a later frame is larger.
    # Use identical per-file invocation scope on BOTH builds for variable-size streams.
    results = [
        run([executable(directory, "jxs_decoder"),
             str(codestream) % index, str(image) % index], env, timeout)
        for index in range(1, individual + 1)
    ]
    return {
        "seconds": sum(r["seconds"] for r in results),
        "sampled_peak_rss_bytes": max(r["sampled_peak_rss_bytes"] for r in results),
        "sampled_peak_threads": max(r["sampled_peak_threads"] for r in results),
    }


def reconstruction(source, decoded, depth):
    a, b = pixels(source), pixels(decoded)
    if a.shape != b.shape:
        raise AssertionError(f"Decoded dimensions/components changed: {source}")
    diff = a - b
    mse = float(np.mean(diff.astype(np.float64) ** 2))
    return {
        "max_abs_error": int(np.abs(diff).max()),
        "different_samples": int(np.count_nonzero(diff)),
        "psnr_db": None if mse == 0 else 10 * math.log10(((1 << depth) - 1) ** 2 / mse),
        "exact_reconstruction": mse == 0,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", type=Path, required=True, help="Untouched build's bin directory")
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--data", type=Path, default=Path("test-data"))
    parser.add_argument("--output", type=Path, default=Path("benchmark-results"))
    parser.add_argument("--threads", type=int, nargs="+", default=[1, 2, 4, 8])
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--warmups", type=int, default=1)
    parser.add_argument("--timeout", type=float, default=180)
    parser.add_argument("--cases", nargs="+", help="Optional subset of case names")
    parser.add_argument("--modes", nargs="+", choices=["main", "columns", "lossless"],
                        default=["main", "columns", "lossless"])
    args = parser.parse_args()
    if args.repeats < 1 or args.warmups < 0 or any(t < 1 or t > 256 for t in args.threads):
        parser.error("Positive repeats and thread counts [1, 256], nonnegative warmups required")
    root, out = args.data.resolve(), args.output.resolve()
    if out.exists() and any(out.iterdir()):
        parser.error("--output must be a new or empty directory (avoids stale sequence frames)")
    out.mkdir(parents=True, exist_ok=True)
    manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
    for item in manifest["cases"]:
        if digest(root / item["input"]) != item["sha256"]:
            raise AssertionError(f"Input hash mismatch: {item['input']}")
    cases = [item for item in manifest["cases"] if not item["name"].startswith("bbb_")]
    frames = [item for item in manifest["cases"] if item["name"].startswith("bbb_")]
    if frames:
        cases.append({"name": "bbb_sequence", "input": "bbb_%04d.ppm",
                      "width": frames[0]["width"], "height": frames[0]["height"], "frames": frames})
    if args.cases:
        unknown = set(args.cases) - {item["name"] for item in cases}
        if unknown:
            parser.error(f"Unknown cases: {sorted(unknown)}")
        cases = [item for item in cases if item["name"] in args.cases]
    reference = args.reference.resolve()
    candidate = args.candidate.resolve()
    base_variants = [("original", reference, 1)] + [
        (f"parallel_{threads}", candidate, threads) for threads in dict.fromkeys(args.threads)
    ]
    binaries = {
        str(directory): {name: digest(executable(directory, name))
                         for name in ("jxs_encoder", "jxs_decoder")}
        for directory in (reference, candidate)
    }
    env_base = dict(os.environ)
    env_base.update(OMP_DYNAMIC="FALSE", OMP_MAX_ACTIVE_LEVELS="1")
    report = {
        "host": {"platform": platform.platform(), "processor": platform.processor(),
                 "physical_cores": psutil.cpu_count(logical=False), "logical_cpus": psutil.cpu_count(),
                 "ram_bytes": psutil.virtual_memory().total,
                 "allowed_cpus": psutil.Process().cpu_affinity()},
        "target": "RK3588 not measured unless this script actually runs on that board",
        "timing_scope": "CLI wall time including process startup, image I/O, context setup and codec; "
                        "warm filesystem cache; encode/decode measured separately; 2ms polling granularity",
        "memory_scope": "Sampled child-process RSS (lower bound on peak), not whole system memory",
        "sequence_scope": "Consecutive source frames; parallel_N uses frame-internal parallelism; "
                          "frames_N uses -j N concurrent encoders and single-threaded decoder. "
                          "Throughput is batch average, not per-frame latency",
        "repeats": args.repeats, "warmups": args.warmups, "seed": 3588,
        "openmp_env": {key: value for key, value in env_base.items()
                       if key.startswith(("OMP_", "GOMP_"))},
        "input_manifest_sha256": digest(root / "manifest.json"), "binaries": binaries,
        "build_settings": {str(directory): build_settings(directory) for directory in (reference, candidate)},
        "input_cases": manifest["cases"],
        "sources": manifest["sources"], "results": [], "negative_tests": [],
    }
    rng = random.Random(3588)
    for case in cases:
        variants = list(base_variants)
        if "frames" in case:
            variants += [(f"frames_{threads}", candidate, threads)
                         for threads in dict.fromkeys(args.threads) if threads > 1]
        sources = [root / item["input"] for item in case.get("frames", [case])]
        count = len(sources)
        width = case["width"]
        # Reference validation rejects exact column-width divisibility. These
        # settings keep an explicit legal final column, without modifying it.
        cw = 4 if width == 3840 else (3 if width == 512 else 1)
        cpih = "none" if case["input"].endswith(".pgm") else "rct"
        depth = case.get("depth", 8)
        configurations = {
            "main": f"p=unrestricted;rate=6;bw=20;fq=8;cpih={cpih};cw=0;nly=2",
            "columns": f"p=unrestricted;rate=6;bw=20;fq=8;cpih={cpih};cw={cw};nly=1",
            "lossless": f"p=unrestricted;size=-1;bw={depth};fq=0;cpih={cpih};cw={cw};nly=1",
        }
        for mode in args.modes:
            cfg = configurations[mode]
            if cpih == "none":
                bands = 10 if mode == "main" else 8
                cfg += ";gains=" + ",".join(["0"] * bands)
                cfg += ";priorities=" + ",".join(str(i) for i in range(bands))
            case_dir = out / case["name"] / mode
            case_dir.mkdir(parents=True, exist_ok=True)
            streams = {}
            decoded = {}
            metrics = {variant: [] for variant, _, _ in variants}
            correctness = {}
            for round_index in range(args.warmups + args.repeats):
                order = list(variants)
                if round_index != 0:
                    rng.shuffle(order)
                for variant, directory, threads in order:
                    dest = case_dir / variant
                    dest.mkdir(exist_ok=True)
                    sequence = "frames" in case
                    codestream = dest / ("frame_%04d.jxs" if sequence else "image.jxs")
                    suffix = ".pgm" if sources[0].suffix == ".pgm" else ".ppm"
                    image = dest / (("frame_%04d" if sequence else "image") + suffix)
                    seq_args = ["-f", "1", "-n", str(count)] if sequence else []
                    frame_workers = variant.startswith("frames_")
                    env = dict(env_base, OMP_NUM_THREADS="1" if frame_workers else str(threads))
                    worker_args = ["-j", str(threads)] if frame_workers else []
                    enc = run([executable(directory, "jxs_encoder"), "-v", "-c", cfg,
                               *worker_args, *seq_args, root / case["input"], codestream], env, args.timeout)
                    individual = count if sequence and mode == "lossless" else 0
                    dec = decode_files(directory, seq_args, codestream, image, env, args.timeout, individual)
                    encoded_files = [dest / (f"frame_{i:04d}.jxs" if sequence else "image.jxs")
                                     for i in range(1, count + 1)]
                    image_files = [dest / (f"frame_{i:04d}{suffix}" if sequence else "image" + suffix)
                                   for i in range(1, count + 1)]
                    stream_hashes = [digest(path) for path in encoded_files]
                    image_hashes = [digest(path) for path in image_files]
                    if variant == "original" and round_index == 0:
                        streams[mode], decoded[mode] = stream_hashes, image_hashes
                    if stream_hashes != streams[mode] or image_hashes != decoded[mode]:
                        raise AssertionError(f"Nondeterministic/different bytes: {case['name']} {mode} {variant}")
                    if round_index == 0:
                        quality = [reconstruction(src, dst, depth) for src, dst in zip(sources, image_files)]
                        if mode == "lossless" and not all(q["exact_reconstruction"] for q in quality):
                            raise AssertionError(f"Lossless reconstruction failed: {case['name']} {variant}")
                        # Cross-decode with the untouched implementation, not just the candidate.
                        cross = dest / "reference_decode"
                        cross.mkdir(exist_ok=True)
                        cross_image = cross / image.name
                        decode_files(reference, seq_args, codestream, cross_image,
                                     dict(env_base, OMP_NUM_THREADS="1"), args.timeout, individual)
                        if [digest(cross / path.name) for path in image_files] != image_hashes:
                            raise AssertionError("Reference cross-decode mismatch")
                        correctness[variant] = {
                            "codestream_sha256": stream_hashes, "decoded_sha256": image_hashes,
                            "source_reconstruction": quality,
                            "encoded_bytes": sum(path.stat().st_size for path in encoded_files),
                            "identical_to_original": True, "reference_cross_decode": True,
                        }
                    if round_index >= args.warmups:
                        metrics[variant].append({
                            "encode_s": enc["seconds"], "decode_s": dec["seconds"],
                            "encode_peak_rss": enc["sampled_peak_rss_bytes"],
                            "decode_peak_rss": dec["sampled_peak_rss_bytes"],
                            "encode_peak_threads": enc["sampled_peak_threads"],
                            "decode_peak_threads": dec["sampled_peak_threads"],
                        })
            original_enc = statistics.median(r["encode_s"] for r in metrics["original"])
            original_dec = statistics.median(r["decode_s"] for r in metrics["original"])
            original_e2e = statistics.median(r["encode_s"] + r["decode_s"] for r in metrics["original"])
            for variant, _, threads in variants:
                runs = metrics[variant]
                enc = statistics.median(r["encode_s"] for r in runs)
                dec = statistics.median(r["decode_s"] for r in runs)
                e2e = statistics.median(r["encode_s"] + r["decode_s"] for r in runs)
                row = {
                    "case": case["name"], "mode": mode, "config": cfg, "variant": variant,
                    "requested_threads": threads, "width": case["width"], "height": case["height"],
                    "column_count": 1 if mode == "main" else math.ceil(width / (256 * cw)),
                    "depth": depth, "strategy": "frames" if variant.startswith("frames_") else "intra-frame",
                    "decoder_invocations": count if "frames" in case and mode == "lossless" else 1,
                    "frame_count": count, "runs": runs, "encode_median_s": enc, "decode_median_s": dec,
                    "encode_p95_s": float(np.percentile([r["encode_s"] for r in runs], 95)),
                    "decode_p95_s": float(np.percentile([r["decode_s"] for r in runs], 95)),
                    "encode_fps": count / enc, "decode_fps": count / dec,
                    "encode_speedup": original_enc / enc, "decode_speedup": original_dec / dec,
                    "codec_roundtrip_median_s": e2e, "codec_roundtrip_speedup": original_e2e / e2e,
                    "max_sampled_encode_rss_mib": max(r["encode_peak_rss"] for r in runs) / 2**20,
                    "correctness": correctness[variant],
                }
                report["results"].append(row)
                print(f"{case['name']:15} {mode:8} {variant:10} enc={enc:.4f}s "
                      f"{row['encode_speedup']:.2f}x dec={dec:.4f}s {row['decode_speedup']:.2f}x",
                      flush=True)
            (out / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")

    bad_input = out / "not-an-image.ppm"
    bad_input.write_bytes(b"not a PPM image\n")
    for variant, directory, threads in base_variants:
        env = dict(env_base, OMP_NUM_THREADS=str(threads))
        for name, arguments in [
            ("missing-input", [root / "does-not-exist.ppm"]),
            ("malformed-input", [bad_input]),
            ("invalid-config", ["-c", "no_such_option=1", root / "astronaut.ppm"]),
            ("overlong-config", ["-c", "x" * 5000, root / "astronaut.ppm"]),
            ("column-rate-failure", ["-c", "p=unrestricted;rate=0.05;bw=20;fq=8;nly=1;cw=1",
                                     root / "astronaut_odd.pgm"]),
        ]:
            result = run([executable(directory, "jxs_encoder"), *arguments, out / "invalid.jxs"],
                         env, args.timeout, expect_success=False)
            report["negative_tests"].append({"variant": variant, "case": name,
                                              "returncode": result["returncode"]})
    report["complete"] = True
    (out / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    lines = [
        "Host E2E comparison; these are NOT RK3588 measurements.",
        report["timing_scope"], report["sequence_scope"],
        "case mode variant encode_s encode_speedup decode_s decode_speedup",
    ]
    lines += [
        f"{r['case']} {r['mode']} {r['variant']} {r['encode_median_s']:.6f} "
        f"{r['encode_speedup']:.3f} {r['decode_median_s']:.6f} {r['decode_speedup']:.3f}"
        for r in report["results"]
    ]
    (out / "summary.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"Completed {len(report['results'])} result rows and "
          f"{len(report['negative_tests'])} negative tests: {out / 'results.json'}")


if __name__ == "__main__":
    main()
