import importlib.util
import tempfile
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("evaluate.py")
SPEC = importlib.util.spec_from_file_location("udp_evaluate", MODULE_PATH)
udp_evaluate = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(udp_evaluate)


HEADER = (
    "frame,encode_ms,send_ms,network_ms,decode_assemble_ms,latency_ms,"
    "schedule_late_ms,complete_ms,datagrams_sent,datagrams_received,"
    "app_bytes_sent,app_bytes_received,checksum,checksum_sampled\n"
)


def row(frame, latency, late, complete, *, sent=10, received=10,
        bytes_sent=1000, bytes_received=1000, checksum="0123456789abcdef"):
    return (
        f"{frame},10.0,1.0,3.0,6.0,{latency},{late},{complete},"
        f"{sent},{received},{bytes_sent},{bytes_received},{checksum},1\n"
    )


class EvaluateTests(unittest.TestCase):
    def evaluate(self, body, expected_frames=3):
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "result.csv"
            path.write_text(HEADER + body, encoding="utf-8")
            return udp_evaluate.evaluate_csv(path, expected_frames=expected_frames)

    def test_accepts_hand_checked_three_frame_fixture(self):
        result = self.evaluate(
            row(0, 20.0, 0.1, 1000.0)
            + row(1, 30.0, 0.2, 1016.0)
            + row(2, 40.0, 0.1, 1032.0)
        )
        self.assertEqual(result["row_count"], 3)
        self.assertEqual(result["latency_p99_ms"], 40.0)
        self.assertEqual(result["latency_max_ms"], 40.0)
        self.assertEqual(result["output_fps"], 62.5)
        self.assertEqual(result["datagrams_sent"], 30)
        self.assertEqual(result["datagrams_received"], 30)
        self.assertTrue(result["accepted"])

    def test_rejects_missing_rows(self):
        with self.assertRaisesRegex(ValueError, "expected 3 rows"):
            self.evaluate(row(0, 20.0, 0.0, 1000.0), expected_frames=3)

    def test_rejects_non_monotonic_frame_ids(self):
        with self.assertRaisesRegex(ValueError, "frame IDs"):
            self.evaluate(
                row(0, 20.0, 0.0, 1000.0)
                + row(2, 20.0, 0.0, 1016.0)
                + row(1, 20.0, 0.0, 1032.0)
            )

    def test_rejects_packet_count_mismatch(self):
        with self.assertRaisesRegex(ValueError, "packet/byte mismatch"):
            self.evaluate(
                row(0, 20.0, 0.0, 1000.0, received=9)
                + row(1, 20.0, 0.0, 1016.0)
                + row(2, 20.0, 0.0, 1032.0)
            )

    def test_latency_equal_to_50_ms_fails_acceptance(self):
        result = self.evaluate(
            row(0, 20.0, 0.0, 1000.0)
            + row(1, 30.0, 0.0, 1016.0)
            + row(2, 50.0, 0.0, 1032.0)
        )
        self.assertEqual(result["latency_p99_ms"], 50.0)
        self.assertFalse(result["accepted"])
        self.assertIn("latency_p99_not_below_50ms", result["failures"])

    def test_accumulated_lateness_fails_acceptance(self):
        result = self.evaluate(
            row(0, 20.0, 0.1, 1000.0)
            + row(1, 20.0, 1.0, 1016.0)
            + row(2, 20.0, 3.0, 1032.0)
        )
        self.assertFalse(result["accepted"])
        self.assertIn("schedule_lateness_accumulated", result["failures"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
