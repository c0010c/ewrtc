#!/usr/bin/env python3
"""Optional real-Chrome offer/answer and H264 receive interoperability matrix.
Requires google-chrome, ffmpeg/ffprobe and Python websockets/psutil.
"""
import argparse
import asyncio
import json
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'examples'))
from run_matrix import evaluate, navigate, stop_group, wait_port


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--demo', type=Path, default=ROOT / 'cmake-build-debug/ewrtc_demo')
    parser.add_argument('--video', type=Path, required=True)
    parser.add_argument('--output', type=Path, default=Path('/tmp/ewrtc-duplex'))
    parser.add_argument('--only', help='e.g. native-openssl-sdk')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    cdp, http, ws = 19323, 18081, 18766
    results = []
    with (args.output / 'chrome.log').open('w') as log:
        chrome = subprocess.Popen(['google-chrome','--headless=new','--no-sandbox','--disable-gpu',
            '--autoplay-policy=no-user-gesture-required','--remote-allow-origins=http://localhost',
            f'--remote-debugging-port={cdp}',f'--user-data-dir={args.output / "chrome-profile"}',
            'about:blank'], stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            wait_port(cdp)
            for ice in ('native','juice'):
                for tls in ('openssl','mbedtls'):
                    for offerer in ('sdk','browser'):
                        name = f'{ice}-{tls}-{offerer}'
                        if args.only and args.only != name: continue
                        out = args.output / name
                        out.mkdir(exist_ok=True)
                        with (out / 'signaling.log').open('w') as slog:
                            server = subprocess.Popen([sys.executable,str(ROOT / 'examples/signaling.py'),
                                '--demo',str(args.demo.resolve()),'--video',str(args.video.resolve()),
                                '--ice',ice,'--dtls',tls,'--http-port',str(http),'--ws-port',str(ws),
                                '--output-dir',str(out)], stdout=slog,stderr=subprocess.STDOUT,start_new_session=True)
                            try:
                                wait_port(http)
                                asyncio.run(navigate(f'http://127.0.0.1:{http}/duplex.html?ws={ws}&offerer={offerer}',cdp))
                                time.sleep(0.5)
                                asyncio.run(evaluate(cdp,"document.querySelector('#start').click()"))
                                state = {}
                                for _ in range(100):
                                    state = asyncio.run(evaluate(cdp,'window.testState')) or {}
                                    if state.get('errors') or state.get('state') == 'connected': break
                                    time.sleep(0.1)
                                assert state.get('state') == 'connected' and not state['errors'], state
                                time.sleep(3)
                                asyncio.run(evaluate(cdp,'window.readStats()'))
                                time.sleep(0.2)
                                state = asyncio.run(evaluate(cdp,'window.testState'))
                                assert not state['errors'], state
                                assert any(x.get('framesDecoded',0)>0 for x in state['inbound']), state
                                stats = dict(x.split('=',1) for x in state['sdkStats'].split() if '=' in x)
                                assert int(stats['recv_frames']) > 0 and int(stats['recv_audio']) > 0, state
                                asyncio.run(evaluate(cdp,"document.querySelector('#stop').click()"))
                                time.sleep(0.4)
                                probe = subprocess.run(['ffprobe','-v','error','-count_frames','-select_streams','v:0',
                                    '-show_entries','stream=width,height,nb_read_frames','-of','json',str(out/'received.h264')],
                                    text=True,capture_output=True,check=True)
                                decoded = json.loads(probe.stdout)['streams'][0]
                                assert decoded['width'] == 320 and decoded['height'] == 180 and int(decoded['nb_read_frames']) > 0, decoded
                                subprocess.run(['ffmpeg','-v','error','-xerror','-i',str(out/'received.h264'),'-f','null','-'],
                                    check=True,timeout=15)
                                results.append({'case':name,'passed':True,'state':state,'decoded':decoded})
                                print(name,'PASS',flush=True)
                            except Exception as e:
                                results.append({'case':name,'passed':False,'error':str(e)})
                                print(name,'FAIL',e,flush=True)
                            finally:
                                asyncio.run(navigate('about:blank',cdp)); stop_group(server)
        finally:
            stop_group(chrome)
    (args.output/'results.json').write_text(json.dumps(results,indent=2))
    assert results and all(x['passed'] for x in results)

if __name__ == '__main__': main()
