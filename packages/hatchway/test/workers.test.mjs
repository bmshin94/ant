import test from 'node:test';
import assert from 'node:assert/strict';
import { fileURLToPath } from 'node:url';
import { Miniflare } from 'miniflare';
import { connect } from 'hatchway/node';
import { websocket } from './helpers.mjs';

test('Workers transport runs in workerd, authenticates, evaluates, and expands live objects through the relay', async t => {
  const worker = new Miniflare({
    modules: true, scriptPath: fileURLToPath(new URL('./fixtures/worker.mjs', import.meta.url)),
    modulesRules: [{ type: 'ESModule', include: ['**/*.js', '**/*.mjs'] }],
    compatibilityDate: '2026-07-30', bindings: { HATCHWAY_TOKEN: 'worker-token', ENVIRONMENT: 'test' },
  });
  t.after(() => worker.dispose());
  const url = await worker.ready;
  assert.equal(await (await fetch(url)).text(), 'application response');
  const endpoint = new URL('/__hatchway', url);
  assert.equal((await fetch(`${endpoint}/json/list`)).status, 403);
  const headers = { Authorization: 'Bearer worker-token' };
  const targets = await (await fetch(`${endpoint}/json/list`, { headers })).json();
  assert.equal(targets[0].title, 'Test Worker');
  assert.equal(new URL(targets[0].webSocketDebuggerUrl).pathname, '/__hatchway/ws');
  assert.equal((await fetch(`${endpoint}/ws`, { headers })).status, 426);
  const relay = await connect(endpoint, { port: 0, headers });
  t.after(() => relay.close());
  const client = await websocket(relay.websocketUrl);
  t.after(() => client.close());
  await client.command('Runtime.enable');
  const preview = await client.command('Runtime.evaluate', { expression: 'increment', throwOnSideEffect: true });
  assert.ok(preview.result.exceptionDetails);
  const status = await client.command('Runtime.evaluate', { expression: 'status', returnByValue: true });
  assert.equal(status.result.result.value.commands, 0);
  const result = await client.command('Runtime.evaluate', { expression: 'hello worker', generatePreview: true });
  const properties = await client.command('Runtime.getProperties', { objectId: result.result.result.objectId, ownProperties: true });
  assert.equal(properties.result.result.find(p => p.name === 'environment').value.value, 'test');
  assert.equal(client.events.find(e => e.method === 'Runtime.consoleAPICalled').params.args[0].value, 'command executed');
  assert.match((await client.command('Runtime.evaluate', { expression: 'fail' })).result.exceptionDetails.exception.description, /Worker command failed/);
  await client.command('Runtime.releaseObject', { objectId: result.result.result.objectId });
  assert.ok((await client.command('Runtime.getProperties', { objectId: result.result.result.objectId })).error);
  const another = await websocket(relay.websocketUrl);
  t.after(() => another.close());
  assert.deepEqual((await another.command('Runtime.evaluate', { expression: 'status', returnByValue: true })).result.result.value,
    { commands: 1, nested: { platform: 'workers' } });
});
