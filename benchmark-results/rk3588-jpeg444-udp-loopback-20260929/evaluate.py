#!/usr/bin/env python3
"""Independent evaluator for the RK3588 simulated-LAN UDP probe."""

from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path
from typing import Any


REQUIRED_COLUMNS = {
    "frame",
    "encode_ms",
    "send_ms",
    "network_ms",
    "decode_assemble_ms",
    "latency_ms",
    "schedule_late_ms",
    "complete_ms",
    "datagrams_sent",
    "datagrams_received",
    "app_bytes_sent",
    "app_bytes_received",
    "checksum",
    "checksum_sampled",
}


def percentile(values: list[float], probability: float) -> float:
    ordered = sorted(values)
    index = max(0, min(len(ordered) - 1,
                       math.ceil(probability * len(ordered)) - 1))
    return ordered[index]


def evaluate_csv(path: Path | str, *, expected_frames: int) -> dict[str, Any]:
    csv_path = Path(path)
    with csv_path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        if reader.fieldnames is None or not REQUIRED_COLUMNS.issubset(reader.fieldnames):
            missing = sorted(REQUIRED_COLUMNS - set(reader.fieldnames or ()))
            raise ValueError(f"missing CSV columns: {missing}")
        rows = list(reader)

    if len(rows) != expected_frames:
        raise ValueError(f"expected {expected_frames} rows, found {len(rows)}")
    frame_ids = [int(row["frame"]) for row in rows]
    if frame_ids != list(range(expected_frames)):
        raise ValueError("frame IDs are not contiguous 0..N-1")

    float_columns = (
        "encode_ms",
        "send_ms",
        "network_ms",
        "decode_assemble_ms",
        "latency_ms",
        "schedule_late_ms",
        "complete_ms",
    )
    values = {
        column: [float(row[column]) for row in rows]
        for column in float_columns
    }
    if any(not math.isfinite(value) or value < 0.0
           for column in float_columns for value in values[column]):
        raise ValueError("timing values must be finite and non-negative")
    if any(later <= earlier for earlier, later in
           zip(values["complete_ms"], values["complete_ms"][1:])):
        raise ValueError("completion timestamps must increase")

    integer_columns = (
        "datagrams_sent",
        "datagrams_received",
        "app_bytes_sent",
        "app_bytes_received",
    )
    integers = {
        column: [int(row[column]) for row in rows]
        for column in integer_columns
    }
    for index in range(expected_frames):
        if (integers["datagrams_sent"][index] !=
                integers["datagrams_received"][index] or
                integers["app_bytes_sent"][index] !=
                integers["app_bytes_received"][index]):
            raise ValueError(f"packet/byte mismatch at frame {index}")
        if integers["datagrams_sent"][index] <= 0:
            raise ValueError(f"zero datagrams at frame {index}")

    sampled_checksums = {
        row["checksum"]
        for row in rows
        if int(row["checksum_sampled"]) != 0
    }
    if len(sampled_checksums) > 1:
        raise ValueError("sampled checksum mismatch")
    if not sampled_checksums:
        raise ValueError("no sampled checksums")

    elapsed_ms = values["complete_ms"][-1] - values["complete_ms"][0]
    output_fps = (expected_frames - 1) * 1000.0 / elapsed_ms
    lateness_growth = (values["schedule_late_ms"][-1] -
                       values["schedule_late_ms"][0])
    failures: list[str] = []
    latency_p99 = percentile(values["latency_ms"], 0.99)
    if output_fps < 60.0:
        failures.append("output_fps_below_60")
    if latency_p99 >= 50.0:
        failures.append("latency_p99_not_below_50ms")
    if lateness_growth > 2.0:
        failures.append("schedule_lateness_accumulated")

    result: dict[str, Any] = {
        "source_csv": str(csv_path),
        "row_count": len(rows),
        "first_frame": frame_ids[0],
        "last_frame": frame_ids[-1],
        "output_fps": output_fps,
        "encode_p99_ms": percentile(values["encode_ms"], 0.99),
        "send_p99_ms": percentile(values["send_ms"], 0.99),
        "network_p99_ms": percentile(values["network_ms"], 0.99),
        "decode_assemble_p99_ms": percentile(
            values["decode_assemble_ms"], 0.99
        ),
        "latency_p50_ms": percentile(values["latency_ms"], 0.50),
        "latency_p95_ms": percentile(values["latency_ms"], 0.95),
        "latency_p99_ms": latency_p99,
        "latency_max_ms": max(values["latency_ms"]),
        "schedule_late_first_ms": values["schedule_late_ms"][0],
        "schedule_late_last_ms": values["schedule_late_ms"][-1],
        "schedule_late_p99_ms": percentile(values["schedule_late_ms"], 0.99),
        "schedule_late_max_ms": max(values["schedule_late_ms"]),
        "schedule_lateness_growth_ms": lateness_growth,
        "datagrams_sent": sum(integers["datagrams_sent"]),
        "datagrams_received": sum(integers["datagrams_received"]),
        "app_bytes_sent": sum(integers["app_bytes_sent"]),
        "app_bytes_received": sum(integers["app_bytes_received"]),
        "sampled_checksum_count": sum(
            int(row["checksum_sampled"]) != 0 for row in rows
        ),
        "sampled_checksum_unique": len(sampled_checksums),
        "failures": failures,
        "accepted": not failures,
    }
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("csv", type=Path)
    parser.add_argument("--expected-frames", type=int, required=True)
    parser.add_argument("--output", type=Path)
    arguments = parser.parse_args()
    result = evaluate_csv(arguments.csv,
                          expected_frames=arguments.expected_frames)
    rendered = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if arguments.output:
        arguments.output.write_text(rendered, encoding="utf-8")
    else:
        print(rendered, end="")
    return 0 if result["accepted"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
