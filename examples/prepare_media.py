#!/usr/bin/env python3
"""Convert Annex-B H.264 with AUD NALs to demo length-prefixed access units."""
import argparse
import struct
from pathlib import Path


def nal_units(data: bytes):
    starts = []
    i = 0
    while i + 3 < len(data):
        if data[i:i+3] == b"\0\0\1":
            starts.append(i)
            i += 3
        elif data[i:i+4] == b"\0\0\0\1":
            starts.append(i)
            i += 4
        else:
            i += 1
    for at, start in enumerate(starts):
        end = starts[at + 1] if at + 1 < len(starts) else len(data)
        prefix = 4 if data[start:start+4] == b"\0\0\0\1" else 3
        yield data[start:end], data[start + prefix] & 31


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    frames = []
    frame = bytearray()
    for nalu, kind in nal_units(args.input.read_bytes()):
        if kind == 9 and frame:
            frames.append(bytes(frame))
            frame.clear()
        frame.extend(nalu)
    if frame:
        frames.append(bytes(frame))
    with args.output.open("wb") as out:
        for frame in frames:
            out.write(struct.pack(">I", len(frame)))
            out.write(frame)
    print(f"{len(frames)} access units -> {args.output}")


if __name__ == "__main__":
    main()
