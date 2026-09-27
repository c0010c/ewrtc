#!/usr/bin/env python3
"""Exercise NACK/RTX and PLI while applying loopback UDP loss locally."""
import argparse
import asyncio
import json
import os
import subprocess
import sys
import time
from pathlib import Path

from run_matrix import EXAMPLES, ROOT, evaluate, navigate, stop_group, wait_port


def qdisc(loss=None):
    original_namespace = os.environ.get("EWRTC_HOST_NETNS")
    if not original_namespace or os.readlink("/proc/self/ns/net") == original_namespace:
        raise RuntimeError("Use tests/run_isolated_network.sh; host loopback must not be modified")
    command = ([] if os.geteuid() == 0 else ["sudo", "-n"]) + ["tc", "qdisc"]
    if loss is None:
        command += ["del", "dev", "lo", "root"]
    else:
        command += ["replace", "dev", "lo", "root", "netem", "loss",
                    f"{loss}%"]
    subprocess.run(command, check=loss is not None,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def snapshot(port):
    asyncio.run(evaluate(port, "window.requestSdkStats()"))
    time.sleep(0.4)
    state = json.loads(asyncio.run(evaluate(port,
        "JSON.stringify(window.testState)")))
    frames = json.loads(asyncio.run(evaluate(port,
        "pc.getStats().then(s=>JSON.stringify([...s.values()].filter(x=>"
        "x.type==='inbound-rtp'&&x.kind==='video').map(x=>({"
        "framesDecoded:x.framesDecoded,keyFramesDecoded:x.keyFramesDecoded}))))")))
    stats = {}
    for item in state.get("stats", "").split():
        if "=" in item:
            key, value = item.split("=", 1)
            if value.isdecimal():
                stats[key] = int(value)
    return {"browser_state": state.get("state"), "sdk": stats,
            "video": frames[0] if frames else {}}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--video", type=Path, required=True)
    parser.add_argument("--duration", type=int, default=20)
    parser.add_argument("--port-offset", type=int, default=160)
    parser.add_argument("--output-dir", type=Path,
                        default=Path("/tmp/ewrtc-loss"))
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
    report = {"duration_seconds_each": args.duration, "cases": []}
    try:
        wait_port(cdp_port)
        server = subprocess.Popen([
            sys.executable, str(EXAMPLES / "signaling.py"),
            "--ice", "juice", "--dtls", "openssl",
            "--demo", str(ROOT / "build-juice-openssl/ewrtc_demo"),
            "--video", str(args.video.resolve()),
            "--turn", "127.0.0.1", "--turn-port", "3479",
            "--turn-user", "test", "--turn-pass", "testpass", "--relay-only",
            "--http-port", str(http_port), "--ws-port", str(ws_port),
            "--output-dir", str(args.output_dir)],
            cwd=ROOT, stdout=server_log, stderr=subprocess.STDOUT,
            start_new_session=True)
        wait_port(http_port)
        asyncio.run(navigate(
            f"http://127.0.0.1:{http_port}/browser.html?ws={ws_port}"
            "&turn=127.0.0.1:3479&user=test&pass=testpass&relay=1",
            cdp_port))
        checked = subprocess.run([sys.executable,
            str(EXAMPLES / "check_chrome.py"), "--port", str(cdp_port),
            "--timeout", "20"], text=True, capture_output=True, timeout=30)
        if checked.returncode:
            raise RuntimeError(checked.stderr or checked.stdout)
        for loss in (1, 5):
            before = snapshot(cdp_port)
            try:
                qdisc(loss)
                time.sleep(args.duration)
            finally:
                qdisc()
            time.sleep(1)
            after = snapshot(cdp_port)
            report["cases"].append({"loss_percent": loss,
                                    "before": before, "after": after})
            print(f"{loss}% loss: NACK {before['sdk'].get('nack')} -> "
                  f"{after['sdk'].get('nack')}, RTX "
                  f"{before['sdk'].get('rtx')} -> {after['sdk'].get('rtx')}",
                  flush=True)
        before = snapshot(cdp_port)
        asyncio.run(evaluate(cdp_port, "window.requestKeyframe()"))
        time.sleep(2)
        after = snapshot(cdp_port)
        report["keyframe_request"] = {"before": before, "after": after}
        report["passed"] = all(
            case["after"]["sdk"].get("nack", 0) >
            case["before"]["sdk"].get("nack", 0) and
            case["after"]["sdk"].get("rtx", 0) >
            case["before"]["sdk"].get("rtx", 0) and
            case["after"]["video"].get("framesDecoded", 0) >
            case["before"]["video"].get("framesDecoded", 0)
            for case in report["cases"]) and (
            after["sdk"].get("pli", 0) > before["sdk"].get("pli", 0) and
            after["video"].get("keyFramesDecoded", 0) >
            before["video"].get("keyFramesDecoded", 0))
        (args.output_dir / "results.json").write_text(
            json.dumps(report, indent=2) + "\n")
        print(f"loss/PLI: {'PASS' if report['passed'] else 'FAIL'}", flush=True)
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
