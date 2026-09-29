"""macOS OpenMP latency and aggregate-throughput screen for 4K YUV444P8."""

import argparse
import csv
import json
import os
import shutil
import subprocess
from pathlib import Path

import psutil

from evaluate import run
from host_4k444 import (
    CONFIGURATIONS,
    DEPTH,
    FRAME_BYTES,
    HEIGHT,
    WIDTH,
    digest_file,
    host_metadata,
    nearest_rank,
)


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
        executable,
        "-w",
        str(WIDTH),
        "-h",
        str(HEIGHT),
        "-d",
        str(DEPTH),
        "-j",
        str(workers),
        "-f",
        "1",
        "-n",
        str(frames),
        "-c",
        config,
        source_pattern,
        output_pattern,
    ]


def decoder_sequence_command(executable, source_pattern, output_pattern, frames):
    return [executable, "-f", "1", "-n", str(frames), source_pattern, output_pattern]


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


def best_compatible_pipeline(throughput_groups):
    configurations = sorted({group["configuration"] for group in throughput_groups})
    pairs = []
    for configuration in configurations:
        encoders = [
            group
            for group in throughput_groups
            if group["phase"] == "encode" and group["configuration"] == configuration
        ]
        decoders = [
            group
            for group in throughput_groups
            if group["phase"] == "decode" and group["configuration"] == configuration
        ]
        if not encoders or not decoders:
            raise ValueError(f"missing compatible encoder/decoder groups for {configuration}")
        encoder = max(encoders, key=lambda group: group["p50_fps"])
        decoder = max(decoders, key=lambda group: group["p50_fps"])
        pairs.append(
            {
                "configuration": configuration,
                "encoder": encoder,
                "decoder": decoder,
                "optimistic_fps": optimistic_pipeline_fps(
                    encoder["p50_fps"], decoder["p50_fps"]
                ),
            }
        )
    return max(pairs, key=lambda pair: pair["optimistic_fps"]), pairs


def validate_run_controls(systems):
    if not systems:
        raise ValueError("at least one system record is required")
    keys = ("frames", "warmups", "repeats", "workers", "threads")
    baseline = systems[0]
    for other in systems[1:]:
        for key in keys:
            if other.get(key) != baseline.get(key):
                raise ValueError(f"partial reports disagree on {key}")


def add_latency_speedups(latency_groups, serial_groups):
    omp_one = {
        (group["case"], group["configuration"]): group["combined_p50_seconds"]
        for group in latency_groups
        if group["threads"] == 1
    }
    serial = {
        (group["case"], group["configuration"]): group["combined_p50_seconds"]
        for group in serial_groups
        if group["threads"] == 1
    }
    if set(omp_one) != set(serial):
        raise ValueError("OpenMP and serial latency groups do not cover the same cases/configurations")
    enriched = []
    for group in latency_groups:
        key = (group["case"], group["configuration"])
        updated = dict(group)
        updated["speedup_vs_openmp_1"] = omp_one[key] / group["combined_p50_seconds"]
        updated["speedup_vs_serial"] = serial[key] / group["combined_p50_seconds"]
        updated["openmp_1_vs_serial"] = serial[key] / omp_one[key]
        enriched.append(updated)
    return enriched


def resolved_by_configuration(latency_groups):
    resolved = {}
    for configuration in sorted({group["configuration"] for group in latency_groups}):
        values = {
            group["resolved_config"]
            for group in latency_groups
            if group["configuration"] == configuration
        }
        if len(values) != 1:
            raise ValueError(
                f"latency groups disagree on resolved configuration for {configuration}"
            )
        resolved[configuration] = values.pop()
    return resolved


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


def _clean_generated(directory, suffix):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    for path in directory.glob(f"*{suffix}"):
        if not path.is_file():
            raise ValueError(f"refusing to remove non-file output: {path}")
        path.unlink()


def _verify_codestreams(directory, frames):
    files = [Path(directory) / f"stream_{index:04d}.jxs" for index in range(1, frames + 1)]
    missing = [path for path in files if not path.is_file() or path.stat().st_size <= 0]
    if missing:
        raise RuntimeError(f"missing or empty codestream outputs: {missing[:3]}")
    return files


def _verify_decoded(directory, frames):
    files = [Path(directory) / f"decoded_{index:04d}.yuv8p" for index in range(1, frames + 1)]
    invalid = [path for path in files if not path.is_file() or path.stat().st_size != FRAME_BYTES]
    if invalid:
        raise RuntimeError(f"missing or invalid decoded outputs: {invalid[:3]}")
    return files


def _run_group(command, env, warmups, repeats, timeout, before_each, verify_each):
    for _ in range(warmups):
        before_each()
        run(command, env, timeout)
        verify_each()
    samples = []
    for _ in range(repeats):
        before_each()
        samples.append(run(command, env, timeout))
        verify_each()
    return samples


def _parallel_improved(latency_groups):
    baselines = {
        (group["case"], group["configuration"]): group["combined_p50_seconds"]
        for group in latency_groups
        if group["threads"] == 1
    }
    return any(
        group["threads"] > 1
        and group["combined_p50_seconds"]
        < baselines[(group["case"], group["configuration"])]
        for group in latency_groups
    )


def _write_csv(path, rows):
    fields = [
        "phase",
        "configuration",
        "requested_config",
        "resolved_config",
        "parallelism",
        "sample",
        "frames",
        "seconds",
        "fps",
        "sampled_peak_rss_bytes",
        "sampled_peak_threads",
        "output_count",
        "output_bytes",
    ]
    with Path(path).open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def _markdown(summary):
    latency = summary["best_latency"]
    encoder = summary["best_encoder_throughput"]
    decoder = summary["best_decoder_throughput"]
    pipeline = summary["best_compatible_pipeline"]
    decision = summary["decision"]
    return "\n".join(
        [
            "# macOS OpenMP 4K YUV444P8 JPEG XS Screen",
            "",
            "> Apple M1/macOS was tested. RK3588 and physical LAN were not tested.",
            "",
            "## Single-frame intra-frame OpenMP latency",
            "",
            f"- Best case/config/threads: `{latency['case']}` / `{latency['configuration']}` / {latency['threads']}",
            f"- Encode P50/P95/P99: {latency['encode']['p50_seconds']:.6f} / {latency['encode']['p95_seconds']:.6f} / {latency['encode']['p99_seconds']:.6f} s",
            f"- Decode P50/P95/P99: {latency['decode']['p50_seconds']:.6f} / {latency['decode']['p95_seconds']:.6f} / {latency['decode']['p99_seconds']:.6f} s",
            f"- Combined P50: {latency['combined_p50_seconds']:.6f} s",
            f"- Speedup vs OpenMP 1-thread: {latency['speedup_vs_openmp_1']:.3f}×",
            f"- Speedup vs Apple-Clang serial baseline: {latency['speedup_vs_serial']:.3f}×",
            "",
            "## Aggregate 60-frame throughput",
            "",
            f"- Measurement repetitions: {summary['throughput_method']['repeats']} formal run(s) after {summary['throughput_method']['warmups']} warm-up run(s) per group",
            f"- Frames per run: {summary['throughput_method']['frames']}",
            f"- Best encoder: `{encoder['configuration']}`, -j {encoder['parallelism']}, {encoder['p50_fps']:.3f} fps",
            f"- Best decoder: `{decoder['configuration']}`, OMP {decoder['parallelism']}, {decoder['p50_fps']:.3f} fps",
            f"- Best compatible pipeline configuration: `{pipeline['configuration']}`",
            f"- Optimistic two-stage upper bound: {summary['optimistic_two_stage_fps']:.3f} fps",
            "",
            "The throughput result is a batch average. It does not reduce one frame's measured latency.",
            "Decoder timing includes decoded-frame file output. The two-stage value is not a simultaneous network pipeline measurement.",
            "",
            "## Decision gates",
            "",
            f"- Status: `{decision['status']}`",
            f"- Latency ≤100 ms: {decision['latency_gate']}",
            f"- Optimistic throughput ≥60 fps: {decision['throughput_gate']}",
            f"- Parallel scaling improves: {decision['parallel_scaling_gate']}",
            f"- Peak RSS <75% RAM: {decision['memory_gate']}",
            "",
        ]
    )


def benchmark_throughput(
    bin_dir,
    data_root,
    output_root,
    serial_report,
    frames=60,
    workers=(1, 2, 4, 8),
    threads=(1, 2, 4, 8),
    warmups=1,
    repeats=3,
    timeout=600,
    configurations=None,
):
    bin_dir = Path(bin_dir).resolve()
    data_root = Path(data_root).resolve()
    output_root = Path(output_root).resolve()
    serial_report = Path(serial_report).resolve()
    latency_root = data_root / "parallel-work" / "latency-report"
    if output_root.exists() and any(output_root.iterdir()):
        raise ValueError(f"Output directory must be new or empty: {output_root}")
    if not (latency_root / "summary.json").is_file():
        raise FileNotFoundError(f"Missing completed latency report: {latency_root}")
    if not (serial_report / "summary.json").is_file():
        raise FileNotFoundError(f"Missing serial baseline report: {serial_report}")
    output_root.mkdir(parents=True, exist_ok=True)

    encoder = bin_dir / "jxs_encoder"
    decoder = bin_dir / "jxs_decoder"
    if not encoder.is_file() or not decoder.is_file():
        raise FileNotFoundError(f"Missing OpenMP binaries under {bin_dir}")

    manifest = json.loads((data_root / "input-manifest.json").read_text(encoding="utf-8"))
    latency_summary = json.loads((latency_root / "summary.json").read_text(encoding="utf-8"))
    serial_summary = json.loads((serial_report / "summary.json").read_text(encoding="utf-8"))
    latency_summary["groups"] = add_latency_speedups(
        latency_summary["groups"], serial_summary["groups"]
    )
    best_key = (
        latency_summary["best"]["case"],
        latency_summary["best"]["configuration"],
        latency_summary["best"]["threads"],
    )
    latency_summary["best"] = next(
        group
        for group in latency_summary["groups"]
        if (group["case"], group["configuration"], group["threads"]) == best_key
    )
    resolved_configs = resolved_by_configuration(latency_summary["groups"])
    sources = [data_root / case["input"] for case in manifest["cases"]]
    sequence = data_root / "parallel-sequence"
    prepare_sequence(sources, sequence, frames)
    source_pattern = sequence / "frame_%04d.yuv8p"
    work_root = data_root / "parallel-work"
    env_base = dict(os.environ)
    env_base.update(OMP_DYNAMIC="FALSE", OMP_MAX_ACTIVE_LEVELS="1", OMP_WAIT_POLICY="PASSIVE")

    selected_configurations = list(configurations or CONFIGURATIONS)
    unknown = set(selected_configurations) - set(CONFIGURATIONS)
    if unknown:
        raise ValueError(f"unknown configurations: {sorted(unknown)}")

    raw_rows = []
    throughput_groups = []
    for configuration in selected_configurations:
        config = CONFIGURATIONS[configuration]
        encoded_dir = work_root / "encoded" / configuration
        decoded_dir = work_root / "decoded" / configuration
        encoded_pattern = encoded_dir / "stream_%04d.jxs"
        decoded_pattern = decoded_dir / "decoded_%04d.yuv8p"

        for worker_count in dict.fromkeys(workers):
            command = encoder_sequence_command(
                encoder, source_pattern, encoded_pattern, config, worker_count, frames
            )
            env = dict(env_base, OMP_NUM_THREADS="1")
            samples = _run_group(
                command,
                env,
                warmups,
                repeats,
                timeout,
                lambda directory=encoded_dir: _clean_generated(directory, ".jxs"),
                lambda directory=encoded_dir: _verify_codestreams(directory, frames),
            )
            outputs = _verify_codestreams(encoded_dir, frames)
            output_bytes = sum(path.stat().st_size for path in outputs)
            group = summarize_fps(samples, frames)
            group.update(
                phase="encode",
                configuration=configuration,
                requested_config=config,
                resolved_config=resolved_configs[configuration],
                parallelism=worker_count,
                output_count=len(outputs),
                output_bytes=output_bytes,
            )
            throughput_groups.append(group)
            for index, sample in enumerate(samples, start=1):
                raw_rows.append(
                    {
                        "phase": "encode",
                        "configuration": configuration,
                        "requested_config": config,
                        "resolved_config": resolved_configs[configuration],
                        "parallelism": worker_count,
                        "sample": index,
                        "frames": frames,
                        "seconds": sample["seconds"],
                        "fps": frames / sample["seconds"],
                        "sampled_peak_rss_bytes": sample["sampled_peak_rss_bytes"],
                        "sampled_peak_threads": sample["sampled_peak_threads"],
                        "output_count": len(outputs),
                        "output_bytes": output_bytes,
                    }
                )
            print(
                f"encode {configuration} -j {worker_count}: {group['p50_fps']:.3f} fps",
                flush=True,
            )

        for thread_count in dict.fromkeys(threads):
            command = decoder_sequence_command(decoder, encoded_pattern, decoded_pattern, frames)
            env = dict(env_base, OMP_NUM_THREADS=str(thread_count))
            samples = _run_group(
                command,
                env,
                warmups,
                repeats,
                timeout,
                lambda directory=decoded_dir: _clean_generated(directory, ".yuv8p"),
                lambda directory=decoded_dir: _verify_decoded(directory, frames),
            )
            outputs = _verify_decoded(decoded_dir, frames)
            output_bytes = sum(path.stat().st_size for path in outputs)
            group = summarize_fps(samples, frames)
            group.update(
                phase="decode",
                configuration=configuration,
                requested_config=config,
                resolved_config=resolved_configs[configuration],
                parallelism=thread_count,
                output_count=len(outputs),
                output_bytes=output_bytes,
            )
            throughput_groups.append(group)
            for index, sample in enumerate(samples, start=1):
                raw_rows.append(
                    {
                        "phase": "decode",
                        "configuration": configuration,
                        "requested_config": config,
                        "resolved_config": resolved_configs[configuration],
                        "parallelism": thread_count,
                        "sample": index,
                        "frames": frames,
                        "seconds": sample["seconds"],
                        "fps": frames / sample["seconds"],
                        "sampled_peak_rss_bytes": sample["sampled_peak_rss_bytes"],
                        "sampled_peak_threads": sample["sampled_peak_threads"],
                        "output_count": len(outputs),
                        "output_bytes": output_bytes,
                    }
                )
            print(
                f"decode {configuration} OMP {thread_count}: {group['p50_fps']:.3f} fps",
                flush=True,
            )

    best_latency = latency_summary["best"]
    best_encoder = max(
        (group for group in throughput_groups if group["phase"] == "encode"),
        key=lambda group: group["p50_fps"],
    )
    best_decoder = max(
        (group for group in throughput_groups if group["phase"] == "decode"),
        key=lambda group: group["p50_fps"],
    )
    best_pipeline, compatible_pipelines = best_compatible_pipeline(throughput_groups)
    pipeline_fps = best_pipeline["optimistic_fps"]
    peak_rss_bytes = max(group["peak_rss_bytes"] for group in throughput_groups)
    ram_bytes = psutil.virtual_memory().total
    decision = gate_decision(
        best_latency["combined_p50_seconds"],
        pipeline_fps,
        _parallel_improved(latency_summary["groups"]),
        peak_rss_bytes / (1024 * 1024),
        ram_bytes / (1024 * 1024),
    )

    system = host_metadata(
        bin_dir.parent, "enabled with Homebrew LLVM 22.1.4 and libomp 22.1.4"
    )
    system.update(
        {
            "encoder_sha256": digest_file(encoder),
            "decoder_sha256": digest_file(decoder),
            "libomp_linkage": subprocess.check_output(
                ["otool", "-L", str(encoder)], text=True
            ).strip(),
            "frames": frames,
            "workers": list(dict.fromkeys(workers)),
            "threads": list(dict.fromkeys(threads)),
            "warmups": warmups,
            "repeats": repeats,
            "configurations": selected_configurations,
            "serial_baseline_report": str(serial_report),
            "serial_baseline_summary_sha256": digest_file(serial_report / "summary.json"),
            "throughput_scope": (
                "batch CLI wall time; decoder includes YUV file output; optimistic pipeline is not simultaneous"
            ),
        }
    )
    summary = {
        "host_scope": "Apple M1 macOS OpenMP screening",
        "rk3588_tested": False,
        "physical_lan_tested": False,
        "serial_baseline": {
            "report": str(serial_report),
            "best": serial_summary["best"],
        },
        "single_frame_latency": latency_summary,
        "throughput_method": {
            "frames": frames,
            "warmups": warmups,
            "repeats": repeats,
            "caveat": (
                "A single 60-frame run per group is a screening measurement, not a stable tail estimate"
                if repeats == 1
                else "Nearest-rank summaries over repeated 60-frame batch runs"
            ),
        },
        "throughput_groups": throughput_groups,
        "best_latency": best_latency,
        "best_encoder_throughput": best_encoder,
        "best_decoder_throughput": best_decoder,
        "best_compatible_pipeline": best_pipeline,
        "compatible_pipelines": compatible_pipelines,
        "optimistic_two_stage_fps": pipeline_fps,
        "peak_rss_bytes": peak_rss_bytes,
        "decision": decision,
    }

    shutil.copy2(data_root / "input-manifest.json", output_root / "input-manifest.json")
    shutil.copy2(latency_root / "raw-timings.csv", output_root / "raw-latency.csv")
    _write_csv(output_root / "raw-throughput.csv", raw_rows)
    (output_root / "system.json").write_text(json.dumps(system, indent=2) + "\n", encoding="utf-8")
    (output_root / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    (output_root / "summary.md").write_text(_markdown(summary), encoding="utf-8")
    return summary


def merge_partial_reports(partial_roots, output_root, serial_report):
    partial_roots = [Path(path).resolve() for path in partial_roots]
    output_root = Path(output_root).resolve()
    serial_report = Path(serial_report).resolve()
    if len(partial_roots) < 2:
        raise ValueError("at least two partial reports are required")
    if output_root.exists() and any(output_root.iterdir()):
        raise ValueError(f"Output directory must be new or empty: {output_root}")
    output_root.mkdir(parents=True, exist_ok=True)

    partial_summaries = [
        json.loads((root / "summary.json").read_text(encoding="utf-8"))
        for root in partial_roots
    ]
    partial_systems = [
        json.loads((root / "system.json").read_text(encoding="utf-8"))
        for root in partial_roots
    ]
    validate_run_controls(partial_systems)
    binary_pairs = {
        (system["encoder_sha256"], system["decoder_sha256"])
        for system in partial_systems
    }
    if len(binary_pairs) != 1:
        raise ValueError("partial reports used different codec binaries")
    if len({digest_file(root / "input-manifest.json") for root in partial_roots}) != 1:
        raise ValueError("partial reports used different input manifests")
    if len({digest_file(root / "raw-latency.csv") for root in partial_roots}) != 1:
        raise ValueError("partial reports used different latency datasets")

    latency_summary = partial_summaries[0]["single_frame_latency"]
    if any(
        summary["single_frame_latency"] != latency_summary
        for summary in partial_summaries[1:]
    ):
        raise ValueError("partial reports disagree on full latency results")
    if not (serial_report / "summary.json").is_file():
        raise FileNotFoundError(f"Missing serial baseline report: {serial_report}")
    serial_summary = json.loads((serial_report / "summary.json").read_text(encoding="utf-8"))
    latency_summary = dict(latency_summary)
    latency_summary["groups"] = add_latency_speedups(
        latency_summary["groups"], serial_summary["groups"]
    )
    best_key = (
        latency_summary["best"]["case"],
        latency_summary["best"]["configuration"],
        latency_summary["best"]["threads"],
    )
    latency_summary["best"] = next(
        group
        for group in latency_summary["groups"]
        if (group["case"], group["configuration"], group["threads"]) == best_key
    )

    throughput_groups = [
        dict(group)
        for summary in partial_summaries
        for group in summary["throughput_groups"]
    ]
    resolved_configs = resolved_by_configuration(latency_summary["groups"])
    for group in throughput_groups:
        configuration = group["configuration"]
        group.setdefault("requested_config", CONFIGURATIONS[configuration])
        group.setdefault("resolved_config", resolved_configs[configuration])
    keys = [
        (group["phase"], group["configuration"], group["parallelism"])
        for group in throughput_groups
    ]
    if len(keys) != len(set(keys)):
        raise ValueError("partial reports contain duplicate throughput groups")

    raw_rows = []
    for root in partial_roots:
        with (root / "raw-throughput.csv").open(newline="", encoding="utf-8") as stream:
            raw_rows.extend(csv.DictReader(stream))
    for row in raw_rows:
        configuration = row["configuration"]
        row.setdefault("requested_config", CONFIGURATIONS[configuration])
        row.setdefault("resolved_config", resolved_configs[configuration])

    best_encoder = max(
        (group for group in throughput_groups if group["phase"] == "encode"),
        key=lambda group: group["p50_fps"],
    )
    best_decoder = max(
        (group for group in throughput_groups if group["phase"] == "decode"),
        key=lambda group: group["p50_fps"],
    )
    best_pipeline, compatible_pipelines = best_compatible_pipeline(throughput_groups)
    pipeline_fps = best_pipeline["optimistic_fps"]
    peak_rss_bytes = max(group["peak_rss_bytes"] for group in throughput_groups)
    ram_bytes = partial_systems[0]["ram_bytes"]
    decision = gate_decision(
        latency_summary["best"]["combined_p50_seconds"],
        pipeline_fps,
        _parallel_improved(latency_summary["groups"]),
        peak_rss_bytes / (1024 * 1024),
        ram_bytes / (1024 * 1024),
    )

    system = dict(partial_systems[0])
    system["configurations"] = sorted(
        {
            configuration
            for partial in partial_systems
            for configuration in partial.get("configurations", [])
        }
    )
    system["partial_reports"] = [str(path) for path in partial_roots]
    system["serial_baseline_report"] = str(serial_report)
    system["serial_baseline_summary_sha256"] = digest_file(serial_report / "summary.json")
    summary = {
        "host_scope": "Apple M1 macOS OpenMP screening",
        "rk3588_tested": False,
        "physical_lan_tested": False,
        "serial_baseline": {
            "report": str(serial_report),
            "best": serial_summary["best"],
        },
        "single_frame_latency": latency_summary,
        "throughput_method": {
            "frames": partial_systems[0]["frames"],
            "warmups": partial_systems[0]["warmups"],
            "repeats": partial_systems[0]["repeats"],
            "caveat": (
                "A single 60-frame run per group is a screening measurement, not a stable tail estimate"
                if partial_systems[0]["repeats"] == 1
                else "Nearest-rank summaries over repeated 60-frame batch runs"
            ),
        },
        "throughput_groups": throughput_groups,
        "best_latency": latency_summary["best"],
        "best_encoder_throughput": best_encoder,
        "best_decoder_throughput": best_decoder,
        "best_compatible_pipeline": best_pipeline,
        "compatible_pipelines": compatible_pipelines,
        "optimistic_two_stage_fps": pipeline_fps,
        "peak_rss_bytes": peak_rss_bytes,
        "decision": decision,
    }

    shutil.copy2(partial_roots[0] / "input-manifest.json", output_root / "input-manifest.json")
    shutil.copy2(partial_roots[0] / "raw-latency.csv", output_root / "raw-latency.csv")
    _write_csv(output_root / "raw-throughput.csv", raw_rows)
    (output_root / "system.json").write_text(json.dumps(system, indent=2) + "\n", encoding="utf-8")
    (output_root / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    (output_root / "summary.md").write_text(_markdown(summary), encoding="utf-8")
    return summary


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin-dir", type=Path, required=True)
    parser.add_argument("--serial-report", type=Path, default=Path("reports/host-current"))
    parser.add_argument("--data", type=Path, default=Path("test-data/host-4k444"))
    parser.add_argument("--output", type=Path, default=Path("reports/host-parallel"))
    parser.add_argument("--frames", type=int, default=60)
    parser.add_argument("--workers", type=int, nargs="+", default=[1, 2, 4, 8])
    parser.add_argument("--threads", type=int, nargs="+", default=[1, 2, 4, 8])
    parser.add_argument("--warmups", type=int, default=1)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--timeout", type=float, default=600)
    parser.add_argument(
        "--configurations",
        nargs="+",
        choices=sorted(CONFIGURATIONS),
        default=list(CONFIGURATIONS),
    )
    args = parser.parse_args()
    if args.frames < 1 or args.repeats < 1 or args.warmups < 0:
        parser.error("frames/repeats must be positive and warmups nonnegative")
    if any(value < 1 for value in args.workers + args.threads):
        parser.error("workers and threads must be positive")
    if args.timeout <= 0:
        parser.error("timeout must be positive")
    if not (args.serial_report / "summary.json").is_file():
        parser.error("--serial-report must contain summary.json")
    return args


def main():
    args = parse_args()
    summary = benchmark_throughput(
        args.bin_dir,
        args.data,
        args.output,
        args.serial_report,
        frames=args.frames,
        workers=args.workers,
        threads=args.threads,
        warmups=args.warmups,
        repeats=args.repeats,
        timeout=args.timeout,
        configurations=args.configurations,
    )
    print(json.dumps(summary["decision"], indent=2))


if __name__ == "__main__":
    main()
