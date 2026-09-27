#!/usr/bin/env python3
"""Verify native ICE reports lost consent and a new session restores media."""
import argparse
import asyncio
import json
import subprocess
import sys
import time
from pathlib import Path

from run_matrix import EXAMPLES, ROOT, evaluate, navigate, stop_group, wait_port
from test_loss import qdisc


def check_chrome(port):
    result = subprocess.run([sys.executable,
        str(EXAMPLES / "check_chrome.py"), "--port", str(port),
        "--timeout", "20"], text=True, capture_output=True, timeout=30)
    return result.returncode == 0, json.loads(result.stdout) if result.stdout else {}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--video", type=Path, required=True)
    parser.add_argument("--outage-seconds", type=int, default=35)
    parser.add_argument("--port-offset", type=int, default=170)
    parser.add_argument("--output-dir", type=Path,
                        default=Path("/tmp/ewrtc-recovery"))
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    cdp_port, http_port, ws_port = (9223 + args.port_offset,
                                    8081 + args.port_offset,
                                    8766 + args.port_offset)
    url = f"http://127.0.0.1:{http_port}/browser.html?ws={ws_port}"
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
            "--http-port", str(http_port), "--ws-port", str(ws_port),
            "--output-dir", str(args.output_dir)],
            cwd=ROOT, stdout=server_log, stderr=subprocess.STDOUT,
            start_new_session=True)
        wait_port(http_port)
        asyncio.run(navigate(url, cdp_port))
        initial_ok, initial = check_chrome(cdp_port)
        if not initial_ok:
            raise RuntimeError("initial connection failed: " + repr(initial))
        try:
            qdisc(100)
            time.sleep(args.outage_seconds)
        finally:
            qdisc()
        disconnected_state = None
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            state = json.loads(asyncio.run(evaluate(cdp_port,
                "JSON.stringify(window.testState)")))
            if state.get("sdkState") in (4, 5):
                disconnected_state = state
                break
            time.sleep(0.5)
        asyncio.run(navigate("about:blank", cdp_port))
        time.sleep(1)
        asyncio.run(navigate(url, cdp_port))
        recovered_ok, recovered = check_chrome(cdp_port)
        report = {"outage_seconds": args.outage_seconds,
                  "initial": initial, "disconnected": disconnected_state,
                  "recovered": recovered,
                  "passed": initial_ok and disconnected_state is not None and
                            recovered_ok}
        (args.output_dir / "results.json").write_text(
            json.dumps(report, indent=2) + "\n")
        print(f"outage/recreate: {'PASS' if report['passed'] else 'FAIL'}",
              flush=True)
        if not report["passed"]:
            raise SystemExit(1)
    finally:
        try:
            qdisc()
        finally:
            if server:
                stop_group(server)
            stop_group(chrome)
            server_log.close()
            chrome_log.close()


if __name__ == "__main__":
    main()
