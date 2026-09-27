#!/usr/bin/env python3
"""Route browser sessions over one persistent camera process/SSH connection."""
import argparse
import asyncio
import contextlib
import json
import signal
from pathlib import Path

from websockets.asyncio.server import serve
from websockets.exceptions import ConnectionClosed
from signaling import http_server, read_stderr

COMMANDS = {name.lower(): name for name in
            ("OFFER", "ANSWER", "CREATE_OFFER", "CANDIDATE", "END", "STATS")}


class Gateway:
    def __init__(self, process, limit):
        self.process, self.limit = process, limit
        self.peers = {}
        self.next_id = 1
        self.lock = asyncio.Lock()
        self.ready = asyncio.Event()
        self.failed = asyncio.Event()

    async def send(self, identity, kind, data=""):
        payload = data.encode()
        if len(payload) > 65536:
            raise ValueError("Signaling payload exceeds limit")
        try:
            async with self.lock:
                self.process.stdin.write(f"{identity} {kind} {len(payload)}\n".encode() + payload)
                await asyncio.wait_for(self.process.stdin.drain(), 5)
        except (OSError, asyncio.TimeoutError):
            self.failed.set()
            raise

    async def read(self):
        try:
            while header := await self.process.stdout.readline():
                identity, kind, length = header.decode("ascii").split()
                identity, length = int(identity), int(length)
                if not 0 <= length <= 65536:
                    raise ValueError("Invalid backend record length")
                data = (await self.process.stdout.readexactly(length)).decode()
                if identity == 0 and kind == "READY":
                    print(f"Shared camera backend ready: {data}", flush=True)
                    self.ready.set()
                elif identity in self.peers:
                    queue, overflow = self.peers[identity]
                    if kind not in ("CREATED", "CLOSED"):
                        try:
                            queue.put_nowait(json.dumps({"type": kind.lower(), "value": data}))
                        except asyncio.QueueFull:
                            overflow.set()
        except (ValueError, OSError, asyncio.IncompleteReadError) as error:
            print(f"Camera transport failed: {error}", flush=True)
        finally:
            self.failed.set()

    async def client(self, ws):
        if self.failed.is_set() or len(self.peers) >= self.limit:
            await ws.send(json.dumps({"type": "error", "value": "观看会话数已达上限或摄像头服务暂不可用。"}))
            await ws.close(code=1013)
            return
        identity = self.next_id
        self.next_id += 1
        queue, overflow = asyncio.Queue(maxsize=32), asyncio.Event()
        self.peers[identity] = queue, overflow

        async def receive():
            async for wire in ws:
                message = json.loads(wire)
                if not isinstance(message, dict):
                    raise ValueError("Expected JSON object")
                if kind := COMMANDS.get(message.get("type")):
                    await self.send(identity, kind, str(message.get("value") or ""))

        async def transmit():
            while True:
                await asyncio.wait_for(ws.send(await queue.get()), 5)

        tasks = []
        try:
            await self.send(identity, "CREATE")
            tasks = [asyncio.create_task(coro) for coro in
                     (receive(), transmit(), overflow.wait(), self.failed.wait())]
            done, _ = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
            for task in done:
                task.result()
        except (ConnectionClosed, OSError, ValueError, TypeError, asyncio.TimeoutError):
            pass
        finally:
            for task in tasks:
                task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)
            # Retain admission slot until CLOSE is queued, preserving CREATE/CLOSE order.
            if not self.failed.is_set():
                with contextlib.suppress(OSError, asyncio.TimeoutError):
                    await self.send(identity, "CLOSE")
            self.peers.pop(identity, None)
            await ws.close()


async def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--demo", type=Path, required=True)
    parser.add_argument("--max-sessions", type=int, choices=range(1, 9), default=4)
    parser.add_argument("--http-port", type=int, default=8081)
    parser.add_argument("--ws-port", type=int, default=8766)
    args = parser.parse_args()
    stop = asyncio.Event()
    for sig in (signal.SIGINT, signal.SIGTERM):
        asyncio.get_running_loop().add_signal_handler(sig, stop.set)
    process = await asyncio.create_subprocess_exec(
        str(args.demo.resolve()), str(args.max_sessions), stdin=asyncio.subprocess.PIPE,
        stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
    gateway = Gateway(process, args.max_sessions)
    reader = asyncio.create_task(gateway.read())
    stderr = asyncio.create_task(read_stderr(process))
    ready = asyncio.create_task(gateway.ready.wait())
    failed = asyncio.create_task(gateway.failed.wait())
    stopped = asyncio.create_task(stop.wait())
    http = None
    try:
        await asyncio.wait((ready, failed, stopped), timeout=10, return_when=asyncio.FIRST_COMPLETED)
        if not gateway.ready.is_set() or gateway.failed.is_set():
            raise RuntimeError("Camera backend did not become ready")
        http = http_server(args.http_port)
        async with serve(gateway.client, "127.0.0.1", args.ws_port, max_size=65536, max_queue=8, close_timeout=2):
            print(f"Open http://127.0.0.1:{args.http_port}/player/?ws={args.ws_port}&source=camera", flush=True)
            await asyncio.wait((failed, stopped), return_when=asyncio.FIRST_COMPLETED)
        if gateway.failed.is_set() and not stop.is_set():
            raise RuntimeError("Shared camera process disconnected")
    finally:
        if http:
            http.shutdown()
        process.stdin.close()
        try:
            await asyncio.wait_for(process.wait(), 12)
        except asyncio.TimeoutError:
            process.terminate()
            try:
                await asyncio.wait_for(process.wait(), 3)
            except asyncio.TimeoutError:
                process.kill()
                await process.wait()
        for task in (reader, stderr, ready, failed, stopped):
            task.cancel()
        await asyncio.gather(reader, stderr, ready, failed, stopped, return_exceptions=True)


if __name__ == "__main__":
    asyncio.run(main())
