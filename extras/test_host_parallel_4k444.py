import tempfile
import unittest
from pathlib import Path

from host_parallel_4k444 import (
    add_latency_speedups,
    best_compatible_pipeline,
    decoder_sequence_command,
    encoder_sequence_command,
    gate_decision,
    optimistic_pipeline_fps,
    prepare_sequence,
    summarize_fps,
    validate_run_controls,
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
            self.assertEqual(
                [path.resolve().name for path in links],
                [
                    "gradient.yuv8p",
                    "natural.yuv8p",
                    "high-frequency.yuv8p",
                    "gradient.yuv8p",
                    "natural.yuv8p",
                    "high-frequency.yuv8p",
                ],
            )

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
            Path("bin/jxs_encoder"),
            Path("frame_%04d.yuv8p"),
            Path("stream_%04d.jxs"),
            "rate=4",
            workers=4,
            frames=60,
        )
        self.assertIn("-j", command)
        self.assertEqual(command[command.index("-j") + 1], "4")
        self.assertEqual(command[command.index("-n") + 1], "60")

    def test_decoder_sequence_command_requests_all_frames(self):
        command = decoder_sequence_command(
            Path("bin/jxs_decoder"),
            Path("stream_%04d.jxs"),
            Path("decoded_%04d.yuv8p"),
            frames=60,
        )
        self.assertEqual(command[command.index("-n") + 1], "60")

    def test_summarize_fps_uses_nearest_rank_wall_time(self):
        summary = summarize_fps(
            [
                {"seconds": 10, "sampled_peak_rss_bytes": 100, "sampled_peak_threads": 1},
                {"seconds": 12, "sampled_peak_rss_bytes": 120, "sampled_peak_threads": 2},
                {"seconds": 11, "sampled_peak_rss_bytes": 110, "sampled_peak_threads": 1},
            ],
            frames=60,
        )
        self.assertEqual(summary["p50_seconds"], 11)
        self.assertAlmostEqual(summary["p50_fps"], 60 / 11)
        self.assertEqual(summary["peak_rss_bytes"], 120)
        self.assertEqual(summary["peak_threads"], 2)

    def test_optimistic_pipeline_is_slower_stage(self):
        self.assertEqual(optimistic_pipeline_fps(80, 55), 55)

    def test_best_compatible_pipeline_pairs_matching_configurations(self):
        groups = [
            {"phase": "encode", "configuration": "full-width", "p50_fps": 100},
            {"phase": "decode", "configuration": "full-width", "p50_fps": 30},
            {"phase": "encode", "configuration": "columns", "p50_fps": 50},
            {"phase": "decode", "configuration": "columns", "p50_fps": 40},
        ]
        best, pairs = best_compatible_pipeline(groups)
        self.assertEqual(best["configuration"], "columns")
        self.assertEqual(best["optimistic_fps"], 40)
        self.assertEqual({pair["configuration"] for pair in pairs}, {"full-width", "columns"})

    def test_validate_run_controls_rejects_mismatched_frames(self):
        first = {"frames": 60, "warmups": 0, "repeats": 1, "workers": [1, 2], "threads": [1, 2]}
        second = {**first, "frames": 30}
        with self.assertRaisesRegex(ValueError, "frames"):
            validate_run_controls([first, second])

    def test_add_latency_speedups_compares_openmp_and_serial_baselines(self):
        latency_groups = [
            {"case": "natural", "configuration": "columns", "threads": 1, "combined_p50_seconds": 1.8},
            {"case": "natural", "configuration": "columns", "threads": 4, "combined_p50_seconds": 1.2},
        ]
        serial_groups = [
            {"case": "natural", "configuration": "columns", "threads": 1, "combined_p50_seconds": 2.0},
        ]
        enriched = add_latency_speedups(latency_groups, serial_groups)
        self.assertAlmostEqual(enriched[1]["speedup_vs_openmp_1"], 1.5)
        self.assertAlmostEqual(enriched[1]["speedup_vs_serial"], 2.0 / 1.2)
        self.assertAlmostEqual(enriched[0]["openmp_1_vs_serial"], 2.0 / 1.8)

    def test_gate_requires_both_latency_and_throughput(self):
        self.assertEqual(
            gate_decision(0.090, 65, True, 1_000, 16_000)["status"], "proceed-rk3588"
        )
        self.assertEqual(gate_decision(0.101, 65, True, 1_000, 16_000)["status"], "stop")
        self.assertEqual(gate_decision(0.090, 59.9, True, 1_000, 16_000)["status"], "stop")
        self.assertEqual(gate_decision(0.090, 65, False, 1_000, 16_000)["status"], "stop")
        self.assertEqual(gate_decision(0.090, 65, True, 12_001, 16_000)["status"], "stop")


if __name__ == "__main__":
    unittest.main()
