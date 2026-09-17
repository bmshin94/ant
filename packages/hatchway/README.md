# Hatchway

Open your app in DevTools.

Hatchway connects Chrome DevTools' Console to a callback in your application.
The callback can run application commands, query a database, or talk to an
interpreter. Results appear as expandable objects; application logs appear in
the same console. Hatchway implements a subset of the Chrome DevTools Protocol
(CDP) in TypeScript and requires no Ant runtime or Wasm module.

## Node

```sh
npm install hatchway
```

```js
import { createHatchway } from 'hatchway';
import { listen } from 'hatchway/node';

const state = { requests: 0, queue: ['email', 'invoice'] };
const app = createHatchway({
  name: 'My app',
  names: ['help', 'status'],
  evaluate(input) {
    if (input.trim() === 'help') return 'Commands: help, status';
    if (input.trim() === 'status') return state;
    throw new Error(`Unknown command: ${input}`);
  },
});

const server = await listen(app); // 127.0.0.1:9229
console.log(server.devtoolsUrl);
app.console.log('Ready. Enter help.');
```

Paste the printed `devtools://` URL into Chrome, or configure `localhost:9229`
in `chrome://inspect`. Enter `status`, then expand the result. Input goes to
your callback; Hatchway never calls `eval()` or `new Function()`.

`listen(app, { port: 0 })` selects an available port. Its result contains `url`,
`websocketUrl`, `devtoolsUrl`, and an async `close()` method. Closing the server
disconnects its clients. Call `app.dispose()` when the application no longer
needs Hatchway; this also clears its retained logs.

The package requires Node 22 or newer for its Node adapter and CLI. The core
and Workers entrypoints do not import Node modules. The Node adapter uses `ws`.

## Cloudflare Workers

```js
import { createHatchway } from 'hatchway';
import { createWorkerHandler } from 'hatchway/workers';

const app = createHatchway({
  name: 'My Worker',
  evaluate(input, { data: env }) {
    if (input.trim() === 'status') {
      return { environment: env.ENVIRONMENT };
    }
    throw new Error(`Unknown command: ${input}`);
  },
});

const inspect = createWorkerHandler(app, {
  authorize(request, env) {
    return !!env.HATCHWAY_TOKEN &&
      request.headers.get('Authorization') === `Bearer ${env.HATCHWAY_TOKEN}`;
  },
});

export default {
  async fetch(request, env) {
    return await inspect(request, env) ?? new Response('Hello from the app');
  },
};
```

The handler owns `/__hatchway` and its subpaths and returns `undefined` for
other paths. Set `path` to mount it elsewhere. `authorize` is required and
runs for discovery and WebSocket upgrades. Connection data is captured from
the accepted request and supplied to each command as `data`.

Connect through a local relay so Chrome does not need authentication headers:

```sh
# Set HATCHWAY_TOKEN in your environment to match the Worker secret.
npx hatchway connect https://my-app.example/__hatchway
```

`HATCHWAY_TOKEN` becomes a Bearer header on the remote discovery request and
WebSocket handshake. Credentials are not placed in the local DevTools URL.
Use `--port 9230` to change the relay's local port. The programmatic equivalent
supports custom authentication headers:

```js
import { connect } from 'hatchway/node';

const relay = await connect('https://my-app.example/__hatchway', {
  port: 9229,
  headers: { Authorization: `Bearer ${token}` },
});
console.log(relay.devtoolsUrl);
// Later: await relay.close();
```

A connection belongs to one Worker isolate. Other requests can reach other
isolates, and reconnects can lose in-memory state. For one application instance,
create Hatchway and its handler inside a Durable Object and route the inspector
requests to that object. This adapter uses ordinary WebSocket event listeners;
it does not implement Durable Object WebSocket hibernation.

## Callback contract

`evaluate(input, context)` receives DevTools' expression string and returns a
value or a Promise for a value. Hatchway awaits command results and reports
thrown values and rejections as console exceptions. DevTools may normalize
JavaScript-looking input, such as wrapping an object literal in parentheses.

The context contains `sessionId`, `contextId`, `data`, and `signal`. `signal`
is aborted when the evaluation times out, the client cancels it, or the session
disconnects. Cancellation is cooperative: it cannot interrupt synchronous
code or undo work a callback already performed. Pass the signal to operations
that support cancellation.

Chrome asks for previews while you type. These requests **never call
`evaluate`**. By default they receive a side-effect exception. If your app has
a read-only preview operation, provide it separately:

```js
const app = createHatchway({
  name: 'My app',
  evaluate: input => commands.execute(input),
  preview: input => commands.describe(input),
  names: ['help', 'status', 'routes'],
});
```

`preview` must be safe to run repeatedly while typing. `names` supplies
top-level names to CDP's `Runtime.globalLexicalScopeNames`; it is not a custom
completion provider. Chrome retains its JavaScript syntax and completion UI.

Additional contexts appear in the Console's execution-context selector:

```js
const app = createHatchway({
  name: 'Application',
  evaluate: input => application.execute(input),
  contexts: [{ name: 'Database', evaluate: input => database.execute(input) }],
});
```

The main context has ID 1; additional contexts use 2, 3, and so on. All share
the session's connection data. Use `app.emitConsole('info', args, contextId)`
to direct a log to a particular context.

## Logs and objects

`app.console` provides `log`, `info`, `warn`, `error`, `debug`, `dir`, `table`,
and `clear`. It does not replace the global console. Logs are broadcast to
connected sessions and a bounded recent history is replayed once per session.

Objects are live references. Expanding an earlier result can show later
mutations. Handles belong to one session and one object group. CDP release
commands, clearing the console, and disconnecting release those references.
Logged values can remain retained by the application's recent log history
until it is cleared, evicted, or disposed.

Properties are inspected through descriptors: getters, `toJSON`, and custom
inspection methods are not invoked by the object viewer. JavaScript Proxy
traps can still run during reflection. Circular references are supported when
viewing objects. `returnByValue` instead copies enumerable data properties and
rejects cycles, accessors, nested BigInts, and oversized structures.

Numbers including `NaN`, infinities, and `-0`, as well as BigInts and symbols,
use CDP's corresponding representations. Arrays, errors, Maps, Sets, dates,
and Promises have basic descriptions; Maps and Sets expose `[[Entries]]`.

## Custom transport

```js
const session = app.connect({
  send: text => socket.send(text),
  close: () => socket.close(),
}, connectionData);

socket.addEventListener('message', event => {
  void session.receive(event.data);
});
socket.addEventListener('close', () => session.close());
```

`receive` accepts CDP JSON text and sends responses/events through `send`.
Call `session.close()` on transport errors as well. `send` must be synchronous;
custom transports own their authentication, buffering, and backpressure.

The bundled Node adapter defaults to loopback, validates the Host and Origin,
and requires an `authorize` callback when binding elsewhere. Both adapters
accept `allowedOrigins` for an explicitly trusted web frontend. A browser
frontend can have its own connection restrictions; the local relay is the
supported path to remote applications.

## Limits and protocol scope

Options on `createHatchway`:

| Option | Default |
| --- | ---: |
| `maxObjects` | 10,000 handles per session |
| `maxProperties` | 1,000 returned properties per expansion |
| `maxConsoleEntries` | 100 retained log entries |
| `maxMessageBytes` | 1 MiB incoming CDP message |
| `maxPendingEvaluations` | 16 per session |
| `evaluationTimeout` | 30,000 ms |

All limits are positive integers. Node WebSocket transports also cap incoming
frames and outbound queues. Workers applications should bound the size of
returned values and logs; its WebSocket API does not expose queue length.

Supported operations cover runtime/context initialization, evaluation,
side-effect previews, object properties and releases, awaiting Promise handles,
console events, names, cancellation, and basic frontend setup. Enter-key
completeness probes succeed for arbitrary command syntax without execution.

There is no source debugger, heap/CPU profiler, network instrumentation,
arbitrary `Runtime.callFunctionOn`, or JavaScript execution built into Hatchway.
Unsupported operations return CDP errors. DevTools features that depend on
injected JavaScript helpers, including property editing, getter invocation,
some completions, copying objects, and large-array range expansion, are not
implemented. The initial target is the Chrome Console; VS Code compatibility
has not been validated.

## Development

From `packages/hatchway`:

```sh
npm install
npm test
npm run example
```

`npm test` builds the package and runs protocol, Node WebSocket, CLI/relay,
and real local workerd integration tests. A separate smoke test exercises
Chrome's bundled DevTools frontend:

```sh
npx playwright install chromium
npm run test:devtools
```

`npm pack --dry-run` checks the distributable. Building emits ESM JavaScript
and declarations to `dist/`; generated output is not committed.

Protocol references: [CDP Runtime](https://chromedevtools.github.io/devtools-protocol/v8/Runtime/),
[DevTools frontend](https://github.com/ChromeDevTools/devtools-frontend), and
[Workers WebSockets](https://developers.cloudflare.com/workers/runtime-apis/websockets/).
