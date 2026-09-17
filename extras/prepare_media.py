"""Download attributed evaluation media and prepare reproducible codec inputs."""

import argparse
import hashlib
import json
import shutil
import subprocess
import urllib.request
import zipfile
from pathlib import Path

from PIL import Image, ImageOps
import imageio_ffmpeg
import numpy as np


SOURCES = [
    {
        "name": "astronaut.png",
        "url": "https://raw.githubusercontent.com/scikit-image/scikit-image/v0.19.3/skimage/data/astronaut.png",
        "attribution": "NASA: astronaut Eileen Collins, public domain",
        "license_url": "https://scikit-image.org/docs/0.19.x/api/skimage.data.html#skimage.data.astronaut",
        "sha256": "88431cd9653ccd539741b555fb0a46b61558b301d4110412b5bc28b5e3ea6cb5",
    },
    {
        "name": "mars.jpg",
        "url": "https://images-assets.nasa.gov/image/PIA04921/PIA04921~orig.jpg",
        "attribution": "NASA/JPL/California Institute of Technology, PIA04921",
        "title": "Andromeda Galaxy (GALEX); legacy local case prefix: mars",
        "metadata_url": "https://images-api.nasa.gov/search?nasa_id=PIA04921",
        "license_url": "https://www.nasa.gov/nasa-brand-center/images-and-media/",
        "sha256": "4c2bc14dccd339b73c55cba14d3c18a63c4609041c375be18c442ee13153a274",
    },
    {
        "name": "bbb.zip",
        "url": "https://download.blender.org/peach/bigbuckbunny_movies/BigBuckBunny_320x180.mp4.zip",
        "attribution": "(c) copyright 2008, Blender Foundation / www.bigbuckbunny.org",
        "license_url": "https://peach.blender.org/about/",
        "license": "CC BY 3.0",
        "sha256": "109e3ede8790bd633f374ca311d9cc61dce8d7f98f5b0797ca98199c9fbceedf",
    },
]


def digest(path):
    with path.open("rb") as stream:
        h = hashlib.sha256()
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path("test-data"))
    parser.add_argument("--frames", type=int, default=12)
    args = parser.parse_args()
    if not 1 <= args.frames <= 240:
        parser.error("--frames must be in [1, 240]")
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=True)
    sources = []
    for source in SOURCES:
        dest = root / source["name"]
        if not dest.exists():
            print("Downloading", source["url"], flush=True)
            partial = dest.with_suffix(dest.suffix + ".part")
            curl = shutil.which("curl")
            if curl:
                subprocess.run([
                    curl, "--fail", "--location", "--silent", "--show-error",
                    "--max-time", "600", "--output", str(partial), source["url"],
                ], check=True, timeout=620)
            else:
                request = urllib.request.Request(source["url"], headers={"User-Agent": "jxs-evaluation/1.0"})
                with urllib.request.urlopen(request, timeout=120) as response, partial.open("wb") as out:
                    shutil.copyfileobj(response, out)
            partial.replace(dest)
        if digest(dest) != source["sha256"]:
            raise RuntimeError(f"Source SHA-256 mismatch: {dest}")
        sources.append({**source, "sha256": digest(dest), "bytes": dest.stat().st_size})

    cases = []

    def record(name, path, source, operation, depth=8):
        with Image.open(path) as image:
            size = image.size
        cases.append({
            "name": name, "input": path.name, "width": size[0], "height": size[1],
            "source": source, "operation": operation, "sha256": digest(path),
            "depth": depth,
        })

    with Image.open(root / "astronaut.png") as image:
        image.convert("RGB").save(root / "astronaut.ppm")
        image.convert("L").crop((0, 0, 509, 507)).save(root / "astronaut_odd.pgm")
    record("astronaut", root / "astronaut.ppm", "astronaut.png", "RGB conversion, native 512x512")
    record("astronaut_odd", root / "astronaut_odd.pgm", "astronaut.png", "grayscale and odd-size crop")
    with Image.open(root / "astronaut_odd.pgm") as image:
        expanded = np.asarray(image).astype(np.uint16) * 16
    (root / "astronaut_12.pgm").write_bytes(
        b"P5\n509 507\n4095\n" + expanded.astype(">u2").tobytes())
    record("astronaut_12", root / "astronaut_12.pgm", "astronaut.png",
           "8-bit grayscale expanded to 12-bit container; not native HDR data", 12)

    with Image.open(root / "mars.jpg") as image:
        image = image.convert("RGB")
        original = image.size
        for width, height in [(1920, 1080), (3840, 2160)]:
            dest = root / f"mars_{width}.ppm"
            ImageOps.fit(image, (width, height), method=Image.Resampling.LANCZOS).save(dest)
            record(f"mars_{width}", dest, "mars.jpg",
                   f"center-crop/resample from {original}; derived load case, not native sensor dimensions")

    video = root / "bbb.mp4"
    if not video.exists():
        with zipfile.ZipFile(root / "bbb.zip") as archive:
            members = [item for item in archive.infolist() if item.filename.lower().endswith(".mp4")]
            if len(members) != 1:
                raise RuntimeError("Expected exactly one MP4 in the Blender archive")
            with archive.open(members[0]) as src, video.open("wb") as out:
                shutil.copyfileobj(src, out)
    ffmpeg = shutil.which("ffmpeg") or imageio_ffmpeg.get_ffmpeg_exe()
    subprocess.run([
        ffmpeg, "-hide_banner", "-loglevel", "error", "-threads", "1",
        "-ss", "60", "-i", str(video), "-frames:v", str(args.frames),
        "-vf", "format=rgb24", "-threads", "1", "-y", str(root / "bbb_%04d.ppm"),
    ], check=True, timeout=120)
    for index in range(1, args.frames + 1):
        record(f"bbb_{index:04d}", root / f"bbb_{index:04d}.ppm", "bbb.zip",
               f"consecutive decoded animation frame {index}, seek=60s; not camera footage")
    manifest = {
        "sources": sources,
        "ffmpeg": subprocess.check_output([ffmpeg, "-version"], text=True).splitlines()[0],
        "video_sha256": digest(video),
        "cases": cases,
    }
    (root / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Prepared {len(cases)} inputs in {root}")


if __name__ == "__main__":
    main()
