#!/usr/bin/env python3
"""Derive a small, video-only camera configuration; preserve the original INI."""
import argparse
from pathlib import Path

CHANGES = {
    "audio.0": {"enable": "0"},
    "video.source": {"enable_ivs": "0", "enable_jpeg": "0", "enable_venc_1": "0",
                     "enable_venc_2": "0", "enable_npu": "0", "enable_wrap": "0"},
    "video.0": {"width": "1280", "height": "720", "max_width": "1280", "max_height": "720",
                "output_data_type": "H.264", "h264_profile": "baseline",
                "src_frame_rate_num": "25", "dst_frame_rate_num": "25", "gop": "25",
                "max_rate": "1000", "mid_rate": "800", "buffer_count": "2",
                "buffer_size": "524288", "enable_motion_deblur": "0"},
    "osd.common": {"enable_osd": "0"},
    "event.regional_invasion": {"enabled": "0"},
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--gop", type=int, default=25,
                        help="Periodic keyframe interval at 25 fps (default: 25 = 1 s; use the IDR bridge for fast start)")
    args = parser.parse_args()
    if not 1 <= args.gop <= 250:
        parser.error("--gop must be between 1 and 250 frames")
    changes = {section: dict(values) for section, values in CHANGES.items()}
    changes["video.0"]["gop"] = str(args.gop)
    if args.source.resolve() == args.destination.resolve():
        parser.error("Use a separate destination to preserve the original configuration")
    section, output, seen = "", [], set()
    for line in args.source.read_text().splitlines():
        if line.startswith("["):
            section = line.strip("[]")
        if "=" in line:
            key = line.split("=", 1)[0].strip()
            if key in changes.get(section, {}):
                line = f"{key} = {changes[section][key]}"
                seen.add((section, key))
        output.append(line)
    missing = {(s, k) for s, keys in changes.items() for k in keys} - seen
    if missing:
        parser.error(f"Unexpected firmware configuration, missing keys: {sorted(missing)}")
    args.destination.write_text("\n".join(output) + "\n")


if __name__ == "__main__":
    main()
