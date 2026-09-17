# Zlib Readable State

Status: completed
Last reviewed: 2026-09-16
Owner: theMackabu

## Outcome

Native zlib streams now expose per-instance readable lifecycle state, including
`endEmitted`. Flush and reset preserve an open readable side; completion marks it
ended before emitting `end`, and reset does not reopen it. The original
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
- Add per-instance lifecycle state and update it at native zlib event sites.
  A constant `false` would conceal completion and make `ws` reuse an exhausted
  inflater. A full Transform migration would also change buffering, callback
  scheduling, and native ownership: shared stream methods require
  `STREAM_NATIVE_TAG`, while zlib owns `ZLIB_STREAM_TAG` on the same object.
  That migration is outside this fix; existing eager delivery remains.
- Repeated messages exposed a second local cause: `js_zlib_write` discarded
  empty buffers before calling deflate. An empty write with `Z_NO_FLUSH` must
  update zlib's `last_flush` so the following `Z_SYNC_FLUSH` emits a new empty
  block. Otherwise `ws` receives no trailer to strip, and the next compressed
  message cannot be decoded. Preserve these writes in the same native path.

## Validation

- Built with `maid build` inside `maid shell`. The configured PGO build reported
  discarded old profile counts for the two changed functions; compilation and
  linking succeeded.
- `tests/test_zlib_readable_state.cjs` passes on Ant and Node 26.6.0. It covers
  all nine constructors, per-instance identity, flush/reset, text/binary/empty
  and fragmented messages, explicit end, both split and combined EOF/trailer
  input, and close without end.
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

- `./build/ant examples/spec/run.js zlib`: 100 passed, 0 failed.
- `maid preflight`, `maid knowledge`, and `git diff --check` pass.

This preserves the existing native stream's eager event delivery. Full Node
Transform buffering/scheduling compatibility and unrelated assertion-library
buffer comparison differences are outside this change. Tests compare payload
bytes as hex strings to avoid those assertion differences. The optional `ws`
integration requires an installed package; the default regression has no
external dependencies.
