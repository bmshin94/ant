import test from 'node:test';
import assert from 'node:assert/strict';
import { createHatchway } from 'hatchway';
import { harness } from './helpers.mjs';

test('DevTools initialization and application commands use separate contexts and connection data', async t => {
  const seen = [];
  const app = createHatchway({ name: 'App', names: ['help', 'status'],
    evaluate: (input, context) => { seen.push([input, context]); return { greeting: context.data.user }; },
    contexts: [{ name: 'Database', evaluate: input => `SQL: ${input}` }],
  });
  t.after(() => app.dispose());
  const client = harness(app, { user: 'Ada' });
  assert.deepEqual((await client.command('Runtime.enable')).result, {});
  const contexts = client.messages.filter(message => message.method === 'Runtime.executionContextCreated').map(m => m.params.context);
  assert.deepEqual(contexts.map(context => context.name), ['App', 'Database']);
  assert.deepEqual((await client.command('Runtime.globalLexicalScopeNames')).result.names, ['help', 'status']);
  assert.equal((await client.command('Runtime.evaluate', { expression: 'select 1', contextId: 2 })).result.result.value, 'SQL: select 1');
  const output = await client.command('Runtime.evaluate', { expression: 'show state', uniqueContextId: contexts[0].uniqueId, returnByValue: true });
  assert.deepEqual(output.result.result.value, { greeting: 'Ada' });
  assert.equal(seen[0][0], 'show state');
  assert.equal(seen[0][1].contextId, 1);
  assert.equal(seen[0][1].sessionId, client.session.id);
  assert.equal((await client.command('Runtime.evaluate', { expression: 'x', contextId: 99 })).error.code, -32602);
  assert.equal((await client.command('Runtime.evaluate', { expression: 'x', uniqueContextId: 'stale' })).error.code, -32602);
});

test('typing previews and Enter completeness checks never execute commands', async t => {
  let commands = 0;
  const app = createHatchway({ name: 'App', maxObjects: 1, evaluate: () => ++commands });
  t.after(() => app.dispose());
  const client = harness(app);
  for (let i = 0; i < 3; i++) {
    const result = await client.command('Runtime.evaluate', { expression: 'delete all', throwOnSideEffect: true });
    assert.equal(result.result.exceptionDetails.exception.className, 'EvalError');
  }
  assert.deepEqual((await client.command('Runtime.compileScript', { expression: 'not javascript!', persistScript: false })).result, {});
  assert.equal(commands, 0);
  assert.equal((await client.command('Runtime.evaluate', { expression: 'run' })).result.result.value, 1);
});

test('explicit previews are separate callbacks', async t => {
  let commands = 0;
  const app = createHatchway({ name: 'App', evaluate: () => ++commands, preview: input => `Preview: ${input}` });
  t.after(() => app.dispose());
  const client = harness(app);
  assert.equal((await client.command('Runtime.evaluate', { expression: 'hello', throwOnSideEffect: true })).result.result.value, 'Preview: hello');
  assert.equal(commands, 0);
});

test('primitive values survive the CDP wire format', async t => {
  const values = { nil: null, undefined, negative: -0, nan: NaN, inf: Infinity, ninf: -Infinity, big: 123n, text: 'hello', bool: false, number: 42 };
  const app = createHatchway({ name: 'App', evaluate: input => values[input] });
  t.after(() => app.dispose());
  const { command } = harness(app);
  const remote = async expression => (await command('Runtime.evaluate', { expression })).result.result;
  assert.equal((await remote('nil')).subtype, 'null');
  assert.equal((await remote('undefined')).type, 'undefined');
  for (const [key, expected] of [['negative', '-0'], ['nan', 'NaN'], ['inf', 'Infinity'], ['ninf', '-Infinity'], ['big', '123n']]) {
    const value = await remote(key);
    assert.equal(value.unserializableValue, expected);
    assert.equal('value' in value, false);
  }
  assert.equal((await remote('text')).value, 'hello');
  assert.equal((await remote('bool')).value, false);
  assert.equal((await remote('number')).value, 42);
});

test('objects expand without calling getters and children inherit the release group', async t => {
  let reads = 0;
  const key = Symbol('key');
  const value = Object.create({ inherited: true });
  Object.defineProperty(value, 'hidden', { value: 7 });
  Object.assign(value, { nested: { ok: true }, [key]: 'symbol value' });
  Object.defineProperty(value, 'getter', { enumerable: true, get() { reads++; return 'must not run'; } });
  value.self = value;
  const app = createHatchway({ name: 'App', evaluate: () => value });
  t.after(() => app.dispose());
  const { command } = harness(app);
  const result = (await command('Runtime.evaluate', { expression: 'state', objectGroup: 'result', generatePreview: true })).result.result;
  assert.equal(result.preview.properties.find(p => p.name === 'getter').type, 'accessor');
  const expanded = (await command('Runtime.getProperties', { objectId: result.objectId, ownProperties: true, generatePreview: true })).result;
  const props = new Map(expanded.result.map(p => [p.name, p]));
  assert.equal(props.get('hidden').enumerable, false);
  assert.equal(props.get('hidden').writable, false);
  assert.equal(props.get('getter').get.type, 'function');
  assert.equal(props.get('self').value.objectId, result.objectId);
  assert.equal(props.get('Symbol(key)').symbol.type, 'symbol');
  assert.equal(props.has('inherited'), false);
  const all = (await command('Runtime.getProperties', { objectId: result.objectId })).result.result;
  assert.equal(all.find(p => p.name === 'inherited').isOwn, false);
  assert.equal(reads, 0);
  const child = props.get('nested').value.objectId;
  await command('Runtime.releaseObjectGroup', { objectGroup: 'result' });
  for (const objectId of [child, result.objectId]) assert.equal((await command('Runtime.getProperties', { objectId })).error.code, -32000);
});

test('object IDs cannot cross sessions and releasing a group preserves other groups', async t => {
  const value = { alive: true };
  const app = createHatchway({ name: 'App', evaluate: () => value });
  t.after(() => app.dispose());
  const a = harness(app), b = harness(app);
  const first = (await a.command('Runtime.evaluate', { expression: '', objectGroup: 'first' })).result.result.objectId;
  const second = (await a.command('Runtime.evaluate', { expression: '', objectGroup: 'second' })).result.result.objectId;
  assert.equal((await b.command('Runtime.getProperties', { objectId: first })).error.code, -32000);
  await a.command('Runtime.releaseObjectGroup', { objectGroup: 'first' });
  assert.ok((await a.command('Runtime.getProperties', { objectId: second })).result);
  await a.command('Runtime.releaseObject', { objectId: second });
  assert.ok((await a.command('Runtime.getProperties', { objectId: second })).error);
});

test('returnByValue handles cycles, accessors, BigInts, toJSON, and __proto__ deliberately', async t => {
  let getterCalls = 0;
  const value = { normal: 1, get danger() { getterCalls++; return 0; } };
  const cycle = {}; cycle.self = cycle;
  const plain = JSON.parse('{"__proto__":{"marker":true},"value":3}');
  plain.toJSON = () => { throw new Error('must not run'); };
  const values = { value, cycle, plain, big: { nested: 4n } };
  const app = createHatchway({ name: 'App', evaluate: input => values[input] });
  t.after(() => app.dispose());
  const { command } = harness(app);
  for (const expression of ['value', 'cycle', 'big']) {
    assert.equal((await command('Runtime.evaluate', { expression, returnByValue: true })).error.code, -32000);
  }
  const returned = (await command('Runtime.evaluate', { expression: 'plain', returnByValue: true })).result.result.value;
  assert.deepEqual(returned, { ['__proto__']: { marker: true }, value: 3 });
  assert.equal(getterCalls, 0);
});

test('console events replay once, use previews, clear handles, and keep bounded history', async t => {
  const app = createHatchway({ name: 'App', maxConsoleEntries: 2, evaluate: () => {} });
  t.after(() => app.dispose());
  app.console.log('discarded'); app.console.log('kept'); app.console.warn({ x: 1 });
  const client = harness(app);
  await client.command('Runtime.enable'); await client.command('Runtime.enable'); await client.command('Console.enable');
  let events = client.messages.filter(m => m.method === 'Runtime.consoleAPICalled');
  assert.equal(events.length, 2);
  assert.equal(events[0].params.args[0].value, 'kept');
  const handle = events[1].params.args[0].objectId;
  assert.equal(events[1].params.type, 'warning');
  app.console.error('live');
  await client.command('Runtime.discardConsoleEntries');
  assert.ok((await client.command('Runtime.getProperties', { objectId: handle })).error);
  const other = harness(app);
  await other.command('Runtime.enable');
  assert.equal(other.messages.filter(m => m.method === 'Runtime.consoleAPICalled').length, 0);
});

test('async commands, rejection values, and promise handles', async t => {
  const app = createHatchway({ name: 'App', async evaluate(input) {
    if (input === 'fail') throw new Error('Application failed');
    if (input === 'throw-null') throw null;
    if (input === 'nested') return { pending: Promise.resolve(99) };
    return 42;
  } });
  t.after(() => app.dispose());
  const { command } = harness(app);
  assert.equal((await command('Runtime.evaluate', { expression: 'ok' })).result.result.value, 42);
  const failed = (await command('Runtime.evaluate', { expression: 'fail' })).result;
  assert.equal(failed.exceptionDetails.exception.description, 'Error: Application failed');
  assert.equal((await command('Runtime.evaluate', { expression: 'throw-null' })).result.exceptionDetails.exception.value, null);
  const nested = (await command('Runtime.evaluate', { expression: 'nested' })).result.result.objectId;
  const props = (await command('Runtime.getProperties', { objectId: nested, ownProperties: true })).result.result;
  const promiseObjectId = props[0].value.objectId;
  assert.equal((await command('Runtime.awaitPromise', { promiseObjectId })).result.result.value, 99);
});

test('timeout and disconnect abort commands without late responses', async t => {
  const signals = [];
  const app = createHatchway({ name: 'App', evaluationTimeout: 30, evaluate(_input, { signal }) {
    signals.push(signal); return new Promise(() => {});
  } });
  t.after(() => app.dispose());
  const first = harness(app);
  const timedOut = await first.command('Runtime.evaluate', { expression: 'wait' });
  assert.match(timedOut.result.exceptionDetails.exception.description, /timed out/);
  assert.equal(signals[0].aborted, true);
  const second = harness(app);
  const pending = second.command('Runtime.evaluate', { expression: 'wait' });
  await new Promise(resolve => setImmediate(resolve));
  second.session.close();
  assert.equal(await pending, undefined);
  assert.equal(signals[1].aborted, true);
  assert.equal(second.messages.length, 0);
});

test('pending evaluation and object limits leave the session usable', async t => {
  const app = createHatchway({ name: 'App', maxObjects: 1, maxPendingEvaluations: 1, evaluate: input =>
    input === 'wait' ? new Promise(() => {}) : ({ input }),
  });
  t.after(() => app.dispose());
  const client = harness(app);
  const pending = client.command('Runtime.evaluate', { expression: 'wait' });
  assert.equal((await client.command('Runtime.evaluate', { expression: 'wait' })).error.code, -32000);
  await client.command('Runtime.terminateExecution');
  assert.match((await pending).result.exceptionDetails.exception.description, /cancelled/);
  // The cancellation exception occupies the one handle; release all unnamed results.
  await client.command('Runtime.releaseObjectGroup', { objectGroup: '' });
  const returned = (await client.command('Runtime.evaluate', { expression: 'first' })).result.result;
  assert.ok((await client.command('Runtime.evaluate', { expression: 'second' })).error);
  await client.command('Runtime.releaseObject', { objectId: returned.objectId });
  assert.ok((await client.command('Runtime.evaluate', { expression: 'third' })).result);
});

test('malformed input and unsupported methods return errors; id zero works', async t => {
  const app = createHatchway({ name: 'App', evaluate: () => 5, maxMessageBytes: 256 });
  t.after(() => app.dispose());
  const client = harness(app);
  await client.session.receive('{');
  await client.session.receive('[]');
  await client.session.receive(JSON.stringify({ id: 0, method: 'Runtime.evaluate', params: { expression: 'hello' } }));
  assert.equal(client.messages[0].error.code, -32700);
  assert.equal(client.messages[1].error.code, -32600);
  assert.equal(client.messages[2].id, 0);
  assert.equal(client.messages[2].result.result.value, 5);
  assert.equal((await client.command('Debugger.pause')).error.code, -32601);
  assert.equal((await client.command('Runtime.callFunctionOn', { functionDeclaration: 'function(){}' })).error.code, -32601);
  await client.session.receive('x'.repeat(257));
  assert.equal(client.session.closed, true);
});

test('disposing is idempotent and prevents new sessions', () => {
  const app = createHatchway({ name: 'App', evaluate: () => 1 });
  let closed = 0;
  const session = app.connect({ send() {}, close() { closed++; } });
  app.dispose(); app.dispose(); session.close();
  assert.equal(closed, 1);
  assert.throws(() => app.connect({ send() {} }), /disposed/);
});
