#!/usr/bin/env python3
"""Run the Chrome direct/TURN matrix against four trimmed SDK builds."""
import argparse
import asyncio
import json
import os
import signal
import socket
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

import psutil
from websockets.asyncio.client import connect

ROOT = Path(__file__).resolve().parents[1]
EXAMPLES = Path(__file__).resolve().parent


def wait_port(port, timeout=10):
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        try:
            with socket.create_connection(("127.0.0.1", port), 0.2):
                return
        except OSError:
            time.sleep(0.1)
    raise TimeoutError(f"port {port} did not open")


async def evaluate(port, expression):
    pages = json.load(urllib.request.urlopen(f"http://127.0.0.1:{port}/json"))
    page = next(p for p in pages if p["type"] == "page")
    async with connect(page["webSocketDebuggerUrl"], origin="http://localhost") as ws:
        await ws.send(json.dumps({"id": 1, "method": "Runtime.evaluate",
            "params": {"expression": expression, "returnByValue": True,
                       "awaitPromise": True}}))
        while True:
            response = json.loads(await ws.recv())
            if response.get("id") == 1:
                return response.get("result", {}).get("result", {}).get("value")


async def navigate(url, port):
    pages = json.load(urllib.request.urlopen(f"http://127.0.0.1:{port}/json"))
    page = next(p for p in pages if p["type"] == "page")
    async with connect(page["webSocketDebuggerUrl"], origin="http://localhost") as ws:
        await ws.send(json.dumps({"id": 1, "method": "Page.navigate",
                                  "params": {"url": url}}))
        while json.loads(await ws.recv()).get("id") != 1:
            pass


def stop_group(process):
    if process.poll() is None:
        os.killpg(process.pid, signal.SIGTERM)
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--video", type=Path, required=True)
    parser.add_argument("--width", type=int, required=True)
    parser.add_argument("--height", type=int, required=True)
    parser.add_argument("--turn-port", type=int, default=3479)
    parser.add_argument("--turn-user", default="test")
    parser.add_argument("--turn-pass", default="testpass")
    parser.add_argument("--demo", type=Path,
                        help="Override the selected trimmed-build demo executable")
    parser.add_argument("--output-dir", type=Path, default=Path("/tmp/ewrtc-matrix"))
    parser.add_argument("--only", help="Run a single ice-dtls-mode case")
    parser.add_argument("--soak-seconds", type=int, default=0)
    parser.add_argument("--port-offset", type=int, default=0)
    args = parser.parse_args()
    args.video = args.video.resolve()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    chrome_log = (args.output_dir / "chrome.log").open("w")
    cdp_port = 9223 + args.port_offset
    http_port = 8081 + args.port_offset
    ws_port = 8766 + args.port_offset
    chrome = subprocess.Popen([
        "google-chrome", "--headless=new", "--no-sandbox", "--disable-gpu",
        "--autoplay-policy=no-user-gesture-required",
        "--remote-allow-origins=http://localhost", f"--remote-debugging-port={cdp_port}",
        f"--user-data-dir={args.output_dir / 'chrome-profile'}", "about:blank"],
        stdout=chrome_log, stderr=subprocess.STDOUT, start_new_session=True)
    results = []
    try:
        wait_port(cdp_port)
        for ice in ("native", "juice"):
            for dtls in ("openssl", "mbedtls"):
                for mode in ("direct", "relay"):
                    label = f"{args.width}x{args.height}-{ice}-{dtls}-{mode}"
                    if args.only and args.only != f"{ice}-{dtls}-{mode}":
                        continue
                    demo = args.demo.resolve() if args.demo else \
                        ROOT / f"build-{ice}-{dtls}/ewrtc_demo"
                    out = args.output_dir / label
                    out.mkdir(exist_ok=True)
                    command = [sys.executable, str(EXAMPLES / "signaling.py"),
                        "--ice", ice, "--dtls", dtls, "--demo", str(demo),
                        "--video", str(args.video), "--http-port", str(http_port),
                        "--ws-port", str(ws_port), "--output-dir", str(out)]
                    query = f"?ws={ws_port}"
                    if mode == "relay":
                        command += ["--turn", "127.0.0.1", "--turn-port",
                            str(args.turn_port), "--turn-user", args.turn_user,
                            "--turn-pass", args.turn_pass, "--relay-only"]
                        query += (f"&turn=127.0.0.1:{args.turn_port}&user={args.turn_user}"
                                  f"&pass={args.turn_pass}&relay=1")
                    log = (out / "signaling.log").open("w")
                    server = subprocess.Popen(command, cwd=ROOT, stdout=log,
                        stderr=subprocess.STDOUT, start_new_session=True)
                    try:
                        wait_port(http_port)
                        asyncio.run(navigate(f"http://127.0.0.1:{http_port}/browser.html" + query,
                                             cdp_port))
                        time.sleep(0.5)
                        checked = subprocess.run([sys.executable,
                            str(EXAMPLES / "check_chrome.py"), "--port", str(cdp_port),
                            "--timeout", "20"], text=True, capture_output=True,
                            timeout=30)
                        detail = json.loads(checked.stdout) if checked.stdout else {}
                        state = detail.get("state", {})
                        dims = detail.get("dimensions", {})
                        passed = checked.returncode == 0 and dims == {
                            "videoWidth": args.width, "videoHeight": args.height}
                        if mode == "relay" and "relay" not in state.get("stats", ""):
                            passed = False
                        resource = None
                        if passed and args.soak_seconds:
                            children = psutil.Process(server.pid).children(recursive=True)
                            device = next((p for p in children if "ewrtc_demo" in p.name()), None)
                            if not device:
                                raise RuntimeError("SDK demo process not found")
                            device.cpu_percent(interval=None)
                            samples = []
                            started = time.monotonic()
                            next_report = started + 60
                            while time.monotonic() - started < args.soak_seconds:
                                time.sleep(1)
                                samples.append({"cpu_percent_one_core": device.cpu_percent(interval=None),
                                    "rss_bytes": device.memory_info().rss,
                                    "threads": device.num_threads()})
                                if time.monotonic() >= next_report:
                                    print(f"{label}: {int(time.monotonic() - started)}s transfer",
                                          flush=True)
                                    next_report += 60
                            resource = {"seconds": args.soak_seconds,
                                "cpu_percent_one_core_mean": round(sum(s["cpu_percent_one_core"]
                                    for s in samples) / len(samples), 2),
                                "rss_bytes_peak": max(s["rss_bytes"] for s in samples),
                                "threads_peak": max(s["threads"] for s in samples)}
                            asyncio.run(evaluate(cdp_port, "window.requestSdkStats()"))
                            time.sleep(0.3)
                            state = json.loads(asyncio.run(evaluate(cdp_port,
                                "JSON.stringify(window.testState)")))
                            inbound = json.loads(asyncio.run(evaluate(cdp_port,
                                "pc.getStats().then(s=>JSON.stringify([...s.values()].filter(x=>"
                                "x.type==='inbound-rtp'&&x.kind==='video').map(x=>({"
                                "framesDecoded:x.framesDecoded,keyFramesDecoded:x.keyFramesDecoded}))))")))
                            resource["chrome_video"] = inbound
                            passed = state.get("state") == "connected" and state.get("sdkState") == 3
                        results.append({"case": label, "passed": passed,
                            "state": state, "dimensions": dims,
                            "resource": resource,
                            "error": checked.stderr.strip()})
                        print(f"{label}: {'PASS' if passed else 'FAIL'}", flush=True)
                    except Exception as error:
                        results.append({"case": label, "passed": False,
                                        "error": repr(error)})
                        print(f"{label}: FAIL {error}", flush=True)
                    finally:
                        asyncio.run(navigate("about:blank", cdp_port))
                        time.sleep(0.4)
                        stop_group(server)
                        log.close()
                    audio = out / "received.opuspkts"
                    if results[-1]["passed"]:
                        checked = subprocess.run([sys.executable,
                            str(EXAMPLES / "verify_audio.py"), str(audio)],
                            text=True, capture_output=True)
                        results[-1]["audio_verification"] = checked.stdout.strip()
                        if checked.returncode:
                            results[-1]["passed"] = False
                            results[-1]["error"] = checked.stderr.strip()
        report = args.output_dir / f"results-{args.width}x{args.height}.json"
        report.write_text(json.dumps(results, indent=2) + "\n")
        print(f"Results: {report}")
        if not all(r["passed"] for r in results):
            raise SystemExit(1)
    finally:
        stop_group(chrome)
        chrome_log.close()


if __name__ == "__main__":
    main()
