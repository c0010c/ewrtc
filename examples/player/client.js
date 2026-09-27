// Transport and session lifecycle are independent of the page UI.
export class StreamClient {
  constructor(video, onState, onError, onTiming = () => {}) {
    this.video = video;
    this.onState = onState;
    this.onError = onError;
    this.onTiming = onTiming;
  }

  connect(url) {
    this.close();
    this.onTiming('start', performance.now());
    const pc = this.pc = new RTCPeerConnection({iceServers: []});
    const mark = name => {if (this.pc === pc) this.onTiming(name, performance.now());};
    mark('socketStart');
    const ws = this.ws = new WebSocket(url);
    const stream = new MediaStream();
    this.video.srcObject = stream;
    this.frameCallback = this.video.requestVideoFrameCallback(() => {
      this.frameCallback = null;
      mark('firstFrame');
    });
    this.onState('connecting');
    const fail = error => {
      if (this.pc !== pc) return;
      this.close();
      this.onState('failed');
      this.onError(String(error.message || error));
    };
    pc.onicegatheringstatechange = () => {
      if (pc.iceGatheringState === 'gathering') mark('gatheringStart');
      if (pc.iceGatheringState === 'complete') mark('gatheringDone');
    };
    pc.oniceconnectionstatechange = () => {
      if (pc.iceConnectionState === 'checking') mark('iceStart');
      if (['connected', 'completed'].includes(pc.iceConnectionState)) mark('iceConnected');
    };
    let observedTransport;
    const observeDtls = () => {
      const transceiver = pc.getTransceivers()[0];
      const transport = transceiver?.receiver.transport || transceiver?.sender.transport;
      if (!transport || transport === observedTransport || this.pc !== pc) return;
      observedTransport = transport;
      const report = () => {
        if (transport.state === 'connecting') mark('dtlsStart');
        if (transport.state === 'connected') mark('dtlsConnected');
      };
      transport.addEventListener('statechange', report);
      report();
    };
    pc.onconnectionstatechange = () => {
      if (this.pc !== pc) return;
      if (pc.connectionState === 'connected') mark('connected');
      this.onState(pc.connectionState);
      if (pc.connectionState === 'failed') fail(new Error('WebRTC 连接失败，请重新连接。'));
    };
    pc.ontrack = ({track}) => stream.addTrack(track);
    pc.onicecandidate = ({candidate}) => {
      if (ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(candidate
        ? {type: 'candidate', value: candidate.candidate} : {type: 'end'}));
    };
    pc.addTransceiver('video', {direction: 'recvonly'});
    pc.addTransceiver('audio', {direction: 'recvonly'});
    // Apply SDP and trickled candidates serially, including asynchronous operations.
    let incoming = Promise.resolve();
    ws.onmessage = ({data}) => {
      incoming = incoming.then(async () => {
        if (this.pc !== pc) return;
        const message = JSON.parse(data);
        if (message.type === 'answer') {
          mark('answerReceived');
          await pc.setRemoteDescription({type: 'answer', sdp: message.value});
          mark('answerApplied');
          observeDtls();
        } else if (message.type === 'candidate') {
          await pc.addIceCandidate({candidate: message.value, sdpMid: pc.getTransceivers()[0].mid});
        } else if (message.type === 'done') {
          await pc.addIceCandidate(null);
        } else if (message.type === 'error') {
          throw new Error(message.value);
        }
      }).catch(fail);
    };
    ws.onclose = () => fail(new Error('信令连接已关闭，请确认示例服务正在运行。'));
    ws.onerror = () => fail(new Error('无法连接信令服务。'));
    ws.onopen = async () => {
      try {
        if (this.pc !== pc) return;
        mark('socketOpen');
        mark('offerStart');
        const offer = await pc.createOffer();
        if (this.pc !== pc) return;
        mark('offerCreated');
        await pc.setLocalDescription(offer);
        mark('localSet');
        observeDtls();
        if (this.pc === pc && ws.readyState === WebSocket.OPEN) {
          ws.send(JSON.stringify({type: 'offer', value: offer.sdp}));
          mark('offerSent');
        }
      } catch (error) { fail(error); }
    };
    this.timeout = setTimeout(() => {
      if (pc.connectionState !== 'connected') fail(new Error('连接超时，请重新连接。'));
    }, 15000);
  }

  async stats() {
    const pc = this.pc;
    if (!pc) return null;
    const reports = await pc.getStats();
    if (this.pc !== pc) return null;
    return [...reports.values()].find(report => report.type === 'inbound-rtp' && report.kind === 'video');
  }

  close() {
    clearTimeout(this.timeout);
    if (this.frameCallback != null) {
      this.video.cancelVideoFrameCallback(this.frameCallback);
      this.frameCallback = null;
    }
    const pc = this.pc, ws = this.ws;
    this.pc = this.ws = null;
    ws?.close();
    pc?.close();
    this.video.srcObject = null;
    this.onState('closed');
  }
}
