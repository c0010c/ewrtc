#!/usr/bin/env python3
"""Local ISP UI, independent of camera capture and WebRTC signaling services."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from isp_protocol import IspClient

ROOT = Path(__file__).resolve().parent
ASSETS = {
    "/": (ROOT / "isp_ui/index.html", "text/html; charset=utf-8"),
    "/isp.css": (ROOT / "isp_ui/isp.css", "text/css; charset=utf-8"),
    "/isp.js": (ROOT / "isp_ui/isp.js", "text/javascript; charset=utf-8"),
    "/client.js": (ROOT.parent / "player/client.js", "text/javascript; charset=utf-8"),
}


class Handler(BaseHTTPRequestHandler):
    def setup(self):
        super().setup()
        self.connection.settimeout(10)

    def reply(self, status, body, kind="application/json; charset=utf-8"):
        if not isinstance(body, bytes):
            body = json.dumps(body, ensure_ascii=False).encode()
        self.send_response(status)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("X-Frame-Options", "DENY")
        self.end_headers()
        self.wfile.write(body)

    def local_request(self):
        port = self.server.server_port
        hosts = {f"127.0.0.1:{port}", f"localhost:{port}"}
        host = self.headers.get("Host")
        origin = self.headers.get("Origin")
        if host not in hosts or (origin and origin != f"http://{host}"):
            self.reply(403, {"error": "仅允许本机同源访问"})
            return False
        return True

    def do_GET(self):
        if not self.local_request():
            return
        path = self.path.split("?", 1)[0]
        if path == "/api/isp":
            self.isp()
        elif path in ASSETS:
            file, kind = ASSETS[path]
            self.reply(200, file.read_bytes(), kind)
        else:
            self.reply(404, {"error": "未找到"})

    def do_POST(self):
        if not self.local_request():
            return
        if self.path != "/api/isp":
            self.reply(404, {"error": "未找到"})
            return
        if self.headers.get("Content-Type", "").split(";")[0] != "application/json":
            self.reply(415, {"error": "需要 JSON 请求"})
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if not 0 < length <= 1024:
                raise ValueError("请求长度无效")
            values = json.loads(self.rfile.read(length))
            if values is None:
                raise ValueError("参数不能为空")
        except (ValueError, UnicodeError) as error:
            self.reply(400, {"error": str(error)})
            return
        self.isp(values)

    def isp(self, values=None):
        try:
            self.reply(200, {"values": self.server.isp.exchange(values)})
        except ValueError as error:
            self.reply(400, {"error": str(error)})
        except (OSError, RuntimeError) as error:
            print(f"ISP error: {error}", flush=True)
            self.reply(503, {"error": "板子未确认此次操作，请重新读取参数后再试。"})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8082)
    parser.add_argument("--host", default=os.environ.get("EWRTC_DEVICE_HOST"))
    parser.add_argument("--key", default=os.environ.get("EWRTC_SSH_KEY"))
    args = parser.parse_args()
    if not args.host:
        parser.error("provide --host or set EWRTC_DEVICE_HOST")
    with tempfile.TemporaryDirectory(prefix="ewrtc-isp-") as directory:
        path = directory + "/rkipc.sock"
        command = ["ssh", "-N", "-T",
                   "-o", "BatchMode=yes", "-o", "ConnectTimeout=5",
                   "-o", "ExitOnForwardFailure=yes", "-o", "ServerAliveInterval=5",
                   "-o", "ServerAliveCountMax=2", "-L", path + ":/var/tmp/rkipc", args.host]
        if args.key:
            command[1:1] = ["-i", args.key]
        tunnel = subprocess.Popen(command)
        client = IspClient(path)
        try:
            deadline = time.monotonic() + 10
            while not Path(path).exists():
                if tunnel.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError("无法建立板子 ISP 控制连接")
                time.sleep(.05)
            print("ISP connected:", client.exchange(), flush=True)
            with ThreadingHTTPServer(("127.0.0.1", args.port), Handler) as server:
                server.isp = client
                server.timeout = .5
                print(f"ISP UI: http://127.0.0.1:{args.port}/", flush=True)
                while tunnel.poll() is None:
                    server.handle_request()
                raise RuntimeError("SSH 控制连接断开")
        finally:
            client.close()
            tunnel.terminate()
            try:
                tunnel.wait(timeout=3)
            except subprocess.TimeoutExpired:
                tunnel.kill()
                tunnel.wait()


if __name__ == "__main__":
    main()
