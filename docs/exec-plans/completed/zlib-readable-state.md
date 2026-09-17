# Zlib Readable State

Status: completed
Last reviewed: 2026-09-16
Owner: theMackabu

## Outcome

Native zlib streams now initialize and inherit Ant's shared Readable machinery.
Output and EOF go through `stream_readable_push`, so buffering, pause/resume,
piping, and `endEmitted` share the normal readable lifecycle. Flush and reset
preserve an open readable side; reset does not reopen a completed one. The original
`createInflateRaw()._readableState.endEmitted` reproducer now prints `false`.

## Evidence and decisions

- The supplied reproducer throws because `zlib_create_stream` never creates
  `_readableState`. All nine factories share that constructor.
- `ws` 8.20.1's `lib/permessage-deflate.js` reads `endEmitted` in its inflate
  flush callback, then either closes the inflater or resets its message buffers
  and optionally its compression history. There is no missing-state fallback.
- Node 26.6.0 keeps the readable side open across ordinary flushes and resets.
  A final DEFLATE block followed by unconsumed input (including the WebSocket
  trailer) ends the readable side. A final block alone does not. Reset does not
  clear readable completion.
- Reuse `stream_init_readable_object` and `stream_readable_prototype`, with
  native zlib cleanup in `_destroy`. `include/ptr.h` supports multiple native
  tags on one object, as already used by child-process streams. Separate tags
  are not an ownership conflict and do not require a separate lifecycle.
- Defer flush callbacks until queued readable completion can run. This lets
  `ws` observe `endEmitted` and dispose of a completed inflater. Ordinary flush
  and reset retain the same readable state object.
- The shared readable lifecycle now updates `.readable` on end/destruction and
  `_readableState.destroyed` on destruction. Native zlib uses that path rather
  than mirroring those flags itself.
- Repeated messages exposed a second local cause: `js_zlib_write` discarded
  empty buffers before calling deflate. An empty write with `Z_NO_FLUSH` must
  update zlib's `last_flush` so the following `Z_SYNC_FLUSH` emits a new empty
  block. Otherwise `ws` receives no trailer to strip, and the next compressed
  message cannot be decoded. Preserve these writes in the same native path.

## Validation

- Built with `maid build` inside `maid shell`. The configured PGO build reported
  discarded old profile counts for changed functions; compilation and
  linking succeeded.
- `tests/test_zlib_readable_state.cjs` passes on Ant and Node 26.6.0. It covers
  all nine constructors, per-instance identity, flush/reset, text/binary/empty
  and fragmented messages, explicit end, both split and combined EOF/trailer
  input, close without end, buffering before consumption, pause/resume, and
  piping to a Writable.
- Negative control against the original binary failed with
  `TypeError: Cannot read properties of undefined (reading 'endEmitted')`.
- The same test's optional integration mode passes against installed `ws`
  8.20.1 on both runtimes. It exercises the actual permessage-deflate consumer,
  including context reuse, no-context-takeover resets, empty and fragmented
  messages, and disposal/recreation of completed inflaters:

  ```sh
  ./build/ant tests/test_zlib_readable_state.cjs "$PWD/docs/api/node_modules/ws/lib/permessage-deflate.js"
  node tests/test_zlib_readable_state.cjs "$PWD/docs/api/node_modules/ws/lib/permessage-deflate.js"
  ```

- `./build/ant examples/spec/run.js zlib streams-readable streams-transform
  streams-pipe streams-compression`: 253 passed, 0 failed, including all 100
  zlib cases. The `streams-*` specs cover Web Streams, not Node stream parity.
- All 10 `tests/test_stream_*.cjs` regressions pass, plus the child stdio and
  filesystem stream regressions. The expanded direct-property assertions also
  pass on Node 26.6.0.
- Repository checks pass via `./build/ant .github/agents/check_all.js`.

## Why coverage missed this

- `_readableState.endEmitted` existed in shared streams since commit `2a3f0fa9`
  (2026-04-02); zlib inherited EventEmitter directly and bypassed that state.
- The existing state-predicate test checked `stream.isReadable()` and
  `readableEnded`, but did not assert `.readable` after end/destruction or
  `_readableState.destroyed`. The predicate consulted other flags, hiding the
  stale public property. The regression now checks both surfaces.
- The still-active [stream property plan](../active/stream-property-surface.md)
  records incomplete readable getters and mirrored-property drift. The earlier
  [stream unification](event-emitter-process-child-streams.md) focused on events,
  writable backpressure, and child stdio; its green suite was not proof of full
  Node stream compatibility.

Full Transform writable scheduling and the remaining property-surface work stay
outside this fix. Tests compare payload bytes as hex strings to avoid unrelated
assertion-library buffer comparison differences. The optional `ws` integration
requires an installed package; the default regression has no external
dependencies.
