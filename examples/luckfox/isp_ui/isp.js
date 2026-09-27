import {StreamClient} from '/client.js';

const definitions = [
  ['brightness', '亮度', '整体明暗，默认 50'],
  ['contrast', '对比度', '明暗差异，默认 50'],
  ['saturation', '饱和度', '颜色浓淡，默认 50'],
  ['sharpness', '锐度', '边缘细节，默认 50'],
];
const controls = new Map();
const status = document.querySelector('#apply-status');
const reset = document.querySelector('#reset');
const read = document.querySelector('#read');
let pending = {}, busy = false, timer, ready = false;

function report(message, error = false) {
  status.textContent = message;
  status.dataset.error = String(error);
}
function enable(value) {
  ready = value;
  for (const {range, number} of controls.values()) range.disabled = number.disabled = !value;
  reset.disabled = !value;
}
function display(key, value) {
  const {range, number} = controls.get(key);
  range.value = number.value = value;
}
for (const [key, label, hint] of definitions) {
  const row = document.createElement('div');
  row.className = 'control';
  row.innerHTML = `<div class="control-header"><label for="${key}">${label}</label><input id="${key}-number" type="number" min="0" max="100" step="1" aria-label="${label}数值" disabled></div><p>${hint}</p><input id="${key}" type="range" min="0" max="100" step="1" aria-label="${label}" disabled>`;
  document.querySelector('#controls').append(row);
  const range = row.querySelector('[type=range]'), number = row.querySelector('[type=number]');
  controls.set(key, {range, number});
  range.oninput = () => queue(key, Number(range.value));
  number.oninput = () => {
    if (number.value !== '' && number.checkValidity()) queue(key, Number(number.value));
  };
  number.onchange = () => {
    if (number.value === '' || !number.checkValidity()) {
      number.value = range.value;
      report('请输入 0–100 的整数。', true);
      return;
    }
    queue(key, Number(number.value));
  };
}
function queue(key, value) {
  if (!ready) return;
  display(key, value);
  pending[key] = value;
  report('正在应用…');
  // Throttle while dragging, coalesce intermediate positions and serialize writes.
  if (!timer && !busy) timer = setTimeout(() => {timer = null; flush();}, 100);
}
async function request(values) {
  const response = await fetch('/api/isp', values === undefined
    ? {signal: AbortSignal.timeout(12000)}
    : {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(values), signal: AbortSignal.timeout(12000)});
  const data = await response.json();
  if (!response.ok) throw new Error(data.error || 'ISP 操作失败');
  return data.values;
}
async function flush() {
  if (busy || !Object.keys(pending).length) return;
  busy = true; read.disabled = reset.disabled = true;
  const values = pending; pending = {};
  try {
    const current = await request(values);
    for (const [key, value] of Object.entries(current)) if (!(key in pending)) display(key, value);
    report(Object.keys(pending).length ? '正在应用…' : `已生效 · ${new Date().toLocaleTimeString('zh-CN')}`);
  } catch (error) {
    pending = {};
    enable(false);
    report(`${error.message} 请点“重新读取”确认当前值。`, true);
  } finally {
    busy = false; read.disabled = false; reset.disabled = !ready;
    if (Object.keys(pending).length) timer = setTimeout(() => {timer = null; flush();}, 100);
  }
}
async function refresh() {
  if (busy) return;
  clearTimeout(timer); timer = null; pending = {};
  busy = true; enable(false); read.disabled = true;
  report('正在读取板子参数…');
  try {
    const values = await request();
    for (const [key, value] of Object.entries(values)) display(key, value);
    enable(true); report('已读取 · 拖动滑块即可实时应用');
  } catch (error) {report(error.message, true);}
  finally {busy = false; read.disabled = false;}
}
read.onclick = refresh;
reset.onclick = () => {for (const [key] of definitions) queue(key, 50);};
refresh();

const video = document.querySelector('#video');
const videoError = document.querySelector('#video-error');
const states = {connecting:'正在连接画面',connected:'实时画面已连接',disconnected:'画面连接中断',failed:'画面连接失败',closed:'画面已断开'};
const client = new StreamClient(video, state => {
  document.querySelector('#stream-state').textContent = states[state] || state;
}, message => {videoError.hidden = false; videoError.textContent = message;});
let previous;
function connect() {
  videoError.hidden = true; previous = null;
  const ws = new URL(location.href);
  ws.protocol = 'ws:'; ws.port = new URLSearchParams(location.search).get('ws') || '8766';
  ws.pathname = '/'; ws.search = ws.hash = '';
  try {client.connect(ws.href);} catch (error) {videoError.hidden = false; videoError.textContent = error.message;}
}
document.querySelector('#reconnect').onclick = connect;
connect();
const statsTimer = setInterval(async () => {
  try {
    const report = await client.stats();
    if (!report) return;
    const bitrate = previous && report.timestamp > previous.timestamp
      ? Math.round(8 * (report.bytesReceived - previous.bytesReceived) / (report.timestamp - previous.timestamp)) : 0;
    document.querySelector('#stats').textContent = `${video.videoWidth} × ${video.videoHeight} · ${Math.round(report.framesPerSecond || 0)} fps · ${bitrate} kbps`;
    previous = report;
  } catch { /* Reconnection can invalidate a pending stats request. */ }
}, 1000);
window.addEventListener('pagehide', () => {clearTimeout(timer); clearInterval(statsTimer); client.close();});
