# Generational throughput and allocation pressure

Status: active
Date: 2026-09-16

## Scope and constraints

Implement in this worktree: allocation-site pretenuring, delayed young promotion,
measured GC/allocation-cost heap growth, shared constant-array storage, smaller
object storage, and parallel/concurrent GC work. Measure independent changes
before composing them. Preserve existing scanner and kqueue patches. Do not
train PGO on bench-v8 or Game of Life. No commits requested.

## Sequence

1. Allocation-site feedback and pretenuring for object/array literals. No larger
   sv_func_t or per-object pointer. Samples must not become conservative roots.
   Old allocations require list/count and initial-reference barriers.
2. Retain first survivors young; preserve remembered edges across collections,
   correctly count promotions, and test mixed old/young graphs.
3. GC-cost/allocation-throughput-based major budget, anchored at major GC;
   isolate-owned state, bounded growth and explicit memory-pressure behavior.
4. Share immutable constant array backing with safe detach on every mutation
   path and ownership independent of literal-template lifetime.
5. Reduce common object storage by moving rare state out of the hot allocation.
6. Parallel/concurrent GC only over work with explicit ownership and synchronization;
   correctness first for mutator races, finalizers, weak references and shutdown.

## Validation

Pin initial worktree binary in /tmp/ant-generational-work-20260916. Native
regressions for feedback, lifetime, barriers and promotion; specs, harness, GC
stress; clean serial ABBA/BAAB Splay and broader workloads. Fresh final-source
PGO before final performance qualification. Audit abandoned processes.

## Initial implementation checkpoint

All six areas have initial implementations in this worktree; none is finally
performance-qualified. No changes committed.

- Literal-site feedback uses weak bounded samples and periodic young probes;
  old initialization/list/count and normal store barriers are covered natively.
- Two-survival object promotion is adaptive: enabled for low-survival mixes.
  Always delaying promotion regressed Splay (~8.3k->6.5k) and was replaced.
  Remembered objects are pruned while non-object records persist when young
  survivors remain; native tests cover old-owner and promotion-grandchild edges.
- Major budgeting uses measured GC and managed-allocation rates, a 97% mutator
  target and bounded 1.1x-4x growth, anchored at a major. Policy is isolate-owned.
  The first isolated policy test was slower; final fresh-PGO evaluation is pending.
- Constant immediate array literals share code-arena backing, detach on writes,
  and skip tracing their non-reference elements. JIT writes guard COW. Mutation,
  callbacks, deletion, descriptors, length changes and dynamic literals tested.
  10000 ten-element literal arrays use80 shared bytes plus131072 holder bytes.
- ant_object_t is128 bytes (was152): exotic hooks move to existing sidecars,
  Promise state uses spare union space while preserving SLOT_DATA, flags occupy
  former native-entry padding. Native layout/brand test passes. sv_func_t and
  sv_ic_entry_t layouts did not change.
- Concurrent reclamation is limited to unreachable private array buffers.
  A bounded8MiB queue, batched frees, synchronous failure/Wasm fallback, explicit
  drain and shutdown join keep object graphs/finalizers/shapes on the owner.
  This is not concurrent marking or a fully parallel collector.

Combined checks so far:4240 specs pass; harness241 pass plus the pre-existing
Rolldown missing react/jsx-runtime failure; allocation-site/aging/COW native
checks, exact edges, kqueue regression and object-layout checks pass. Full native
suite and fresh standard PGO, followed by broad serial comparisons, remain.
Pretenuring alone initially improved Splay~7.8k->8.3k, but always-aging and the
initial tight budget regressed it. Combined pre-retrain scores are still below
baseline; do not claim a general win from partial results or stale profile data.
Artifacts: /tmp/ant-generational-work-20260916; all phase binaries are pinned.


### First fresh-PGO qualification and regression isolation

Standard PGO completed cleanly (no training failures/runtime profile mismatch).
Fresh candidate passed18 native tests,4240 specs, COW regression, and241 harness
cases; only known Rolldown dependency failure remains. Short ABBA benchmark-v8
comparison: Richards-0.14%, DeltaBlue+0.68%, Crypto-0.39%, RayTrace+4.46%,
EarleyBoyer-46.02%, RegExp-0.20%, Splay+5.33%, NavierStokes-0.70%.
This candidate is NOT qualified. Remaining broad comparisons paused for the
large Earley/Boyer regression. Binaries/results in fresh/ are immutable.

Production sampling and disassembly locate the new cost in minor sweep/free,
not speculative JIT changes. An archived diagnostic binary independently toggled
aging and pre-minor pressure-triggered majors. Fixed1000 Earley/40 Boyer runs:
current1870/1467ms, no-aging1375/725ms, pressure-triggered majors1152/728ms,
both1153/692ms. Counts are more important than diagnostic timings: current Earley
588 minors, no majors, median~12504 remembered upvalues; periodic majors reduce
it to265 minors/33 majors and~162 upvalues. Original production source/binary
were restored after the diagnostic run.

Cause: delayed survivors kept coarse remembered owners until a major, while
post-minor recovered headroom could keep deferring majors. The current fix bounds
those owner records' age using the existing minor-count budget; transient arrays
with no remaining remembered-owner debt still avoid an unnecessary major. An
explicit native regression covers both cases. Rare-state free-path checks now
use the sidecar-owner pointer/type instead of redundantly dereferencing absent
exotic/Promise state (the sampled hot free loop). This fix is being built and
must be requalified/retrained; not a final performance claim.


### Second fresh-PGO qualification

The owner-debt bound and cached arena capacity are included in a second clean
standard PGO build. Immutable binaries, profile hashes and logs are in
`/tmp/ant-generational-work-20260916/fresh2/`. All 18 native tests, 4240 specs,
GC stress and strengthened COW tests pass. Harness: 241 pass, known Rolldown
missing dependency failure only. Preflight passes. Stack-depth sweep reports
no rejected functions or JIT stack overflows, but exits 2 for the intentionally
throwing `test_throw_stack.cjs` and strict-mode syntax-error
`test_with_strict.cjs`; coverage is reported incomplete, not green.

The additional separate-receiver Reflect.set array check exposed a baseline bug:
both saved baseline and candidate mutate the target instead of the receiver.
It is excluded from COW acceptance, with diagnostic logs and fixture under
`/tmp/ant-generational-work-20260916/cow-*`. Hot postfix/read-then-write and
reference-array mutation comparisons remain in the COW regression test.
Serial broad performance qualification is in progress; no final win claimed.


### Cost of debt-only major collections

Second fresh-PGO broad results: Earley/Boyer +18.5%, Splay -1.1% after 20 runs
per binary; all 18 truthiness medians level/lower after 20 runs. Game of Life
held steady or improved. yt-dlp regressed 2.67->2.84s. Socket exhaustion invalidated
one short-server round; its log was preserved and the round rerun after cooldown.
No zero-regression qualification claimed.

Scored-phase Splay samples still show roughly 47-48% GC in both binaries:
less scanning/array allocation is offset by sweeping/object allocation. A
tracking-before-shape initialization reorder had no measured benefit in its
initial comparison and was reverted. A longer diagnostic Splay fixture has a
different heap trajectory and is not used to attribute the scored result.

yt-dlp sampling isolated major sweeping. Temporary diagnostics: 82 minors,
9 majors costing617ms with the fixed minor-count debt trigger; disabling only
that trigger gave82 minors,1 major costing116ms, identical output,2.82->2.33s.
Repeated majors retained almost all old AST nodes. Minor intervals cost6-14ms,
while majors grew19-141ms. Diagnostic source/binary saved under ytdlp-diagnostic/;
production source restored before the correction.

The debt trigger now additionally requires accumulated minor pause time to reach
the previous major's duration. Independent heap/pool/closure pressure is unchanged.
This is a measured-work bound, not a fixed owner-age guarantee. Native tests cover
cheap-minor deferral, eventual debt collection and rooted-child survival. Initial
native/COW tests pass; fresh PGO and renewed broad qualification remain pending.


### Allocation-budget arithmetic experiment

Generated `jit_helper_object` for the cost-gated build evaluates about26 budget
instructions before every allocation. A cached allocation checkpoint reduces
that gate to two loads, compare and branch. The first implementation forced full
GC-policy polling every1024 allocations and regressed Splay (~7.7k->6.7k); it is
not retained. It changed collection cadence despite reducing allocator work.

The narrower experiment caches only below-pressure budget arithmetic. Its limit
is the minimum of the current pressure threshold, live+1024 (saturated), and the
arena ceiling. At the checkpoint it re-evaluates the current threshold and calls
`gc_maybe` only if pressure is reached, retaining the old per-allocation tick
cadence above pressure. Every completed collection refreshes against the new
live count. Pool growth can be observed up to1024 object allocations later;
this is not a byte-overshoot bound. Independent pressure hooks remain.

Template roots, disabled collection, and unchanged below-pressure timer ticks
are covered natively; native/COW tests pass. Read-only review found no concrete
safety/cadence issue. Comparison is pending in allocation-cache/ against the
pinned cost-gated build. The prior broad runner was stopped with completed rows
saved in final/ and can resume if this experiment is rejected.


The narrower allocation cache also showed no useful initial Splay gain and was
reverted. Both experiment sources/binaries/results remain outside the worktree
under allocation-check/ and allocation-cache/. The restored production source
matches the saved cost-gated source; a forced rebuild plus the normal codesign
target produced a binary byte-identical to final/ant (acb2c4753b771224...). The
native GC test passes after restoration. The saved broad run is resuming from
completed rows. No allocation-check cache or diagnostic tracing is retained.


### Shared-array input materialization

Retained a direct completion of COW literal emission: when immutable shared
backing already exists for the exact literal length, OP_ARRAY passes that stable
pointer to the existing helper and pops its virtual operands without boxing or
copying them into the temporary argument buffer. Untrained/dynamic sites retain
the original path. No new runtime state or GC scheduling changes. Read-only
review verified code-arena lifetime and snapshot/root independence.

COW and dynamic/holey JIT array tests pass, including hot mixed-immediate literals.
Generated MIR confirms the factory uses the shared-pointer operand without the
array-input buffer stores. Focused500k-array median:6.58->6.03ms for constants,
12.48->12.60ms dynamic control (short initial comparison). Scored Splay+0.58%
in four samples per binary is not a strong macro claim; fixed1000-work RSS is
unchanged (~837MB) against the prior candidate. Standard PGO regeneration and
final qualification follow. Artifacts: cow-input-elision/.


## Final validation and measured limits

The retained source is built with fresh standard PGO. No commits were made.
Final artifacts: `/tmp/ant-generational-work-20260916/final-cow/` (binary/profile/source hashes, raw logs, ranges and sample counts in comparison.json).
The baseline is the original worktree binary saved before this all-six task, not a rebuilt master or installed Ant.

| Benchmark | Saved baseline | Candidate | Score change |
| --- | ---: | ---: | ---: |
| Richards | 6466 | 6457 | -0.14% |
| DeltaBlue | 6321 | 6407 | +1.36% |
| Crypto | 13555 | 13579 | +0.18% |
| RayTrace | 12216 | 12585 | +3.02% |
| EarleyBoyer | 12475 | 14875 | +19.24% |
| RegExp | 7546 | 7505 | -0.55% |
| Splay | 7823 | 7725 | -1.25% |
| NavierStokes | 24499 | 24401 | -0.40% |

Splay used20 runs per binary. Other final benchmark-v8 rows used4 per binary.
20 truthiness runs per binary leave Object/Array/String/Symbol at27.37/27.93/32.84/29.93ms per10M; all remain within their accepted targets. Earlier short-pass losses largely disappeared with repeats.
Final field/parameter controls used4 runs per binary: ordinary field24.2->24.2ms, optional missing39.0->38.5ms, optional present27.0->26.2ms per10M. Small null/parameter differences remain in the raw table.

yt-dlp:2.624->2.413s (-8.0%,4 runs per binary), identical output hashes. Ink:47 displays in5s for both; first-render medians roughly0.106/0.105s.
Game of Life checksums match; original rendering average0.438->0.429ms, world tick average0.333->0.334ms at5000 ticks. bench_dec207.57->207.96ms.
Short-server medians are lower by about1-3%; oha medians lower by about0.2-1.2%. These are4-run-per-binary comparisons, not proof of a universal regression magnitude.
Fixed1000-iteration Splay peak RSS:1.182->0.837GB (-29.2%,2 runs per binary); elapsed1.140->1.110s. Do not generalize this to every workload or substitute this fixture for the scored suite.

Final correctness:18 Meson tests pass,4240 specs/102 files pass, harness241 pass plus the known Rolldown missing react/jsx-runtime failure. COW/mixed-immediate/dynamic/holey-array checks and GC stress pass. Final stack-depth sweep reports no rejected functions or JIT overflows; it exits2 for the same intentional throw/strict-mode syntax-error fixtures that also exit1 on baseline. Thus its own status is coverage-incomplete, not a clean pass. Final preflight passes and the process audit finds no abandoned Ant processes.

**Not a zero-regression performance pass:** Splay is still below9k and slightly slower than the saved baseline; small server losses remain. All six areas are implemented and correctness-checked, but the performance goal remains open. Background work is limited to freeing unreachable private array buffers; marking and graph mutation stay on the isolate thread.
