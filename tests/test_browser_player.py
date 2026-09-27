#!/usr/bin/env python3
"""Verify autoplay, looping and reconnect against a running player server."""
import argparse
import asyncio
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "examples"))
from run_matrix import evaluate, navigate, stop_group, wait_port

SNAPSHOT = """(() => {
  const v = document.querySelector('#video');
  return {status: document.querySelector('#status').textContent,
    error: document.querySelector('#error').hidden ? '' : document.querySelector('#error').textContent,
    width: v.videoWidth, height: v.videoHeight, paused: v.paused, muted: v.muted,
    frames: v.getVideoPlaybackQuality().totalVideoFrames,
    stats: document.querySelector('#stats').textContent,
    timings: Array.from(document.querySelectorAll('#timings tr'), row => ({
      stage: row.dataset.stage, status: row.dataset.status,
      duration: parseFloat(row.cells[1].textContent),
      elapsed: parseFloat(row.cells[2].textContent)}))};
})()"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default="http://127.0.0.1:8080/player/?ws=8765")
    parser.add_argument("--cdp-port", type=int, default=19324)
    args = parser.parse_args()
    port = args.cdp_port

    def read(expression):
        return asyncio.run(evaluate(port, expression))

    def await_video():
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            state = read(SNAPSHOT)
            if state:
                assert not state["error"], state
                if state["width"] == 1280 and state["height"] == 720 and state["frames"] >= 30:
                    assert not state["paused"] and state["muted"], state
                    return state
            time.sleep(0.25)
        raise AssertionError(f"No autoplay video: {state}")

    def check_timings(state):
        timings = {row['stage']: row for row in state['timings']}
        assert len(timings) == 11, state
        for row in timings.values():
            assert row['status'] == 'complete', row
            assert row['duration'] is not None and row['duration'] >= 0, row
            assert row['elapsed'] is not None and row['elapsed'] >= row['duration'], row
        assert timings['total']['elapsed'] == timings['total']['duration'], timings
        assert abs(timings['connection']['duration'] + timings['render']['duration']
                   - timings['total']['duration']) <= 0.2, timings

    with tempfile.TemporaryDirectory(prefix="ewrtc-player-test-") as temp:
        with open(Path(temp) / "chrome.log", "w") as log:
            # Fresh profile and normal autoplay policy: no user-gesture override.
            chrome = subprocess.Popen([
                "google-chrome", "--headless=new", "--no-sandbox", "--disable-gpu",
                "--remote-allow-origins=http://localhost",
                f"--remote-debugging-port={port}", f"--user-data-dir={temp}/profile",
                "about:blank"], stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            try:
                wait_port(port)
                asyncio.run(navigate(args.url, port))
                initial = await_video()
                check_timings(initial)
                time.sleep(12)  # Cross the ten-second source loop boundary.
                looped = read(SNAPSHOT)
                assert not looped["error"] and not looped["paused"], looped
                assert looped["frames"] > initial["frames"] + 250, looped
                assert looped['timings'] == initial['timings'], looped
                read("document.querySelector('#stop').click()")
                assert read("document.querySelector('#video').srcObject === null")
                reset = read("""(() => {
                  document.querySelector('#reconnect').click();
                  return Array.from(document.querySelectorAll('#timings tr'), row => row.dataset.status);
                })()""")
                assert reset and all(status != 'complete' for status in reset), reset
                reconnected = await_video()
                check_timings(reconnected)
                print(json.dumps({"autoplay": initial, "looped": looped,
                                  "reconnected": reconnected}, ensure_ascii=False, indent=2))
            finally:
                stop_group(chrome)


if __name__ == "__main__":
    main()
