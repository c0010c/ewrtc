let transformer;
let pending = 0;

async function request() {
  if (!transformer) { pending++; return; }
  try {
    await transformer.sendKeyFrameRequest();
    self.postMessage({type: 'keyframe-requested'});
  } catch (error) {
    self.postMessage({type: 'keyframe-error', error: String(error)});
  }
}

self.onrtctransform = event => {
  transformer = event.transformer;
  transformer.readable.pipeTo(transformer.writable).catch(error =>
    self.postMessage({type: 'pipe-error', error: String(error)}));
  while (pending-- > 0) request();
  pending = 0;
};

self.onmessage = event => {
  if (event.data === 'request') request();
};
