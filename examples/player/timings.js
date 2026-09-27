// All timestamps use the browser's monotonic performance clock.
const stages = [
  ['signaling', 'WebSocket 信令连接', 'socketStart', 'socketOpen'],
  ['offer', '创建 Offer', 'offerStart', 'offerCreated'],
  ['local', '设置本地 SDP', 'offerCreated', 'localSet'],
  ['answer', '等待 Answer（含服务端处理与传输）', 'offerSent', 'answerReceived'],
  ['remote', '应用远端 SDP', 'answerReceived', 'answerApplied'],
  ['gathering', '本地 ICE 候选收集', 'gatheringStart', 'gatheringDone'],
  ['ice', 'ICE 连通性检查', 'iceStart', 'iceConnected'],
  ['dtls', 'DTLS 握手', 'dtlsStart', 'dtlsConnected'],
  ['connection', 'WebRTC 建连总计', 'start', 'connected'],
  ['render', '建连后等待首帧呈现', 'connected', 'firstFrame'],
  ['total', '开始连接 → 首帧总计', 'start', 'firstFrame'],
];

export class ConnectionTimings {
  constructor(body) {
    this.body = body;
    this.marks = new Map();
    this.stoppedAt = null;
  }

  record(name, at = performance.now()) {
    if (name === 'start') {
      this.marks.clear();
      this.stoppedAt = null;
    }
    if (this.stoppedAt !== null || this.marks.has(name)) return;
    this.marks.set(name, at);
    this.render();
  }

  stop() {
    this.stoppedAt = performance.now();
    this.render();
  }

  render() {
    const now = this.stoppedAt ?? performance.now();
    const origin = this.marks.get('start');
    const rows = stages.map(([id, label, from, to]) => {
      const start = this.marks.get(from), end = this.marks.get(to);
      // A browser may expose an already-connected transport without a start event.
      const complete = start !== undefined && end !== undefined && end >= start;
      const running = start !== undefined && end === undefined;
      const duration = complete ? end - start : running ? now - start : null;
      const status = complete ? '已完成' : this.stoppedAt !== null ? '未完成'
        : running ? '进行中' : end !== undefined ? '未观测到起点' : '等待中';
      const row = document.createElement('tr');
      row.dataset.stage = id;
      row.dataset.status = complete ? 'complete' : running && this.stoppedAt === null ? 'running' : 'pending';
      for (const text of [label, duration === null ? '—' : `${duration.toFixed(1)} ms`,
        end !== undefined && origin !== undefined ? `${(end - origin).toFixed(1)} ms` : '—', status]) {
        const cell = document.createElement('td');
        cell.textContent = text;
        row.append(cell);
      }
      return row;
    });
    this.body.replaceChildren(...rows);
  }
}
