#!/usr/bin/env python3
"""Keep a Chrome session connected while an intentionally tiny send queue fills."""
import argparse
import asyncio
import json
import subprocess
import sys
import time
from pathlib import Path

import psutil

from run_matrix import EXAMPLES, ROOT, evaluate, navigate, stop_group, wait_port


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--video", type=Path, required=True)
    parser.add_argument("--seconds", type=int, default=15)
    parser.add_argument("--queue-limit", type=int, default=16384)
    parser.add_argument("--port-offset", type=int, default=220)
    parser.add_argument("--output-dir", type=Path,
                        default=Path("/tmp/ewrtc-backpressure"))
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    cdp_port, http_port, ws_port = (9223 + args.port_offset,
                                    8081 + args.port_offset,
                                    8766 + args.port_offset)
    chrome_log = (args.output_dir / "chrome.log").open("w")
    server_log = (args.output_dir / "signaling.log").open("w")
    chrome = subprocess.Popen([
        "google-chrome", "--headless=new", "--no-sandbox", "--disable-gpu",
        "--autoplay-policy=no-user-gesture-required",
        "--remote-allow-origins=http://localhost",
        f"--remote-debugging-port={cdp_port}",
        f"--user-data-dir={args.output_dir / 'chrome-profile'}", "about:blank"],
        stdout=chrome_log, stderr=subprocess.STDOUT, start_new_session=True)
    server = None
    try:
        wait_port(cdp_port)
        server = subprocess.Popen([
            sys.executable, str(EXAMPLES / "signaling.py"),
            "--ice", "native", "--dtls", "openssl",
            "--demo", str(ROOT / "build-native-openssl/ewrtc_demo"),
            "--video", str(args.video.resolve()),
            "--queue-limit", str(args.queue_limit),
            "--http-port", str(http_port), "--ws-port", str(ws_port),
            "--output-dir", str(args.output_dir)],
            cwd=ROOT, stdout=server_log, stderr=subprocess.STDOUT,
            start_new_session=True)
        wait_port(http_port)
        asyncio.run(navigate(
            f"http://127.0.0.1:{http_port}/browser.html?ws={ws_port}",
            cdp_port))
        asyncio.run(evaluate(cdp_port, "document.querySelector('#start').click()"))
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            state = json.loads(asyncio.run(evaluate(cdp_port,
                "JSON.stringify(window.testState)")))
            if state.get("state") == "connected" and state.get("sdkState") == 3:
                break
            if state.get("error"):
                raise RuntimeError(state["error"])
            time.sleep(0.2)
        else:
            raise TimeoutError("Chrome did not connect")
        children = psutil.Process(server.pid).children(recursive=True)
        device = next(p for p in children if "ewrtc_demo" in p.name())
        rss = []
        for _ in range(args.seconds):
            rss.append(device.memory_info().rss)
            time.sleep(1)
        asyncio.run(evaluate(cdp_port, "window.requestSdkStats()"))
        time.sleep(0.5)
        state = json.loads(asyncio.run(evaluate(cdp_port,
            "JSON.stringify(window.testState)")))
        stats = {}
        for item in state.get("stats", "").split():
            if "=" in item:
                key, value = item.split("=", 1)
                if value.isdecimal():
                    stats[key] = int(value)
        report = {"queue_limit_bytes": args.queue_limit,
                  "seconds": args.seconds, "state": state,
                  "sdk": stats, "rss_bytes_min": min(rss),
                  "rss_bytes_max": max(rss)}
        report["passed"] = (state.get("state") == "connected" and
            stats.get("backpressure", 0) > 0 and
            stats.get("queue", args.queue_limit + 1) <= args.queue_limit and
            stats.get("sent_audio", 0) > 0 and
            report["rss_bytes_max"] - report["rss_bytes_min"] < 4 * 1024 * 1024)
        (args.output_dir / "results.json").write_text(
            json.dumps(report, indent=2) + "\n")
        print(f"backpressure: {'PASS' if report['passed'] else 'FAIL'}",
              flush=True)
        if not report["passed"]:
            raise SystemExit(1)
    finally:
        if server:
            stop_group(server)
        stop_group(chrome)
        server_log.close()
        chrome_log.close()


if __name__ == "__main__":
    main()
