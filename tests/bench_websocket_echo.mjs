// WebSocket round-trip throughput: echo server + N clients in one process.
// Usage: ant bench.mjs <label>
const label = process.argv[2] || 'run';
const SOCKETS = 8;
const INFLIGHT = 16;          // messages in flight per socket
const DURATION_MS = 2500;

const server = Ant.serve({
  hostname: '127.0.0.1',
  port: 0,
  fetch(request, ctx) {
    const { socket, response } = ctx.upgradeWebSocket(request);
    socket.onmessage = e => socket.send(e.data);
    return response;
  }
});

function run(name, payload) {
  return new Promise(resolve => {
    let received = 0;
    let bytes = 0;
    let opened = 0;
    let running = true;
    const sockets = [];
    const start = () => {
      const t0 = performance.now();
      for (const ws of sockets) for (let i = 0; i < INFLIGHT; i++) ws.send(payload);
      setTimeout(() => {
        running = false;
        const seconds = (performance.now() - t0) / 1000;
        for (const ws of sockets) ws.close();
        resolve({ name, msgsPerSec: Math.round(received / seconds), mbPerSec: +(bytes / seconds / 1048576).toFixed(1) });
      }, DURATION_MS);
    };
    for (let i = 0; i < SOCKETS; i++) {
      const ws = new WebSocket(`ws://127.0.0.1:${server.port}/`);
      ws.binaryType = 'arraybuffer';
      sockets.push(ws);
      ws.onopen = () => { if (++opened === SOCKETS) start(); };
      ws.onmessage = e => {
        received++;
        bytes += typeof e.data === 'string' ? e.data.length : e.data.byteLength;
        if (running) ws.send(payload);
      };
    }
  });
}

const text64 = 'x'.repeat(64);
const text4k = 'y'.repeat(4096);
const bin4k = new Uint8Array(4096);
const bin64k = new Uint8Array(65536);

const results = [];
results.push(await run('text-64B', text64));
results.push(await run('text-4KB', text4k));
results.push(await run('bin-4KB', bin4k));
results.push(await run('bin-64KB', bin64k));
console.log(JSON.stringify({ label, results }));
await server.stop();
