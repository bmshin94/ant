import test from 'node:test';
import assert from 'node:assert/strict';
import { request } from 'node:http';
import { once } from 'node:events';
import { spawn } from 'node:child_process';
import { createHatchway } from 'hatchway';
import { connect, listen } from 'hatchway/node';
import { websocket } from './helpers.mjs';

test('Node serves discovery and round-trips CDP over real WebSockets', async t => {
  const app = createHatchway({ name: 'Local app', evaluate: input => ({ command: input, values: [1, 2] }) });
  const server = await listen(app, { port: 0 });
  t.after(async () => { app.dispose(); await server.close(); });
  const targets = await (await fetch(`${server.url}/json/list`)).json();
  assert.equal(targets[0].title, 'Local app');
  assert.equal(targets[0].webSocketDebuggerUrl, server.websocketUrl);
  assert.equal(targets[0].devtoolsFrontendUrl, server.devtoolsUrl);
  assert.equal(new URL(server.devtoolsUrl).searchParams.get('ws'), server.websocketUrl.slice('ws://'.length));
  assert.equal((await (await fetch(`${server.url}/json/version`)).json())['Protocol-Version'], '1.3');
  const client = await websocket(server.websocketUrl);
  t.after(() => client.close());
  await client.command('Runtime.enable');
  const evaluated = await client.command('Runtime.evaluate', { expression: 'status', generatePreview: true });
  const properties = await client.command('Runtime.getProperties', { objectId: evaluated.result.result.objectId, ownProperties: true });
  assert.equal(properties.result.result.find(p => p.name === 'command').value.value, 'status');
  app.console.info('live log');
  await client.command('Runtime.getIsolateId');
  assert.equal(client.events.find(e => e.method === 'Runtime.consoleAPICalled').params.args[0].value, 'live log');
  assert.equal((await fetch(`${server.url}/missing`)).status, 404);
  assert.equal((await fetch(`${server.url}/json/list`, { method: 'POST' })).status, 405);
});

test('loopback server validates hosts, origins, paths, and optional authentication', async t => {
  const app = createHatchway({ name: 'Private app', evaluate: () => 'ok' });
  const server = await listen(app, { port: 0, authorize: req => req.headers.authorization === 'Bearer test-token' });
  t.after(async () => { app.dispose(); await server.close(); });
  assert.equal((await fetch(`${server.url}/json/list`)).status, 403);
  const headers = { Authorization: 'Bearer test-token' };
  assert.equal((await fetch(`${server.url}/json/list`, { headers })).status, 200);
  assert.equal((await fetch(`${server.url}/json/list`, { headers: { ...headers, Origin: 'https://unrelated.example' } })).status, 403);
  const invalidHost = await new Promise(resolve => {
    request(`${server.url}/json/list`, { headers: { ...headers, Host: 'other.example' } }, res => { res.resume(); resolve(res.statusCode); }).end();
  });
  assert.equal(invalidHost, 403);
  const localhost = await new Promise(resolve => {
    request(`${server.url}/json/list`, { headers: { ...headers, Host: `localhost:${new URL(server.url).port}` } }, res => {
      res.resume(); resolve(res.statusCode);
    }).end();
  });
  assert.equal(localhost, 200);
  await assert.rejects(websocket(server.websocketUrl), /403/);
  await assert.rejects(websocket(`${server.url.replace('http:', 'ws:')}/other`, { headers }), /404/);
  const client = await websocket(server.websocketUrl, { headers });
  t.after(() => client.close());
  assert.equal((await client.command('Runtime.evaluate', { expression: 'ok' })).result.result.value, 'ok');
  await assert.rejects(listen(app, { host: '0.0.0.0', port: 0 }), /authorize/);
});

test('relay keeps credentials upstream and forwards commands, events, and disconnect', async t => {
  const app = createHatchway({ name: 'Remote app', evaluate: input => ({ echo: input }) });
  const upstream = await listen(app, { port: 0, authorize: req => req.headers.authorization === 'Bearer test-token' });
  t.after(async () => { app.dispose(); await upstream.close(); });
  await assert.rejects(connect(upstream.url, { port: 0 }), /403/);
  const relay = await connect(upstream.url, { port: 0, headers: { Authorization: 'Bearer test-token' } });
  t.after(() => relay.close());
  const metadata = await (await fetch(`${relay.url}/json/list`)).text();
  assert.equal(metadata.includes('test-token'), false);
  const client = await websocket(relay.websocketUrl);
  t.after(() => client.close());
  await client.command('Runtime.enable');
  assert.deepEqual((await client.command('Runtime.evaluate', { expression: 'hello', returnByValue: true })).result.result.value, { echo: 'hello' });
  app.console.log('through relay');
  await client.command('Runtime.getIsolateId');
  assert.equal(client.events.at(-1).params.args[0].value, 'through relay');
  const closed = once(client.socket, 'close');
  await upstream.close();
  await closed;
});

test('CLI connects, prints a usable local target, and shuts down on SIGTERM', async t => {
  const app = createHatchway({ name: 'CLI app', evaluate: () => 42 });
  const server = await listen(app, { port: 0, authorize: req => req.headers.authorization === 'Bearer cli-token' });
  t.after(async () => { app.dispose(); await server.close(); });
  const child = spawn(process.execPath, ['dist/cli.js', 'connect', server.url, '--port', '0'], {
    cwd: new URL('..', import.meta.url), env: { ...process.env, HATCHWAY_TOKEN: 'cli-token' }, stdio: ['ignore', 'pipe', 'pipe'],
  });
  t.after(() => child.kill('SIGTERM'));
  let output = '';
  const printed = new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`CLI did not start: ${output}`)), 10000);
    child.once('error', error => { clearTimeout(timer); reject(error); });
    child.stdout.on('data', chunk => {
      output += chunk;
      if (output.includes('devtools://')) { clearTimeout(timer); resolve(); }
    });
    child.stderr.on('data', chunk => { output += chunk; });
  });
  await printed;
  assert.equal(output.includes('cli-token'), false);
  const url = output.match(/http:\/\/127\.0\.0\.1:\d+/)[0];
  const target = (await (await fetch(`${url}/json/list`)).json())[0];
  const client = await websocket(target.webSocketDebuggerUrl);
  assert.equal((await client.command('Runtime.evaluate', { expression: 'answer' })).result.result.value, 42);
  await client.close();
  const exited = once(child, 'exit');
  child.kill('SIGTERM');
  assert.equal((await exited)[0], 0);
});
