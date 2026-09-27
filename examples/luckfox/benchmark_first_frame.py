#!/usr/bin/env python3
"""Measure actual Chrome first-frame presentation, using a dedicated CDP on 9239."""
import argparse
import asyncio
import json
import random
import statistics
from pathlib import Path
from test_camera_mux import new_page, close_page, evaluate


async def main(args):
    page = await new_page()
    samples = []
    try:
        for index in range(args.samples):
            await evaluate(page, "document.querySelector('#reconnect').click()")
            for _ in range(150):
                sample = await evaluate(page, """(() => {
                  const total=document.querySelector('[data-stage=total]');
                  if(total?.dataset.status !== 'complete') return null;
                  return Object.fromEntries([...document.querySelectorAll('#timings tr')].map(
                    r=>[r.dataset.stage,parseFloat(r.children[1].textContent)]));
                })()""")
                if sample:
                    break
                await asyncio.sleep(.1)
            assert sample, "No first frame within 15 seconds"
            samples.append(sample)
            print(index + 1, sample, flush=True)
            await asyncio.sleep(random.Random(index).uniform(.1, .9))
        summary = {stage: {"median_ms": round(statistics.median(s[stage] for s in samples), 1),
                           "max_ms": max(s[stage] for s in samples),
                           "min_ms": min(s[stage] for s in samples)}
                   for stage in ("connection", "render", "total")}
        args.output.write_text(json.dumps({"summary": summary, "samples": samples}, indent=2))
        print(json.dumps(summary, indent=2))
    finally:
        await close_page(page)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--samples", type=int, default=12)
    parser.add_argument("--output", type=Path, required=True)
    asyncio.run(main(parser.parse_args()))
