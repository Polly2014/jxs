"""Offline codec regression tests; only the Python standard library is required."""

import argparse
import os
import struct
import subprocess
import tempfile
from pathlib import Path


def command(args, threads, success=True):
    env = dict(os.environ, OMP_NUM_THREADS=str(threads), OMP_DYNAMIC="FALSE",
               OMP_WAIT_POLICY="PASSIVE", OMP_MAX_ACTIVE_LEVELS="1")
    result = subprocess.run([str(x) for x in args], env=env, capture_output=True, timeout=45)
    if (result.returncode == 0) != success:
        raise AssertionError(f"Unexpected status {result.returncode}: {args}\n"
                             + result.stderr.decode(errors="replace"))


def payload(path):
    with path.open("rb") as stream:
        magic = stream.readline().strip()
        dimensions = stream.readline().strip()
        maxval = stream.readline().strip()
        return magic, dimensions, maxval, stream.read()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin", type=Path, required=True)
    parser.add_argument("--openmp", choices=["ON", "OFF"], default="ON")
    parser.add_argument("--reference", type=Path, help="Optional original bin directory for golden bytes")
    parser.add_argument("--probe-16bit", action="store_true",
                        help="Also reproduce the upstream full-range 16-bit failure (not a supported gate)")
    args = parser.parse_args()
    suffix = ".exe" if os.name == "nt" else ""
    enc, dec = [args.bin.resolve() / (name + suffix) for name in ("jxs_encoder", "jxs_decoder")]
    ref_enc = args.reference.resolve() / ("jxs_encoder" + suffix) if args.reference else enc
    ref_dec = args.reference.resolve() / ("jxs_decoder" + suffix) if args.reference else dec
    with tempfile.TemporaryDirectory(prefix="jxs-regression-") as temp:
        root = Path(temp)
        formats = [(1, 8, 100, 61), (3, 8, 100, 61), (1, 10, 100, 61),
                   (1, 12, 100, 61), (3, 8, 520, 513)]
        if args.probe_16bit:
            formats.append((1, 16, 100, 61))
        for components, depth, width, height in formats:
            values = [((i * 61) ^ (i >> 2)) % (1 << depth)
                      for i in range(width * height * components)]
            body = bytes(values) if depth == 8 else struct.pack(">" + "H" * len(values), *values)
            magic = "P5" if components == 1 else "P6"
            extension = ".pgm" if components == 1 else ".ppm"
            source = root / ("source" + extension)
            source.write_bytes(f"{magic}\n{width} {height}\n{(1 << depth)-1}\n".encode() + body)
            for nly, cw in [(0, 1), (1, 1), (2, 0)]:
                bands = components * (3 + 2 * nly)
                config = (f"p=unrestricted;size=-1;bw={depth};fq=0;nlx=2;nly={nly};cw={cw};"
                          f"cpih={'rct' if components == 3 else 'none'};"
                          "gains=" + ",".join(["0"] * bands) + ";priorities="
                          + ",".join(str(i) for i in range(bands)))
                baseline = root / "baseline.jxs"
                command([ref_enc, "-c", config, source, baseline], 1)
                golden = root / ("golden" + extension)
                command([ref_dec, baseline, golden], 1)
                if depth <= 12 and payload(source) != payload(golden):
                    raise AssertionError("Reference is not lossless for the supported 8/10/12-bit case")
                for threads in [1, 2, 4]:
                    stream, image = root / "parallel.jxs", root / ("decoded" + extension)
                    command([enc, "-c", config, source, stream], threads)
                    if stream.read_bytes() != baseline.read_bytes():
                        raise AssertionError(f"Stream mismatch: {components}/{depth}/{nly}/{cw}/{threads}")
                    command([dec, stream, image], threads)
                    if payload(golden) != payload(image):
                        raise AssertionError(f"Decode mismatch: {components}/{depth}/{nly}/{cw}/{threads}")
        for sy, depth in [(1, 10), (2, 12)]:
            extension = ".yuv16_422p" if sy == 1 else ".yuv16_420p"
            samples = 104 * 62 + 2 * 52 * (62 // sy)
            values = [((i * 37) ^ (i >> 3)) % (1 << depth) for i in range(samples)]
            source = root / ("source" + extension)
            source.write_bytes(struct.pack("<" + "H" * samples, *values))
            bands = 15 if sy == 1 else 11
            config = (f"p=unrestricted;size=-1;bw={depth};fq=0;nlx=2;nly=1;cw=1;cpih=none;"
                      "gains=" + ",".join(["0"] * bands) + ";priorities="
                      + ",".join(map(str, range(bands))))
            baseline = root / "raw-reference.jxs"
            command([ref_enc, "-c", config, "-w", "104", "-h", "62", "-d", str(depth),
                     source, baseline], 1)
            for threads in [1, 2, 4]:
                stream, image = root / "raw.jxs", root / ("raw-decoded" + extension)
                command([enc, "-c", config, "-w", "104", "-h", "62", "-d", str(depth),
                         source, stream], threads)
                if stream.read_bytes() != baseline.read_bytes():
                    raise AssertionError("Subsampled stream mismatch")
                command([dec, stream, image], threads)
                if source.read_bytes() != image.read_bytes():
                    raise AssertionError("Subsampled lossless reconstruction mismatch")
        # Four RGB frames exercise reentrant contexts, output indexing and tails.
        config = ("p=unrestricted;size=-1;bw=8;fq=0;nlx=2;nly=1;cw=1;cpih=rct;"
                  "gains=" + ",".join(["0"] * 15) + ";priorities=" + ",".join(map(str, range(15))))
        for index in range(7, 11):
            body = (bytes(100 * 61 * 3) if index == 7 else
                    bytes((i * 7 + index) % 256 for i in range(100 * 61 * 3)))
            (root / f"in_{index:04d}.ppm").write_bytes(b"P6\n100 61\n255\n" + body)
        inp = root / "in_%04d.ppm"
        serial = root / "serial_%04d.jxs"
        command([enc, "-c", config, "-f", "7", "-n", "4", inp, serial], 1)
        command([dec, "-f", "7", "-n", "4", serial, root / "decoded_%04d.ppm"], 4)
        for index in range(7, 11):
            if payload(root / f"in_{index:04d}.ppm") != payload(root / f"decoded_{index:04d}.ppm"):
                raise AssertionError("Variable-size sequence decode mismatch")
        command([dec, "-f", "7", "-n", "1", serial, root / "limited_%04d.ppm"], 4)
        if (root / "limited_0008.ppm").exists():
            raise AssertionError("Decoder ignored -n")
        command([dec, "-f", "7", "-n", "5", serial, root / "missing_%04d.ppm"], 4, success=False)
        for jobs in [2, 4, 8]:
            command([enc, "-c", config, "-f", "7", "-n", "4", "-j", str(jobs),
                     inp, root / "jobs_%04d.jxs"], 8, success=args.openmp == "ON")
            if args.openmp == "ON":
                for index in range(7, 11):
                    if (root / f"serial_{index:04d}.jxs").read_bytes() != (root / f"jobs_{index:04d}.jxs").read_bytes():
                        raise AssertionError("Frame-parallel output mismatch")
        bad_options = [
            ["-j", "0"], ["-j", "-2"], ["-j", "2.5"], ["-j", "257"],
            ["-j", "2"], ["-j", "2", "-n", "2", "-D"],
            ["-j", "2", "-f", "2147483647", "-n", "2"],
            ["-j", "2", "-n", "4294967297"], ["-j", "2", "-f", "nan", "-n", "2"],
            ["-j", "2", "-n", "2", "-f", "99"],
        ]
        for options in bad_options:
            command([enc, "-c", config, *options, inp, root / "bad_%04d.jxs"], 4, success=False)
        for pattern in ["same.jxs", "%s.jxs", "%d_%d.jxs", "%999999d.jxs", "tail%.jxs"]:
            command([enc, "-c", config, "-j", "2", "-n", "2", "-f", "7",
                     inp, root / pattern], 4, success=False)
        # Valid image and weights, but rate allocation must fail inside workers.
        command([enc, "-c", config.replace("size=-1", "rate=0.2"),
                 root / "in_0007.ppm", root / "too-small.jxs"], 4, success=False)
    print("Offline regression passed: 51 round trips (including YUV 420/422), frame workers, invalid arguments and worker failure")
    print("Source-exact gate: 8/10/12-bit. Full-range 16-bit is unsupported; --probe-16bit reproduces upstream failures.")


if __name__ == "__main__":
    main()
