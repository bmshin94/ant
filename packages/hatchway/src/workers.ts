import type { Hatchway } from './index.js';
import { originAllowed, target, version } from './discovery.js';

// Structural types keep Cloudflare's global declarations out of the core and
// Node entrypoints. The runtime supplies WebSocketPair and 101 Responses.
interface WorkerSocket {
  accept(): void;
  send(message: string): void;
  close(code?: number, reason?: string): void;
  addEventListener(type: 'message', callback: (event: { data: unknown }) => void): void;
  addEventListener(type: 'close' | 'error', callback: () => void): void;
}

export interface WorkerOptions<T> {
  path?: string;
  authorize(request: Request, data: T): boolean | Promise<boolean>;
  allowedOrigins?: readonly string[];
}

export function createWorkerHandler<T>(hatchway: Hatchway<T>, options: WorkerOptions<T>) {
  const path = (options.path ?? '/__hatchway').replace(/\/$/, '');
  if (!path.startsWith('/') || path.includes('?') || path.includes('#')) throw new TypeError('path must be an absolute URL path');
  if (typeof options.authorize !== 'function') throw new TypeError('Workers requires an authorize callback');
  return async function handle(request: Request, data: T): Promise<Response | undefined> {
    const url = new URL(request.url);
    if (url.pathname !== path && !url.pathname.startsWith(`${path}/`)) return undefined;
    const reply = (body: string, status = 200, type = 'text/plain; charset=utf-8') =>
      new Response(body, { status, headers: { 'Content-Type': type, 'Cache-Control': 'no-store' } });
    try {
      if (!originAllowed(request.headers.get('Origin'), options.allowedOrigins) || !await options.authorize(request, data)) {
        return reply('Forbidden', 403);
      }
    } catch { return reply('Forbidden', 403); }
    if (request.method !== 'GET') return reply('Method not allowed', 405);
    const wsUrl = new URL(url);
    wsUrl.protocol = url.protocol === 'https:' ? 'wss:' : 'ws:';
    wsUrl.pathname = `${path}/ws`;
    wsUrl.search = '';
    const info = target(hatchway.name, hatchway.id, wsUrl.href);
    const route = url.pathname.slice(path.length);
    if (route === '/json/list' || route === '/json') return reply(JSON.stringify([info]), 200, 'application/json');
    if (route === '/json/version') return reply(JSON.stringify(version), 200, 'application/json');
    if (route === '' || route === '/') return reply(`${hatchway.name}\nConnect with: hatchway connect ${url.origin}${path}\n`);
    if (route !== '/ws') return reply('Not found', 404);
    if (request.headers.get('Upgrade')?.toLowerCase() !== 'websocket') return reply('Expected WebSocket upgrade', 426);
    const Pair = (globalThis as unknown as { WebSocketPair: new () => { 0: WorkerSocket; 1: WorkerSocket } }).WebSocketPair;
    if (!Pair) throw new Error('The Workers adapter requires a Workers runtime');
    const pair = new Pair();
    const server = pair[1];
    server.accept();
    const session = hatchway.connect({ send: message => server.send(message), close: () => server.close(1000, 'Session closed') }, data);
    server.addEventListener('message', event => {
      if (typeof event.data !== 'string') { server.close(1003, 'CDP requires text messages'); return; }
      void session.receive(event.data);
    });
    server.addEventListener('close', () => session.close());
    server.addEventListener('error', () => session.close());
    return new Response(null, { status: 101, webSocket: pair[0] } as ResponseInit);
  };
}
