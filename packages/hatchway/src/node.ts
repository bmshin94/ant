import { createServer, type IncomingMessage } from 'node:http';
import { randomUUID } from 'node:crypto';
import type { AddressInfo } from 'node:net';
import { WebSocket, WebSocketServer } from 'ws';
import type { Hatchway } from './index.js';
import { originAllowed, target, version } from './discovery.js';

export interface ListenOptions<T = unknown> {
  port?: number;
  host?: string;
  data?: T;
  authorize?(request: IncomingMessage): boolean | Promise<boolean>;
  allowedOrigins?: readonly string[];
}

export interface Listener {
  url: string;
  websocketUrl: string;
  devtoolsUrl: string;
  close(): Promise<void>;
}

interface ServerOptions extends ListenOptions {
  name: string;
  connected(socket: WebSocket): void;
}

function send(socket: WebSocket, message: string): void {
  if (socket.readyState !== WebSocket.OPEN) return;
  if (socket.bufferedAmount + Buffer.byteLength(message) > 4 * 1024 * 1024) {
    socket.close(1013, 'Inspector client is too slow');
    return;
  }
  socket.send(message);
}

async function startServer(options: ServerOptions): Promise<Listener> {
  const host = options.host ?? '127.0.0.1';
  if (!['127.0.0.1', 'localhost', '::1'].includes(host) && !options.authorize) {
    throw new Error('Listening outside loopback requires an authorize callback');
  }
  const id = randomUUID();
  const sockets = new Set<WebSocket>();
  const wss = new WebSocketServer({ noServer: true, maxPayload: 1024 * 1024, perMessageDeflate: false });
  let address = '';
  let allowedHosts = new Set<string>();
  let closing = false;
  let closePromise: Promise<void> | undefined;
  const authorized = async (request: IncomingMessage): Promise<boolean> => {
    try {
      if (!originAllowed(request.headers.origin, options.allowedOrigins)) return false;
      if (!request.headers.host || !allowedHosts.has(request.headers.host)) return false;
      return options.authorize ? await options.authorize(request) === true : true;
    } catch { return false; }
  };
  const discovery = () => target(options.name, id, `ws://${address}/${id}`);
  const server = createServer((request, response) => {
    void (async () => {
      response.setHeader('Cache-Control', 'no-store');
      if (!await authorized(request)) { response.writeHead(403).end('Forbidden'); return; }
      if (request.method !== 'GET') { response.writeHead(405).end('Method not allowed'); return; }
      const path = new URL(request.url ?? '/', `http://${address}`).pathname;
      let body: unknown;
      if (path === '/json/list' || path === '/json') body = [discovery()];
      else if (path === '/json/version') body = version;
      else if (path === '/' || path === '/devtools') {
        response.setHeader('Content-Type', 'text/plain; charset=utf-8');
        response.end(`${options.name}\n\nPaste this URL into Chrome:\n${discovery().devtoolsFrontendUrl}\n`);
        return;
      } else { response.writeHead(404).end('Not found'); return; }
      response.setHeader('Content-Type', 'application/json; charset=utf-8');
      response.end(JSON.stringify(body));
    })().catch(() => { if (!response.headersSent) response.writeHead(500); response.end(); });
  });
  server.on('upgrade', (request, socket, head) => {
    socket.on('error', () => socket.destroy());
    void (async () => {
      if (closing || !await authorized(request)) {
        socket.end('HTTP/1.1 403 Forbidden\r\nConnection: close\r\nContent-Length: 0\r\n\r\n');
        return;
      }
      if (request.method !== 'GET' || request.url !== `/${id}`) {
        socket.end('HTTP/1.1 404 Not Found\r\nConnection: close\r\nContent-Length: 0\r\n\r\n');
        return;
      }
      if (closing || socket.destroyed) { socket.destroy(); return; }
      wss.handleUpgrade(request, socket, head, ws => {
        sockets.add(ws);
        ws.on('error', () => ws.terminate());
        ws.once('close', () => sockets.delete(ws));
        try { options.connected(ws); } catch { ws.close(1011, 'Cannot open inspector session'); }
      });
    })().catch(() => socket.destroy());
  });
  await new Promise<void>((resolve, reject) => {
    server.once('error', reject);
    server.listen(options.port ?? 9229, host, () => {
      server.off('error', reject);
      const bound = server.address() as AddressInfo;
      address = `${host.includes(':') ? `[${host}]` : host}:${bound.port}`;
      allowedHosts = new Set([address]);
      if (['127.0.0.1', 'localhost', '::1'].includes(host)) {
        for (const alias of ['127.0.0.1', 'localhost', '[::1]']) allowedHosts.add(`${alias}:${bound.port}`);
      }
      resolve();
    });
  });
  return {
    url: `http://${address}`, websocketUrl: discovery().webSocketDebuggerUrl, devtoolsUrl: discovery().devtoolsFrontendUrl,
    close() {
      return closePromise ??= new Promise<void>((resolve, reject) => {
        closing = true;
        for (const ws of sockets) ws.terminate();
        wss.close();
        server.close(error => error ? reject(error) : resolve());
        server.closeAllConnections();
      });
    },
  };
}

export function listen<T>(hatchway: Hatchway<T>, options: ListenOptions<T> = {}): Promise<Listener> {
  return startServer({
    ...options, name: hatchway.name,
    connected(socket) {
      const session = hatchway.connect({ send: message => send(socket, message), close: () => socket.close() }, options.data);
      socket.on('message', (message, binary) => {
        if (binary) { socket.close(1003, 'CDP requires text messages'); return; }
        void session.receive(message.toString());
      });
      socket.once('close', () => session.close());
    },
  });
}

export interface RelayOptions extends ListenOptions {
  /** Sent only to the remote target, never included in local discovery URLs. */
  headers?: Record<string, string>;
}

/** Publish an authenticated remote Hatchway endpoint as a local DevTools target. */
export async function connect(endpoint: string | URL, options: RelayOptions = {}): Promise<Listener> {
  const remote = new URL(endpoint);
  if (!['http:', 'https:'].includes(remote.protocol) || remote.username || remote.password || remote.search || remote.hash) {
    throw new Error('Expected an http(s) Hatchway endpoint without credentials, query, or fragment');
  }
  remote.pathname = `${remote.pathname.replace(/\/$/, '')}/json/list`;
  const response = await fetch(remote, { headers: options.headers, redirect: 'error', signal: AbortSignal.timeout(10000) });
  if (!response.ok) throw new Error(`Remote discovery failed (${response.status})`);
  const targets: unknown = await response.json();
  if (!Array.isArray(targets) || !targets[0] || typeof targets[0].webSocketDebuggerUrl !== 'string') {
    throw new Error('Remote endpoint did not return a DevTools target');
  }
  const selected = targets[0];
  const wsUrl = new URL(selected.webSocketDebuggerUrl);
  if (wsUrl.host !== remote.host || wsUrl.protocol !== (remote.protocol === 'https:' ? 'wss:' : 'ws:') || wsUrl.username || wsUrl.password) {
    throw new Error('Remote WebSocket must use the same host and security as discovery');
  }
  return startServer({
    ...options, name: typeof selected.title === 'string' ? selected.title : 'Hatchway',
    connected(local) {
      const upstream = new WebSocket(wsUrl, {
        headers: options.headers, handshakeTimeout: 10000, maxPayload: 4 * 1024 * 1024,
        perMessageDeflate: false, followRedirects: false,
      });
      const queued: string[] = [];
      let queuedBytes = 0;
      local.on('message', (data, binary) => {
        if (binary) { local.close(1003, 'CDP requires text messages'); return; }
        const message = data.toString();
        if (upstream.readyState === WebSocket.OPEN) send(upstream, message);
        else if (upstream.readyState === WebSocket.CONNECTING) {
          queuedBytes += Buffer.byteLength(message);
          if (queuedBytes > 1024 * 1024) local.close(1013, 'Remote inspector is not ready');
          else queued.push(message);
        }
      });
      upstream.on('open', () => {
        if (local.readyState !== WebSocket.OPEN) { upstream.close(); return; }
        for (const message of queued) send(upstream, message);
        queued.length = 0;
      });
      upstream.on('message', (data, binary) => {
        if (binary) local.close(1003, 'CDP requires text messages');
        else send(local, data.toString());
      });
      upstream.on('error', () => local.close(1011, 'Remote inspector connection failed'));
      upstream.once('close', () => { queued.length = 0; local.close(1000, 'Remote inspector disconnected'); });
      local.once('close', () => { queued.length = 0; upstream.terminate(); });
    },
  });
}
