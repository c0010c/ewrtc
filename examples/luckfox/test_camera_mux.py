#!/usr/bin/env python3
"""Live-board integration test using an already running, dedicated headless Chrome.

Run with a fresh gateway (no other viewers) and Chrome debugging on port 9239.
"""
import asyncio
import json
import urllib.request
from pathlib import Path
from websockets.asyncio.client import connect

CDP = "http://127.0.0.1:9239"
PAGE = "http://127.0.0.1:8081/player/?ws=8766&source=camera"
SOCKET = "ws://127.0.0.1:8766"
INSTRUMENT = """
window.testPCs = []; window.testSockets = []; window.backendStats = null;
const PC = window.RTCPeerConnection, WS = window.WebSocket;
window.RTCPeerConnection = class extends PC { constructor(...a) { super(...a); testPCs.push(this); } };
window.WebSocket = class extends WS { constructor(...a) { super(...a); testSockets.push(this);
this.addEventListener('message', e => { const m = JSON.parse(e.data); if(m.type === 'stats') backendStats = JSON.parse(m.value); }); } };
"""


async def cdp(url, method, params=None):
    async with connect(url, origin="http://localhost") as ws:
        await ws.send(json.dumps({"id": 1, "method": method, "params": params or {}}))
        while True:
            answer = json.loads(await ws.recv())
            if answer.get("id") == 1:
                if "error" in answer:
                    raise RuntimeError(answer)
                return answer["result"]


async def evaluate(page, script):
    result = await cdp(page["webSocketDebuggerUrl"], "Runtime.evaluate",
                       {"expression": script, "awaitPromise": True, "returnByValue": True})
    if "exceptionDetails" in result:
        raise RuntimeError(result)
    return result.get("result", {}).get("value")


async def new_page():
    request = urllib.request.Request(CDP + "/json/new?about:blank", method="PUT")
    page = json.load(urllib.request.urlopen(request))
    # The injection belongs to the CDP connection; keep it attached through load.
    async with connect(page["webSocketDebuggerUrl"], origin="http://localhost") as ws:
        for identity, method, params in (
            (0, "Page.enable", {}),
            (1, "Page.addScriptToEvaluateOnNewDocument", {"source": INSTRUMENT}),
            (2, "Page.navigate", {"url": PAGE}),
        ):
            await ws.send(json.dumps({"id": identity, "method": method, "params": params}))
            while json.loads(await ws.recv()).get("id") != identity:
                pass
        await asyncio.sleep(2)
    return page


async def close_page(page):
    urllib.request.urlopen(CDP + "/json/close/" + page["id"]).read()


async def sample(page):
    return await evaluate(page, """(async () => {
      backendStats = null;
      testSockets.at(-1).send(JSON.stringify({type:'stats'}));
      for(let i=0;i<100 && !backendStats;i++) await new Promise(r=>setTimeout(r,50));
      const pc=testPCs.at(-1), reports=await pc.getStats();
      const v=[...reports.values()].find(s=>s.type==='inbound-rtp' && s.kind==='video');
      return {backend:backendStats,state:pc.connectionState,video:v && {
        frames:v.framesDecoded,fps:v.framesPerSecond,width:v.frameWidth,height:v.frameHeight,bytes:v.bytesReceived}};
    })()""")


async def socket_stats(ws):
    await ws.send(json.dumps({"type": "stats"}))
    async with asyncio.timeout(5):
        while True:
            record = json.loads(await ws.recv())
            if record["type"] == "stats":
                return json.loads(record["value"])


async def main():
    pages, results = [], {}
    try:
        pages = [await new_page(), await new_page()]
        await asyncio.sleep(8)
        first = [await sample(page) for page in pages]
        print("two viewers", first, flush=True)
        pid = first[0]["backend"]["pid"]
        for s in first:
            assert s["state"] == "connected" and s["video"]["frames"] > 50, s
            assert s["backend"]["pid"] == pid and s["backend"]["sessions"] == 2, s
            assert s["backend"]["workers"] == 1 and s["backend"]["sdk_threads"] == 2, s
        await asyncio.sleep(12)
        results["two_viewers"] = [await sample(page) for page in pages]
        for old, new in zip(first, results["two_viewers"]):
            assert new["video"]["frames"] - old["video"]["frames"] > 200, new
        # Exercise admission and release while both video streams keep running.
        async with connect(SOCKET) as third, connect(SOCKET) as fourth:
            assert (await socket_stats(fourth))["sessions"] == 4
            async with connect(SOCKET) as fifth:
                assert json.loads(await fifth.recv())["type"] == "error"
        await asyncio.sleep(1)
        for _ in range(20):
            async with connect(SOCKET) as probe:
                stats = await socket_stats(probe)
                assert stats["pid"] == pid and stats["sessions"] == 3, stats
            await asyncio.sleep(.05)
        await close_page(pages.pop(0))
        await asyncio.sleep(5)
        remaining = await sample(pages[0])
        assert remaining["backend"]["sessions"] == 1 and remaining["backend"]["pid"] == pid, remaining
        assert remaining["video"]["fps"] >= 20, remaining
        results["after_close"] = remaining
        pages.append(await new_page())
        await asyncio.sleep(6)
        results["reconnected"] = [await sample(page) for page in pages]
        for s in results["reconnected"]:
            assert s["backend"]["pid"] == pid and s["backend"]["sessions"] == 2, s
            assert s["video"]["frames"] > 50 and s["video"]["fps"] >= 20, s
        print("close/reconnect passed", results["reconnected"], flush=True)
    finally:
        for page in pages:
            await close_page(page)
    await asyncio.sleep(2)
    async with connect(SOCKET) as probe:
        stats = await socket_stats(probe)
        assert stats["sessions"] == 1 and stats["pid"] == pid, stats
        results["after_all_closed_and_reopened"] = stats
    Path("build-luckfox/camera-mux-validation.json").write_text(json.dumps(results, indent=2))
    print("PASS: shared process, dual playback, admission limit, 20 churn cycles, close/reconnect isolation")


if __name__ == "__main__":
    asyncio.run(main())
