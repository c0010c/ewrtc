#!/usr/bin/env python3
"""Exercise lifetime and pipe-failure handling of a locally built camera app."""
import asyncio
import json
import sys


async def record(process, identity, kind, data=""):
    wire = data.encode()
    process.stdin.write(f"{identity} {kind} {len(wire)}\n".encode() + wire)
    await process.stdin.drain()


async def expect(process, identity, kind):
    async with asyncio.timeout(5):
        while True:
            header = await process.stdout.readline()
            assert header, f"Unexpected EOF waiting for {identity} {kind}"
            actual_id, actual_kind, length = header.decode().split()
            payload = (await process.stdout.readexactly(int(length))).decode()
            if int(actual_id) == identity and actual_kind == kind:
                return payload


async def start():
    process = await asyncio.create_subprocess_exec(sys.argv[1], "4", stdin=-1, stdout=-1, stderr=-1)
    await expect(process, 0, "READY")
    return process


async def finish(process):
    process.stdin.close()
    await asyncio.wait_for(process.wait(), 5)
    errors = (await process.stderr.read()).decode()
    assert "Sanitizer" not in errors and "runtime error:" not in errors, errors


async def main():
    process = await start()
    try:
        for identity in range(1, 5):
            await record(process, identity, "CREATE")
            await expect(process, identity, "CREATED")
        for identity in (1, 5):  # duplicate and over-capacity
            await record(process, identity, "CREATE")
            await expect(process, identity, "ERROR")
        await record(process, 0, "STATS")
        stats = json.loads(await expect(process, 0, "STATS"))
        assert stats["sessions"] == 4 and stats["workers"] == 1 and stats["sdk_threads"] == 2, stats
        for identity in range(1, 5):
            await record(process, identity, "CLOSE")
            await expect(process, identity, "CLOSED")
        for identity in range(10, 110):
            await record(process, identity, "CREATE")
            await expect(process, identity, "CREATED")
            await record(process, identity, "CLOSE")
            await expect(process, identity, "CLOSED")
        await record(process, 0, "STATS")
        assert json.loads(await expect(process, 0, "STATS"))["sessions"] == 0
    finally:
        await finish(process)
    for bad in (b"1 OFFER 65537\n", b"1 OFFER -1\n", b"1 OFFER 3\nx\x00y", b"X" * 128):
        process = await start()
        process.stdin.write(bad)
        await process.stdin.drain()
        await finish(process)
        assert process.returncode != 0
    # A blocked signal consumer must not hold SDK callbacks or shutdown forever.
    process = await start()
    try:
        for _ in range(10000):
            process.stdin.write(b"0 STATS 0\n")
            await asyncio.wait_for(process.stdin.drain(), 5)
    except (BrokenPipeError, ConnectionResetError):
        pass
    await finish(process)
    assert process.returncode != 0
    print("PASS: capacity, duplicate IDs, 100 session lifetimes, invalid input, blocked stdout shutdown")


if __name__ == "__main__":
    asyncio.run(main())
