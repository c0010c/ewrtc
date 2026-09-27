#!/usr/bin/env python3
"""Build the SDK, prepare a loopable sample, and serve the Chrome player."""
import argparse
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def prepare(build, source):
    build.mkdir(parents=True, exist_ok=True)
    subprocess.run([
        "cmake", "-S", str(ROOT), "-B", str(build),
        "-DCMAKE_BUILD_TYPE=Release", "-DEWRTC_WITH_NATIVE_ICE=ON",
        "-DEWRTC_WITH_LIBJUICE=OFF", "-DEWRTC_WITH_OPENSSL=ON",
        "-DEWRTC_WITH_MBEDTLS=OFF", "-DEWRTC_BUILD_EXAMPLES=ON",
        "-DEWRTC_ENFORCE_DEPENDENCY_LOCK=OFF",
    ], check=True)
    subprocess.run(["cmake", "--build", str(build), "--target", "ewrtc_demo",
                    "-j", str(min(os.cpu_count() or 2, 8))], check=True)
    media = build / "player-media"
    media.mkdir(exist_ok=True)
    h264, frames = media / "sample.h264", media / "sample.frames"
    inputs = ["-i", str(source)] if source else [
        "-f", "lavfi", "-i", "testsrc2=size=1280x720:rate=30"]
    # Normalize High-profile samples to the SDK's Baseline / 30 fps / AUD contract.
    subprocess.run([
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-y", *inputs,
        "-map", "0:v:0", "-an", "-vf", "scale=1280:720", "-r", "30", "-t", "10",
        "-c:v", "libx264", "-preset", "ultrafast", "-b:v", "1M",
        "-maxrate", "1M", "-bufsize", "2M", "-pix_fmt", "yuv420p",
        "-profile:v", "baseline", "-level:v", "3.1", "-x264-params",
        "keyint=60:min-keyint=60:scenecut=0:bframes=0:repeat-headers=1:aud=1",
        "-f", "h264", str(h264),
    ], check=True)
    subprocess.run([sys.executable, str(ROOT / "examples/prepare_media.py"),
                    str(h264), str(frames)], check=True)
    return frames


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--http-port", type=int, default=8080)
    parser.add_argument("--ws-port", type=int, default=8765)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build-player")
    parser.add_argument("--source", type=Path,
                        help="Optional local video; defaults to a generated test pattern")
    args = parser.parse_args()
    build = args.build_dir.resolve()
    frames = prepare(build, args.source.resolve() if args.source else None)
    command = [sys.executable, "-u", str(ROOT / "examples/signaling.py"),
               "--demo", str(build / "ewrtc_demo"), "--video", str(frames),
               "--http-port", str(args.http_port), "--ws-port", str(args.ws_port),
               "--page", "player/", "--output-dir", str(build / "player-output")]
    os.execv(sys.executable, command)


if __name__ == "__main__":
    main()
