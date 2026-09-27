#!/usr/bin/env python3
"""Decode demo's received Opus packets and verify the Chrome test tone."""
import argparse
import ctypes
import ctypes.util
import math
import struct
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("packets", type=Path)
    parser.add_argument("--expected-hz", type=float, default=440)
    args = parser.parse_args()
    opus = ctypes.CDLL(ctypes.util.find_library("opus"))
    opus.opus_decoder_create.argtypes = (ctypes.c_int, ctypes.c_int,
                                         ctypes.POINTER(ctypes.c_int))
    opus.opus_decoder_create.restype = ctypes.c_void_p
    opus.opus_decode.argtypes = (ctypes.c_void_p, ctypes.c_void_p,
                                 ctypes.c_int, ctypes.c_void_p,
                                 ctypes.c_int, ctypes.c_int)
    opus.opus_decode.restype = ctypes.c_int
    opus.opus_decoder_destroy.argtypes = (ctypes.c_void_p,)
    error = ctypes.c_int()
    decoder = opus.opus_decoder_create(48000, 1, ctypes.byref(error))
    if not decoder or error.value:
        raise RuntimeError(f"Opus decoder creation failed: {error.value}")
    packet_count = 0
    samples = []
    data = args.packets.read_bytes()
    at = 0
    try:
        while at + 10 <= len(data):
            length, timestamp, sequence, _ = struct.unpack_from(">HIHH", data, at)
            at += 10
            if at + length > len(data):
                break  # The media demo may still be writing its final packet.
            payload = ctypes.create_string_buffer(data[at:at + length])
            at += length
            pcm = (ctypes.c_int16 * 5760)()
            n = opus.opus_decode(decoder, payload, length, pcm, 5760, 0)
            if n < 0:
                raise RuntimeError(f"Opus decode failed: {n} at RTP seq {sequence}")
            packet_count += 1
            samples.extend(pcm[:n])
    finally:
        opus.opus_decoder_destroy(decoder)
    if packet_count < 30 or len(samples) < 48000:
        raise SystemExit(f"too little Opus audio: {packet_count} packets")
    # Skip the beginning of the stream to avoid codec priming transients.
    samples = samples[4800:52800]
    rms = math.sqrt(sum(x * x for x in samples) / len(samples))
    crossings = sum(a <= 0 < b for a, b in zip(samples, samples[1:]))
    frequency = crossings * 48000 / len(samples)
    print(f"Opus packets={packet_count}, decoded RMS={rms:.1f}, tone={frequency:.1f} Hz")
    if rms < 100 or abs(frequency - args.expected_hz) > 35:
        raise SystemExit("decoded audio does not match the expected test tone")


if __name__ == "__main__":
    main()
