# Hatchway

Status: completed
Last reviewed: 2026-09-17
Owner: theMackabu

## Outcome

Implemented [Hatchway](../../../packages/hatchway/README.md), a standalone
TypeScript package that presents application command callbacks through Chrome
DevTools' Console using CDP. It includes a transport-independent core, Node and
Workers adapters, a local authenticated relay CLI, and examples. No Ant runtime
or Wasm module is required. Package contents are prepared locally; no publish
was performed.

## Decisions

- A transport-independent core owns sessions, execution contexts, console
  events, protocol routing, and per-session remote object handles.
- An explicit `evaluate` callback handles submitted input. DevTools speculative
  evaluations use a separate optional `preview` callback; otherwise they are
  rejected without running application commands.
- Property browsing uses descriptors and does not invoke accessors. Reflection
  can still trigger Proxy traps; application code remains trusted.
- Node provides a loopback discovery/WebSocket server. Workers provides an
  authenticated fetch/WebSocket handler with per-connection application data.
- A local `hatchway connect` relay authenticates to remote endpoints without
  putting remote credentials in DevTools URLs.
- Object groups, disconnect cleanup, bounded histories, message limits, and
  cooperative evaluation cancellation are part of the initial implementation.

## Validation

- `npm test`: 18 passing tests, covering protocol initialization, context and
  connection-data routing, speculative evaluations, remote values, descriptors,
  circular references, object groups, session separation, console replay,
  async results/errors, cancellation, limits, malformed messages, Node discovery
  and WebSockets, authenticated relay, CLI shutdown, and local workerd.
- `npm run test:devtools`: passed against Playwright's Chrome for Testing
  153.0.8010.12. Confirmed arbitrary command syntax (`show state`), no command
  execution during typing, nested property expansion, live logs, and errors.
- `npm pack --dry-run`: verified ESM output, declarations, source maps with
  embedded source, README, license, and CLI entrypoint.
- `maid preflight`, `maid knowledge`, and `git diff --check`: passed. The repo
  validation router recommends knowledge checks for this package/doc change;
  native Ant builds and spec suites are outside its validation scope.

## Limits and follow-ups

The initial implementation targets the Console. It does not provide a source
debugger, network instrumentation, CPU/heap profiling, or arbitrary
`Runtime.callFunctionOn`. Features that need injected JavaScript helpers,
including property editing, getter invocation, some completions, copying
objects, and large-array range expansion, remain unsupported. Unsupported CDP
commands return errors. VS Code has not been validated.

The Workers transport was tested in local workerd, not deployed to Cloudflare.
Connections belong to one isolate; durable state and instance affinity are the
application's responsibility. Hibernating Durable Object sockets are not
supported. Evaluation cancellation is cooperative and property reflection may
execute Proxy traps. The package README records the public contracts and
configuration limits for future work.
