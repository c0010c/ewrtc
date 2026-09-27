#!/usr/bin/env python3
"""Connect Chrome to a fresh C SDK process, then close it, repeatedly."""
import argparse
import asyncio
import json
import subprocess
import sys
import time
from pathlib import Path

import psutil

from run_matrix import EXAMPLES, ROOT, navigate, stop_group, wait_port


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--video", type=Path, required=True)
    parser.add_argument("--cycles", type=int, default=100)
    parser.add_argument("--port-offset", type=int, default=60)
    parser.add_argument("--output-dir", type=Path,
                        default=Path("/tmp/ewrtc-lifecycle"))
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
    results = []
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
        for cycle in range(1, args.cycles + 1):
            asyncio.run(navigate(
                f"http://127.0.0.1:{http_port}/browser.html?ws={ws_port}",
                cdp_port))
            checked = subprocess.run([sys.executable,
                str(EXAMPLES / "check_chrome.py"), "--port", str(cdp_port),
                "--timeout", "20"], text=True, capture_output=True, timeout=30)
            state = json.loads(checked.stdout) if checked.stdout else {}
            asyncio.run(navigate("about:blank", cdp_port))
            until = time.monotonic() + 5
            while time.monotonic() < until:
                children = psutil.Process(server.pid).children(recursive=True)
                if not any("ewrtc_demo" in p.name() for p in children):
                    break
                time.sleep(0.05)
            exited = not any("ewrtc_demo" in p.name() for p in
                             psutil.Process(server.pid).children(recursive=True))
            passed = checked.returncode == 0 and exited
            results.append({"cycle": cycle, "passed": passed,
                            "sdk_state": state.get("state", {}).get("sdkState"),
                            "error": checked.stderr.strip()})
            if cycle % 10 == 0 or not passed:
                print(f"cycle {cycle}/{args.cycles}: "
                      f"{'PASS' if passed else 'FAIL'}", flush=True)
            if not passed:
                break
        report = args.output_dir / "results.json"
        report.write_text(json.dumps(results, indent=2) + "\n")
        print(f"Results: {report}")
        if len(results) != args.cycles or not all(r["passed"] for r in results):
            raise SystemExit(1)
    finally:
        if server:
            stop_group(server)
        stop_group(chrome)
        server_log.close()
        chrome_log.close()


if __name__ == "__main__":
    main()
