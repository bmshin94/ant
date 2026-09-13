# Test Sweep 2026-09-07

Status: active
Last reviewed: 2026-09-13
Owner: theMackabu

Latest full rerun: 2026-09-13 against the clean source tree at
`b1beeac2890237d112e5f07eb0f4f06853ce037d`. This supersedes the September 7
acceptance totals. No runtime or test sources were changed during this rerun.
The remaining work is the differential gaps and three stale test checks below.

## Current totals

| Suite | Result |
| --- | --- |
| JavaScript `tests/test_*` (628 files) | 618 clean passes, 3 stale checks that print failures but exit 0, 3 platform skips, 3 intentional non-zero exits, 1 expected timeout |
| Native `tests/test_*.c` (19 files) | 18 pass; 1 Linux-only test not run on macOS |
| JIT harness (`examples/jit/run.js`) | 10 of 10 files pass |
| Spec suite (`examples/spec/run.js --all`) | 4229 tests, 102 files, 0 failures |
| Differential runner (`--cases 5 --seed 44`) | 14 of 25 probes match; 11 mismatch |
| Differential runner unit checks | pass |
| `maid preflight` and recommended `maid knowledge` | knowledge and structure checks pass |

The JavaScript process-status totals are **624 exit 0, 3 intentional non-zero
exits, 1 expected timeout, and no unexpected non-zero exits or timeouts**.
The 624 includes the three skips and three stale-check files; those six are
separated from clean passes above. Counts describe whole files, including
sample/helper files, rather than individual assertions. There are 52 more
JavaScript files than in the September 7 evening sweep.

## Build and sweep method

- Host: macOS 27.0, arm64; Node `v26.8.1`; configured Clang `21.1.8` build.
- Configuration: release, optimization 3, LTO and PGO enabled, system
  allocator, Temporal enabled, no sanitizer.
- `meson compile -C build` and `meson compile -C build ant-runtime` both
  completed with no work to do. The existing configured version metadata is
  `15.1.0c62ef72.0`; it was not refreshed to the source HEAD. The exact tested
  binaries are identified below, and their hashes were unchanged after testing.
- Run every top-level `tests/test_*` file with extension `.js`, `.cjs`, or
  `.mjs` using the absolute `build/ant` path, from the repository root, six
  workers, 90 seconds per file, and stdin closed. Prepend `build/` to `PATH`
  so child commands resolving `ant` use this build. Capture combined stdout
  and stderr per file; kill the test process group at the timeout.
- Run the JIT harness, full spec suite, and differential commands after the
  JavaScript file sweep. Review failure/skip output as well as exit status,
  and rerun the three suspicious exit-zero files individually.
- Run `meson test -C build --print-errorlogs`: all six registered Ant C tests
  and the additional vendored zlib-ng example pass. Build and run the ten
  other configured `test-*` targets for listener accept, WebSocket cancellation,
  HTTP protocols, and the seven sandbox tests. Compile the two unregistered
  cage-allocator and object-absence-guard tests using the configured
  HTTP-protocol test's compiler/linker flags and `libant.a`, with `-UNDEBUG`;
  both pass. `test_native_stack_limits.c` requires Linux's
  `pthread_getattr_np` and is excluded on this host.

Tested SHA-256 values:

```text
build/ant          cfe5ca752b64c868b736967202d746e4542f1a88db2a625cec8c4befcbf7f9b4
build/ant-runtime  a7af98190d98ca0bf94564e3e3c998e9f5d5eaee18d77a9e0c9c161dd7fb62e8
```

Local evidence is under `/tmp/ant-test-sweep-2026-09-13/`: `metadata.json`,
`run_sweep.py`, raw `tests.json`, classified `tests-reviewed.json`, per-file
`tests/*.log`, `suites.json`, `suites/*.log`, `rechecks.json`, and native build
and run logs with `run_native.py`. These are temporary local artifacts, not
versioned acceptance evidence; the durable results are recorded here.

The differential commands were:

```sh
node tests/differential/runner.mjs --ant ./build/ant --cases 5 --seed 44 --json --output /tmp/ant-test-sweep-2026-09-13/differential-output
node tests/differential/test.mjs
```

## Stale checks that still exit zero

All three messages reproduce when run individually. They are test-maintenance
issues, not evidence of a new runtime regression. They must not be silently
counted as clean passes merely because the process exits zero.

| File | Observed output and classification |
| --- | --- |
| `test_fs_async.js` | Prints `✗ Content mismatch!` and then `All tests passed!`. It compares `readFile()` without an encoding directly to a string. A focused probe confirms that the result is a Buffer with the expected contents; strict comparison to a string is false. The sample also prints `stats.isFile` and `stats.isDirectory` as properties instead of calling the methods. |
| `test_define_property_edge_cases.js` | Test 6 prints `FAIL: Should have thrown error` for defining an own `__proto__` property. Node `v26.8.1` produces the same message on this unchanged test: the expectation that this definition throws is incorrect. |
| `test_gc_tco.js` | Prints seven arena-size failures and `Failures: 7`, then exits zero. It reads `Ant.stats().arenaUsed`, which is undefined in the current stats API, so every arena-size check gets `NaN MB`. Its tail-call result checks pass, but the intended allocation-pressure checks are not valid. |

Follow-up: update these checks for the current contracts and make actual
assertion failures produce a non-zero exit. No test changes are included in
this documentation update.

## Expected exits, timeout, and platform skips

These three intentional non-zero exits remain sample behavior. Do not fix or
remove them to make an exit-status sweep green.

| Test | Why it exits non-zero |
| --- | --- |
| `test_highlight_long_strings.js` | Throws after three long string lines so the syntax highlighter can be inspected near the terminal edge. |
| `test_throw_stack.cjs` | Throws a string through three nested frames to show the stack trace format. |
| `test_with_strict.cjs` | Expects the SyntaxError for a `with` statement under `"use strict"`. |

`test_gc_stress10.js` remains an open-ended stress loop. It was still rendering
at the 90-second cutoff (frame 814 at 89.88 seconds; last reported RSS 26.4 MB).
This is the expected timeout, not a completed stress test or a long-term memory
stability claim.

The three JavaScript platform skips are `test_ant_cron_linux.cjs`,
`test_ant_cron_windows.cjs`, and `test_fs_mkdir_recursive_windows.cjs`.
`ant-runtime` was present: `test_compile_basic.cjs` and
`test_compile_native_addon.cjs` completed without taking their missing-runtime
skip paths. `test_eval.cjs`, `test_jit_for_of.cjs`, and
`test_websocket_client_buffered_frames.cjs` also pass.

## Differential runner mismatches

The mismatching probe IDs are unchanged from the September 7 record. All
probes complete without an engine error or timeout; the runner exits 1 because
of output differences. All five Promise-timing and all five path probes match.

| Probes | Current difference from Node |
| --- | --- |
| `regexp-0` through `regexp-4` | Empty-pattern `source` is still `""` in the affected cases instead of `"(?:)"`; named `$<m>` replacements remain literal; empty global replacement on an astral character produces replacement characters instead of preserving UTF-16 surrogate halves. |
| `property-1`, `property-4` | `JSON.stringify` omits the property whose key contains a NUL byte. Get, has, descriptor, and key enumeration agree with Node. |
| `stream-shape-1` | Readable `destroyed` is an own property instead of Node's prototype getter/setter. |
| `stream-shape-2`, `stream-shape-3` | Writable `writableObjectMode` and its prototype descriptor are absent. |
| `stream-shape-4` | Writable `writable` has a different prototype descriptor, including configurability and setter presence. |

The stream differences have a separate [stream property surface plan](stream-property-surface.md).
The earlier sweep classified all 11 as pre-existing using release
`v14.0.ff84a70d.0` (2026-08-17); that old binary was **not rerun** here. Matching
probe IDs support continuity with the prior findings, not a fresh historical
A/B comparison. The v14 comparison is only valid for tests unchanged at its
tag; it was not valid for the rewritten native-addon fixture below.

## September 7 history

The initial pass used `13e340d5`. The first recorded full checkpoint used
`87077b56` plus the constructor-context implementation later committed in
`87c85b9c`: 569 JavaScript files, 563 passes, 2 failures, 3 intentional exits,
and 1 expected timeout. The evening rerun at `828a9b2f` plus the then-uncommitted
loader fix recorded 576 files, 572 passes, no unexpected failures, 3 intentional
exits, and 1 expected timeout. Both recorded 4221 specs across 102 files.
These historical pass totals used process status and did not separately
classify the stale exit-zero checks identified above.

| Resolved test or issue | Cause and resolution |
| --- | --- |
| `test_jit_for_of.cjs` | PR #95 broke integer-range locals across an OSR bailout resume; the nested for-of case returned 1065 instead of 2166. Fixed in `0bcab93b`; the case also moved into `examples/jit/bailout_resume.js` for harness coverage. |
| `test_websocket_client_buffered_frames.cjs` | Global `new.target` leaked from the WebSocket constructor into a native accept callback, producing a Socket with the WebSocket prototype and no `.on`. A `module.exports` data-property change exposed the leak by removing an incidental getter call that cleared the slot. Invocation state moved into JS/JIT frames; native callbacks receive `call_new_target`, native constructor frames root targets for GC, and accept explicitly selects the Socket prototype. Fixed in `87c85b9c`, with follow-ups in `1bfe480e` and `a7b0be86`; see the [constructor-context plan](../completed/net-connection-websocket-arg-regression.md). |
| `test_compile_native_addon.cjs` | PR #96 pre-created a CommonJS module using virtual `/$ant/...` metadata, which the materialized loader reused for `__dirname` and `__filename`; its helper then exited 127 at the virtual path. The fix creates metadata from the registered module record's real `resolved_path` while retaining virtual require-cache/module keys. `require.resolve` already mapped the materialized path. The original claim that this was pre-existing because it failed on v14 was wrong: the fixture had been rewritten after v14. Always build `ant-runtime` before trusting this test's pass. |
| `test_eval.cjs` | Sloppy direct eval did not leak `var` or function declarations into the caller. The implementation fix made the spec-faithful test from `fe9904d6` pass. |
| CLI file versus package script | `ant t.js` chose the package script even when `t.js` was a regular file. `87077b56` skips the script shortcut for an existing regular file. |
| Console rope and REPL tests | `87077b56` adjusted the rope fixture to the 32-character short-concat threshold introduced by PR #90 and allowed SGR escapes in the REPL prompt marker introduced by `671d8071`. |
| Obsolete debug channel and ESM fixtures | `87077b56` removed a test for the nonexistent `ANT_DEBUG=dump/errors:trace` channel; `fe9904d6` renamed the ESM-syntax FFI and RPC fixtures from `.cjs` to `.mjs`. |

The longer original checkpoint narrative is recoverable from this file at
`b1beeac2` before the September 13 document refresh.

## CI coverage

The current `build-platform` workflow runs selected cron/platform, upgrade,
native-stack, compile, and Temporal checks. It still does not run the complete
`tests/test_*` sweep, the JIT harness, or `examples/spec/run.js --all`.
The earlier statement that it ran only cron and upgrade tests is superseded.
