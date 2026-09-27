#!/usr/bin/env python3
"""Local Chrome interoperability harness for ewrtc_demo.

The WebSocket carries JSON; the C demo uses length-prefixed records on pipes.
One WebSocket connection owns one SDK process and therefore one SDK session.
"""
import argparse
import asyncio
import json
import os
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from threading import Thread

from websockets.asyncio.server import serve
from websockets.exceptions import ConnectionClosed


def http_server(port: int):
    handler = partial(SimpleHTTPRequestHandler, directory=str(Path(__file__).parent))
    server = ThreadingHTTPServer(("127.0.0.1", port), handler)
    Thread(target=server.serve_forever, daemon=True).start()
    return server


async def read_records(proc, ws):
    try:
        while True:
            header = await proc.stdout.readline()
            if not header:
                break
            kind, length = header.decode("ascii").strip().split(" ", 1)
            size = int(length)
            if size > 65536:
                raise ValueError("SDK record exceeds signaling limit")
            payload = await proc.stdout.readexactly(size)
            await ws.send(json.dumps({"type": kind.lower(), "value": payload.decode()}))
    except (asyncio.IncompleteReadError, ConnectionError, ConnectionClosed):
        pass
    except Exception as error:
        print(f"SDK record reader failed: {error!r}", flush=True)
        raise


async def read_stderr(proc):
    while line := await proc.stderr.readline():
        print(line.decode(errors="replace"), end="", flush=True)


async def client(ws, args):
    command = [str(args.demo), args.ice, args.dtls, str(args.video)]
    if args.stun or args.turn or args.queue_limit:
        command += [args.stun or "", args.turn or "", args.turn_user or "",
                    args.turn_pass or "", "1" if args.relay_only else "0",
                    str(args.turn_port), str(args.queue_limit)]
    proc = await asyncio.create_subprocess_exec(
        *command, stdin=asyncio.subprocess.PIPE,
        stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE,
        cwd=args.output_dir)
    reader = asyncio.create_task(read_records(proc, ws))
    stderr = asyncio.create_task(read_stderr(proc))
    try:
        async for wire in ws:
            message = json.loads(wire)
            kind = message.get("type")
            command_name = {"offer": "OFFER", "answer": "ANSWER", "create_offer": "CREATE_OFFER", "candidate": "CANDIDATE",
                            "end": "END", "stats": "STATS"}.get(kind)
            if not command_name:
                continue
            data = str(message.get("value") or "").encode()
            if len(data) > 65536:
                raise ValueError("signaling message too large")
            proc.stdin.write(f"{command_name} {len(data)}\n".encode() + data)
            await proc.stdin.drain()
    except (BrokenPipeError, ConnectionResetError):
        pass
    finally:
        if proc.stdin and not proc.stdin.is_closing():
            proc.stdin.close()
        await proc.wait()
        await reader
        await stderr
        if proc.returncode:
            print(f"SDK process exited: {proc.returncode}", flush=True)


async def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--ice", choices=("native", "juice"), default="native")
    parser.add_argument("--dtls", choices=("openssl", "mbedtls"), default="openssl")
    parser.add_argument("--demo", type=Path, default=Path(__file__).resolve().parents[1] / "build/ewrtc_demo")
    parser.add_argument("--video", type=Path, required=True)
    parser.add_argument("--stun", default="")
    parser.add_argument("--turn", default="")
    parser.add_argument("--turn-user", default="")
    parser.add_argument("--turn-pass", default="")
    parser.add_argument("--turn-port", type=int, default=3478)
    parser.add_argument("--relay-only", action="store_true")
    parser.add_argument("--queue-limit", type=int, default=0)
    parser.add_argument("--http-port", type=int, default=8080)
    parser.add_argument("--ws-port", type=int, default=8765)
    parser.add_argument("--page", default="browser.html")
    parser.add_argument("--output-dir", type=Path, default=Path.cwd())
    args = parser.parse_args()
    args.demo = args.demo.resolve()
    args.video = args.video.resolve()
    args.output_dir = args.output_dir.resolve()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    server = http_server(args.http_port)
    try:
        async with serve(lambda ws: client(ws, args), "127.0.0.1", args.ws_port):
            print(f"Open http://127.0.0.1:{args.http_port}/{args.page}?ws={args.ws_port}", flush=True)
            await asyncio.Future()
    finally:
        server.shutdown()


if __name__ == "__main__":
    asyncio.run(main())
