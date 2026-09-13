// Binary WebSocket frames must arrive as ArrayBuffer by default and as Blob
// when binaryType is "blob", on both server and client sockets.
const assert = require('node:assert');
const { spawn } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

function waitForLine(child) {
  return new Promise((resolve, reject) => {
    let stdout = '';
    let stderr = '';
    const timeout = setTimeout(() => {
      child.kill('SIGTERM');
      reject(new Error(`timed out waiting for server\nstdout:\n${stdout}\nstderr:\n${stderr}`));
    }, 3000);
    child.stdout.on('data', chunk => {
      stdout += String(chunk);
      const newline = stdout.indexOf('\n');
      if (newline === -1) return;
      clearTimeout(timeout);
      resolve(stdout.slice(0, newline));
    });
    child.stderr.on('data', chunk => { stderr += String(chunk); });
    child.on('exit', code => {
      clearTimeout(timeout);
      if (code !== null && stdout.indexOf('\n') === -1) {
        reject(new Error(`server exited early; code=${code}\nstderr:\n${stderr}`));
      }
    });
  });
}

function describe(data) {
  if (data instanceof Blob) return 'blob:' + data.size;
  if (data instanceof ArrayBuffer) return 'arraybuffer:' + data.byteLength;
  if (ArrayBuffer.isView(data)) return 'view:' + data.byteLength;
  return typeof data;
}

function openSocket(url, binaryType) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(url);
    if (binaryType) ws.binaryType = binaryType;
    const timer = setTimeout(() => reject(new Error('open timed out')), 3000);
    ws.onopen = () => { clearTimeout(timer); resolve(ws); };
    ws.onerror = () => { clearTimeout(timer); reject(new Error('socket error')); };
  });
}

function nextMessage(ws) {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error('message timed out')), 3000);
    ws.onmessage = event => { clearTimeout(timer); resolve(event.data); };
  });
}

async function main() {
  const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-ws-binary-type-'));
  const serverPath = path.join(tmpDir, 'server.mjs');
  fs.writeFileSync(
    serverPath,
    `
const describe = data => {
  if (data instanceof Blob) return 'blob:' + data.size;
  if (data instanceof ArrayBuffer) return 'arraybuffer:' + data.byteLength;
  if (ArrayBuffer.isView(data)) return 'view:' + data.byteLength;
  return typeof data;
};

const server = Ant.serve({
  hostname: '127.0.0.1',
  port: 0,
  fetch(request, ctx) {
    const url = new URL(request.url);
    if (url.pathname === '/stop') { queueMicrotask(() => ctx.stop()); return new Response('stopping'); }
    const { socket, response } = ctx.upgradeWebSocket(request);
    if (url.pathname === '/blob') socket.binaryType = 'blob';
    if (url.pathname === '/meta') {
      // Message events must look like real MessageEvents whichever path emits them.
      socket.addEventListener('message', event => {
        socket.send('listener:' + event.type + ':' + (event.target === socket) + ':' + (event.currentTarget === socket) + ':' + (event instanceof MessageEvent));
      });
      socket.onmessage = event => {
        socket.send('handler:' + event.type + ':' + (event.target === socket) + ':' + (event instanceof MessageEvent) + ':' + event.origin + ':' + event.lastEventId);
      };
      return response;
    }
    socket.binaryType = socket.binaryType; // setter round-trips
    socket.onmessage = event => {
      if (typeof event.data === 'string') {
        // "send:N" asks the server to send N binary bytes back.
        const n = Number(event.data.slice(5));
        socket.send(new Uint8Array(n));
        return;
      }
      socket.send('server-got:' + describe(event.data));
    };
    return response;
  }
});
console.log(JSON.stringify({ port: server.port, defaultBinaryType: 'x' }));
`
  );

  const child = spawn(process.execPath, [serverPath], { stdio: ['ignore', 'pipe', 'pipe'] });
  try {
    const { port } = JSON.parse(await waitForLine(child));
    const base = `ws://127.0.0.1:${port}`;

    // Default binaryType on both sides is "arraybuffer".
    const ws = await openSocket(`${base}/ws`);
    assert.equal(ws.binaryType, 'arraybuffer');

    let reply = nextMessage(ws);
    ws.send(new Uint8Array(5));
    assert.equal(await reply, 'server-got:arraybuffer:5');

    reply = nextMessage(ws);
    ws.send('send:9');
    assert.equal(describe(await reply), 'arraybuffer:9');

    // A subarray view must only deliver the bytes it covers.
    reply = nextMessage(ws);
    ws.send(new Uint8Array(new ArrayBuffer(32), 8, 4));
    assert.equal(await reply, 'server-got:arraybuffer:4');
    ws.close();

    // Event shape on both the addEventListener and on* paths.
    const meta = await openSocket(`${base}/meta`);
    const first = nextMessage(meta);
    meta.send('x');
    assert.equal(await first, 'listener:message:true:true:true');
    assert.equal(await nextMessage(meta), 'handler:message:true:true::');
    meta.close();

    // Invalid binaryType values are ignored.
    const probe = await openSocket(`${base}/ws`);
    probe.binaryType = 'nonsense';
    assert.equal(probe.binaryType, 'arraybuffer');
    probe.binaryType = 'blob';
    assert.equal(probe.binaryType, 'blob');
    probe.close();

    // binaryType = "blob" on both sides.
    const blobWs = await openSocket(`${base}/blob`, 'blob');
    assert.equal(blobWs.binaryType, 'blob');

    reply = nextMessage(blobWs);
    blobWs.send(new Uint8Array(3));
    assert.equal(await reply, 'server-got:blob:3');

    reply = nextMessage(blobWs);
    blobWs.send('send:6');
    const blob = await reply;
    assert.equal(describe(blob), 'blob:6');
    assert.equal((await blob.arrayBuffer()).byteLength, 6);
    blobWs.close();

    await fetch(`http://127.0.0.1:${port}/stop`);
    console.log('websocket:binary-type:ok');
  } finally {
    if (child.exitCode === null) child.kill('SIGTERM');
    fs.rmSync(tmpDir, { recursive: true, force: true });
  }
}

main().catch(error => {
  console.error(error && error.stack ? error.stack : error);
  process.exit(1);
});
