import {StreamClient} from './client.js';
import {ConnectionTimings} from './timings.js';

const video = document.querySelector('#video');
const cameraMode = new URLSearchParams(location.search).get('source') === 'camera';
if (cameraMode) {
  document.title = 'Luckfox · 板载摄像头';
  document.querySelector('#stream-title').textContent = '板载摄像头实时画面';
  document.querySelector('#stream-description').textContent = 'Luckfox Pico Plus · SC3336 摄像头 · WebRTC 实时传输';
  document.querySelector('#stream-hint').textContent = '720p · 25 fps · 实时采集。当前为视频流，无需开启电脑的摄像头或麦克风权限。';
  document.querySelector('#sound').hidden = true;
}
const error = document.querySelector('#error');
const stats = document.querySelector('#stats');
const timings = new ConnectionTimings(document.querySelector('#timings'));
const states = {connecting: '正在连接…', connected: '已连接 · 实时传输', disconnected: '连接中断', failed: '连接失败', closed: '已断开'};
const showError = message => {error.textContent = message; error.hidden = false;};
const client = new StreamClient(video, state => {
  document.querySelector('#status').textContent = states[state] || state;
  if (state === 'closed' || state === 'failed') timings.stop();
}, showError, (name, at) => timings.record(name, at));
let previous;
function start() {
  error.hidden = true;
  previous = null;
  stats.textContent = '等待视频数据…';
  const url = new URL(location.href);
  url.protocol = location.protocol === 'https:' ? 'wss:' : 'ws:';
  url.port = new URLSearchParams(location.search).get('ws') || '8765';
  url.pathname = '/';
  url.search = url.hash = '';
  try {client.connect(url.href);} catch (error) {client.close(); showError(String(error));}
}
document.querySelector('#reconnect').onclick = start;
document.querySelector('#stop').onclick = () => {client.close(); stats.textContent = '已停止接收';};
const sound = document.querySelector('#sound');
sound.onclick = () => {video.muted = !video.muted; video.play().catch(() => {});};
video.onvolumechange = () => {sound.textContent = video.muted ? '开启声音' : '静音';};
const timer = setInterval(async () => {
  try {
    const report = await client.stats();
    if (!report) return;
    const kbps = previous && report.timestamp > previous.timestamp
      ? Math.round(8 * (report.bytesReceived - previous.bytesReceived) / (report.timestamp - previous.timestamp)) : 0;
    stats.textContent = `${video.videoWidth} × ${video.videoHeight} · ${Math.round(report.framesPerSecond || 0)} fps · ${kbps} kbps · 已解码 ${report.framesDecoded || 0} 帧`;
    previous = report;
  } catch { /* A session may close while getStats is pending. */ }
}, 1000);
const timingTimer = setInterval(() => timings.render(), 100);
window.addEventListener('pagehide', () => {
  clearInterval(timer);
  clearInterval(timingTimer);
  client.close();
});
start();
