#!/usr/bin/env python3
"""Test the native bridge and exported mock: malformed input, merge, rate limit, ownership."""
import asyncio
import os
import socket
import sys
from pathlib import Path

ENDPOINT = "/tmp/ewrtc-idr.sock"


async def spawn():
    return await asyncio.create_subprocess_exec(str(Path(sys.argv[2]).resolve()), stdin=-1,
        stdout=-1, stderr=-1, env={**os.environ, "LD_PRELOAD": str(Path(sys.argv[1]).resolve())})


async def stop(process):
    process.stdin.close()
    await asyncio.wait_for(process.wait(), 3)
    assert process.returncode == 0


async def main():
    process = await spawn()
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    try:
        assert b"bridge ready" in await asyncio.wait_for(process.stderr.readline(), 3)
        assert os.stat(ENDPOINT).st_mode & 0o777 == 0o600
        for data in (b"EWRTC_IDR_1", b"wrong", b"EWRTC_IDR_1\0extra", b"x" * 256):
            sock.sendto(data, ENDPOINT)
        try:
            await asyncio.wait_for(process.stdout.readline(), .15)
            raise AssertionError("Malformed packet reached encoder")
        except asyncio.TimeoutError:
            pass
        # Hundreds of callers still cannot invoke the hardware more than 10 times/second.
        sock.setblocking(False)
        for _ in range(100):
            try:
                sock.sendto(b"EWRTC_IDR_1\0", ENDPOINT)
            except BlockingIOError:
                await asyncio.sleep(.001)
        stamps = []
        while True:
            try:
                line = await asyncio.wait_for(process.stdout.readline(), .25)
            except asyncio.TimeoutError:
                break
            channel, instant, stamp = map(int, line.split())
            assert channel == 0 and instant == 0
            stamps.append(stamp)
        assert 1 <= len(stamps) <= 3, stamps
        assert all(b - a >= 99 for a, b in zip(stamps, stamps[1:])), stamps
        duplicate = await spawn()
        assert b"unavailable" in await asyncio.wait_for(duplicate.stderr.readline(), 3)
        await stop(duplicate)
        assert Path(ENDPOINT).exists(), "Second process removed active endpoint"
        sock.sendto(b"EWRTC_IDR_1\0", ENDPOINT)
        assert await asyncio.wait_for(process.stdout.readline(), .1)
    finally:
        sock.close()
        await stop(process)
    assert not Path(ENDPOINT).exists()
    print("PASS: input validation, bounded IDR frequency, duplicate ownership, cleanup")


if __name__ == "__main__":
    asyncio.run(main())
