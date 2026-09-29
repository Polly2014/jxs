import tempfile
import unittest
from pathlib import Path

import numpy as np

from host_4k444 import (
    FRAME_BYTES,
    decoder_command,
    digest_bytes,
    encoder_command,
    gate_decision,
    gradient_planes,
    high_frequency_planes,
    nearest_rank,
    parse_resolved_config,
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

    def test_gradient_planes_are_deterministic_and_full_resolution(self):
        first = gradient_planes(8, 4)
        second = gradient_planes(8, 4)
        self.assertEqual([plane.shape for plane in first], [(4, 8)] * 3)
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

    def test_encoder_command_declares_raw_geometry_and_depth(self):
        command = encoder_command(
            Path("bin/jxs_encoder"), Path("in.yuv8p"), Path("out.jxs"), "rate=4"
        )
        self.assertEqual(command[1:7], ["-w", "3840", "-h", "2160", "-d", "8"])
        self.assertEqual(command[-2:], [Path("in.yuv8p"), Path("out.jxs")])

    def test_decoder_command_requests_verbose_probe_and_planar_output(self):
        command = decoder_command(Path("bin/jxs_decoder"), Path("in.jxs"), Path("out.yuv8p"))
        self.assertEqual(command, [Path("bin/jxs_decoder"), "-v", Path("in.jxs"), Path("out.yuv8p")])

    def test_parse_resolved_config_extracts_encoder_dump(self):
        output = 'Image loaded\nConfiguration: "p=unrestricted;rate=4;cw=0;"\n'
        self.assertEqual(parse_resolved_config(output), "p=unrestricted;rate=4;cw=0;")

    def test_parse_resolved_config_rejects_missing_dump(self):
        with self.assertRaisesRegex(ValueError, "configuration dump"):
            parse_resolved_config("Image loaded only")


if __name__ == "__main__":
    unittest.main()
