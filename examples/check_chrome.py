#!/usr/bin/env python3
"""Drive the local browser.html page through a Chrome debugging port."""
import argparse
import asyncio
import json
import time
import urllib.request

from websockets.asyncio.client import connect


async def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=9222)
    parser.add_argument("--timeout", type=int, default=30)
    args = parser.parse_args()
    pages = json.load(urllib.request.urlopen(f"http://127.0.0.1:{args.port}/json"))
    page = next(p for p in pages if p["type"] == "page" and "browser.html" in p["url"])
    async with connect(page["webSocketDebuggerUrl"], origin="http://localhost") as ws:
        next_id = 1

        async def evaluate(expression):
            nonlocal next_id
            ident = next_id
            next_id += 1
            await ws.send(json.dumps({"id": ident, "method": "Runtime.evaluate",
                "params": {"expression": expression, "returnByValue": True,
                           "awaitPromise": True}}))
            while True:
                result = json.loads(await ws.recv())
                if result.get("id") == ident:
                    if "exceptionDetails" in result.get("result", {}):
                        raise RuntimeError(result["result"]["exceptionDetails"])
                    return result.get("result", {}).get("result", {}).get("value")

        await evaluate("document.querySelector('#start').click()")
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            state = await evaluate("JSON.stringify(window.testState)")
            parsed = json.loads(state)
            if parsed.get("state") == "connected" or parsed.get("error"):
                break
            await asyncio.sleep(1)
        await evaluate("window.requestSdkStats()")
        await asyncio.sleep(2)
        state = json.loads(await evaluate("JSON.stringify(window.testState)"))
        dimensions = json.loads(await evaluate("JSON.stringify({videoWidth:document.querySelector('#video').videoWidth,videoHeight:document.querySelector('#video').videoHeight})"))
        log = await evaluate("document.querySelector('#log').textContent")
        print(json.dumps({"state": state, "dimensions": dimensions, "log": log}))
        if state.get("state") != "connected" or state.get("sdkState") != 3 or state.get("error"):
            raise SystemExit(1)
        if dimensions["videoWidth"] < 1 or dimensions["videoHeight"] < 1:
            raise SystemExit(1)
        for key in ("sent_video", "sent_audio", "recv_audio"):
            value = state.get("stats", "").split(key + "=")[-1].split(" ")[0]
            if not value.isdigit() or int(value) < 1:
                raise SystemExit(1)
        if state.get("remoteAudioRms", 0) < 0.01 or not 500 <= state.get("remoteAudioHz", 0) <= 700:
            raise SystemExit(1)


if __name__ == "__main__":
    asyncio.run(main())
