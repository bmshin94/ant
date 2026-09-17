# Silver SSA optimizing-tier experiment

Status: active
Date: 2026-09-16

## Objective and scope

Implement and measure the requested connected compiler work: JavaScript-aware
SSA, nested polymorphic inlining, persistent memory/type facts, whole-loop
analysis, and integer/floating-point representation selection. MIR remains the
native backend. A 5x gain is a target, not an established result. This is a new
tier, not another extension of the linear emitter's local fact tracking.

Work stays in this checkout. Baseline is user commit a1ba1567; the executable,
profile and score-file hashes are pinned in /tmp/ant-ssa-tier-20260916/baseline.json.
No commits, implementation-review skill, subagents, or master builds. Preserve
the existing JIT as fallback. Use standard PGO only; bench-v8 and Game of Life
are validation holdouts. Run timed workloads serially and audit stray processes.

## Design boundaries

- A control-flow graph owns SSA values, merge/loop phis, explicit memory effects,
  typed representations, and interpreter frame-state recipes. Graph verification
  precedes MIR lowering. Resource/unsupported-bytecode failures fall back before
  publishing any partial code.
- Inlining imports complete eligible control-flow graphs, including loops and
  direct/method dispatch. Each imported frame retains its actual closure and
  receiver. Budgets bound recursion, graph size and polymorphism. Frame-state
  chains resume after earlier effects instead of restarting an entire callee.
- Field/cell knowledge is tied to value identity and effects. Calls and possibly
  aliasing stores invalidate facts. Retained shapes and epoch dependencies must
  obey existing IC lifetime rules; pointer equality alone is not a dependency.
- Natural-loop analysis supplies induction/range information and preheaders.
  Hoisted guards deoptimize to an entry state, including zero-trip loops. OSR
  must establish the same proofs and representations as ordinary entry.
- Phi representation selection reaches a fixpoint across backedges. Checked
  conversions preserve overflow, negative zero, NaN and non-number semantics.
  Interpreter reconstruction always receives canonical boxed values.
- Captured local frames, exceptions, async/generators and dynamic eval require
  explicit support; unsupported cases use the existing compiler. No claim of
  general language coverage may be inferred from the initial supported subset.

## Implementation and validation checkpoints

1. CFG/SSA builder, verifier, frame-state format and native analysis tests.
2. MIR lowering and precise nested-frame interpreter resumption; prove that
   getters, writes and calls are not replayed on guard failure.
3. Nested/polymorphic inlining, simplification and effect-aware load/guard reuse.
4. Loop induction/bounds analysis, OSR entry proof reconstruction and phi
   representation selection, with overflow and mutation regressions.
5. Generated-code evidence that the new tier is exercised by the target
   functions, followed by serial pinned comparisons on all eight workloads
   against baseline and installed V8. Check frozen truthiness/Life, startup and
   server workloads. Fresh PGO and broad correctness validation precede any
   claim that a candidate is ready to keep.

## Current checkpoint

All five mechanisms are connected in the experimental tier, enabled only by
`ANT_JIT_SSA=1`. Default compilation and tier counters remain unchanged.

- CFG SSA has loop/merge phis, verified bytecode boundaries, operand depths,
  dominance, same-block ordering and complete parent frame-state chains.
  OSR is an explicit CFG edge before phi simplification. Native entry validates
  the offset and imports current parameters, locals and the operand stack.
- The importer handles nested direct/method calls, multiple observed targets,
  loops and multiple returns. Target guards preserve closure/receiver identity.
  Misses resume before the call. Constant arithmetic, constant branches, trivial
  phis and common pure expressions are simplified after import.
- Own-data shape assumptions are retained independently of mutable IC entries,
  with explicit invalidation guards. Guard and load availability intersects at
  joins and is invalidated by possibly aliasing calls/stores. Captured cell
  values can be hoisted only through loops that cannot modify them.
- Natural-loop analysis creates normal and OSR preheaders. Dense-array feedback
  is required before specializing a receiver. Eligible loops reuse array data
  and length; simple increasing-index loops get checked entry ranges and can
  omit repeated bounds/index checks. Holes still leave the fast path.
- Number, word-integer, bounded induction-integer and Boolean representations
  survive joins and backedges. Edge/OSR conversions preserve non-number,
  overflow and negative-zero behavior. Native metadata pointers have a distinct
  representation and cannot enter interpreter snapshots.

Numeric use propagates through phi chains without assigning a numeric type to
the original boxed input. Incoming edges check uncertain values before filling
unboxed phis. This handles parameter-initialized recurrences as well as constant
initializers; guards on OSR edges remain mandatory. Floating-point operations
use existing floating-point operands directly instead of copying them through
shared temporary registers.

Untrained numeric array reads collect bounded existing feedback. Once a site
settles, a learning exit permits recompilation; a real speculative failure
records the failed feedback version and permits the ordinary tier to take over.
These exits occur before stores or user-observable effects. Abstract equality
handles object identity directly and declines effectful coercion before it runs;
strict equality handles numbers and identity inline, using helpers for string
and BigInt value comparison.

Nested deopt restores actual closures, receivers, original argument counts,
mutable parameters, locals and operand stacks. The native test proves that a
three-frame chain first runs inline without bailing, then resumes after a leaf
write without repeating it. Async/generator/eval bytecodes requiring unsupported
frame state are declined. Ordinary global opcodes retain their normal global
lookup semantics; eval-aware opcodes are distinct and remain unsupported here.

Compiled code roots closures in initialized borrowed buffers on the existing
root chain. Call argument storage shares that registered buffer and is cleared
after use. Deopt-only buffers are allocated on the cold path and freed after
resumption, so unused large stack buffers cannot retain stale heap pointers.
`sv_func_t` did not grow. The optional function sidecar records an SSA rejection
version so a speculative miss can fall back to the ordinary JIT with the same
feedback instead of accidentally disabling JIT altogether.

## Qualification findings

The initial full-workload run exposed a Splay MIR crash and severe Crypto and
Richards losses. No performance acceptance was claimed from focused tests.

- The Splay crash was an unreachable fallback edge whose representation guard
  had no deopt state. Inline misses now terminate through deopt; lowering also
  rejects a required guard without a snapshot before opening a MIR module.
- Crypto's first suite was about 5.2k, but subsequent sequential suites recovered
  to about 13.0k, matching the warmed baseline. Sampling and compile timings
  identified compile work spilling into measurement: `bnpExp` expanded to about
  80k MIR instructions / 1.34 seconds. Inlining is now charged for the entire
  reconstruction chain, not just bytecode length. The current cost limit is
  8,000 units. Final repeated Crypto results remain about 2.3% below the
  same-binary SSA-off control; this is not a performance acceptance.
- Unused MIR register declarations and unnecessary deopt blocks were removed.
  Known Boolean conditions use direct branches rather than generic truthiness.
- Array specialization previously happened without sufficient receiver feedback;
  it now requires the existing dense-numeric specialization evidence.
- The full harness exposed a scalar nested-loop regression: `bench_dec` took
  about 1,215 ms rather than 215 ms. Generated MIR kept parameter-initialized
  recurrences boxed. Checked numeric phi inputs removed repeated boxing and
  guards; direct floating-point operands reduced further overhead. Development
  timing reached about 266 ms, still slower than baseline. Extending the old
  numeric-unrolling pass to SSA labels made it worse (292 versus 266 ms in
  ABBA/BAAB), so that extension was removed. The ordinary unroller is unchanged.

Final standard PGO training ran with `ANT_JIT_SSA=1`; the script was unmodified
and neither bench-v8 nor Game of Life was included in training. Source hashes
were checked before and after training. No training failure, timeout, corrupt
profile or runtime profile hash mismatch was reported.

Validation on that binary:

- All 19 Meson tests pass, including native graph/codegen/deopt/root tests.
- The SSA JS regression passes and verifies actual SSA module emission.
- All 4,240 specs in 102 files pass with SSA both enabled and disabled.
- The full harness reports 244 passed / 1 failed in each mode. The sole failure
  is Rolldown's missing `react/jsx-runtime` dependency; the saved baseline also
  reproduces it. The four sustained-server gates pass in both modes.
- A temporary harness run used `NO_COLOR=1`, which the installed oha rejects.
  Final validation uses `NO_COLOR=true`; those environment failures are not
  attributed to the runtime.

## Final repeated workload scores

Higher is better. B is the pinned pre-change Ant; O and S are the exact same
new binary with SSA disabled/enabled. V8 is installed Node 26.8.1, V8
14.6.202.34-node.28. Each workload ran serially in `BSSBSBBSOSSOVV` order: four
baseline, six SSA-on, two SSA-off and two V8 samples. This includes baseline
ABBA/BAAB and a same-binary OSSO control. Scores below are medians; raw samples,
ranges, timestamps, load averages and identities are in the artifact directory.

| Workload | Baseline B | New, SSA off O | New, SSA on S | Node/V8 | S vs O |
| --- | ---: | ---: | ---: | ---: | ---: |
| Richards | 6,510 | 6,550 | 6,499 | 75,800 | -0.78% |
| DeltaBlue | 6,615 | 6,688 | 6,658 | 193,828 | -0.44% |
| Crypto | 13,520 | 13,455 | 13,147 | 90,709 | -2.29% |
| RayTrace | 12,431 | 12,413 | 12,653 | 146,851 | +1.93% |
| EarleyBoyer | 14,881 | 14,968 | 15,013 | 153,049 | +0.30% |
| RegExp | 7,629 | 7,558 | 7,527 | 21,898 | -0.40% |
| Splay | 9,661 | 10,202 | 10,040 | 83,153 | -1.59% |
| NavierStokes | 24,709 | 24,610 | 25,228 | 72,569 | +2.51% |

This does not establish a large overall gain or a zero-regression result. In
particular, Splay's improvement over B also occurs with SSA disabled and must
not be attributed to SSA. Crypto and the scalar loop remain losses. This tier
is an opt-in experiment, not ready to enable by default.

## Additional non-regression checks

These compare the saved baseline against final SSA-on, serially in ABBA/BAAB
order, four process samples each. Durations are medians; lower is better.
The standard PGO script includes `tests/bench_*`, so those microbenchmarks are
not unseen training holdouts. Bench-v8 and Game of Life remain excluded.

| Case | Baseline | SSA on |
| --- | ---: | ---: |
| Object truthiness, 10M | 27.52 ms | 23.79 ms |
| Array truthiness, 10M | 28.33 ms | 25.02 ms |
| String truthiness, 10M | 32.97 ms | 29.62 ms |
| Symbol truthiness, 10M | 30.00 ms | 27.35 ms |
| Optional present, 10M | 25 ms | 23 ms |
| Optional missing, 10M | 40 ms | 36 ms |
| Optional null, 10M | 17.5 ms | 17.5 ms |
| Optional undefined, 10M | 20 ms | 17 ms |
| Normal field, 10M | 24 ms | 19 ms |
| bench_dec | 211.30 ms | 273.40 ms |
| yt-dlp wall time | 2,469.87 ms | 2,494.19 ms |
| Ink first render | 110.22 ms | 109.90 ms |

Ink completes 47 tests in each five-second run; yt-dlp's output hash is identical
across all runs. The latter has a small 24.32 ms increase, while the scalar loop
has a clear 62.11 ms loss. Neither is hidden by the successful correctness gates.

At Life tick 5,000, original-fixture median world tick L/A is 0.2945/0.3365 ms
baseline and 0.2950/0.3365 ms SSA-on. Rendering is 0.3830/0.4385 versus
0.3805/0.4385 ms. The deterministic fixture's tick average changes from 0.33238
to 0.33398 ms and render average from 0.43766 to 0.43960 ms. Every deterministic
world/checksum/rendered-length check matches the saved Node reference. These
small timing changes do not establish a material Life regression.

The same-binary controls also retain the two application losses: SSA-off/on
`bench_dec` medians are 204.63/265.17 ms, and yt-dlp is 2,420.85/2,453.11 ms.
Thus the scalar loss is caused by enabling this tier, not solely by retraining.

Server comparisons use the saved baseline for the HTTP load generator in both
variants and change only `ANT_TEST_BIN`. Short runs use ABBA/BAAB; sustained oha
uses ABBA, with eight seconds between suites to avoid port exhaustion. Medians
are requests/second, so higher is better:

| Server | Short baseline | Short SSA | Sustained baseline | Sustained SSA |
| --- | ---: | ---: | ---: | ---: |
| Hono | 8,784.5 | 8,651 | 50,748 | 50,188.5 |
| Express | 2,621 | 2,632 | 25,218 | 25,356.5 |
| h3 | 3,700 | 3,673.5 | 28,999 | 28,918 |
| Elysia | 8,877 | 8,719.5 | 89,506 | 88,548 |

The small mixed changes do not establish a server improvement. All requests and
harness checks completed successfully; logs retain each sample and its range.

## What still limits the experiment

Fresh diagnostic dumps confirm that the tier is exercised rather than simply
declining every target. DeltaBlue includes a graph with nine imported calls and
14 removed loads; Richards includes seven imported calls and 29 removed loads.
However, call-target guards and unsupported element stores still trigger real
deoptimizations in those workloads, returning execution to the ordinary tier.
This limits how much work benefits from the larger inlined graphs. These dumps
are evidence of generated structure and exits, not native instruction counts or
timing profiles. NavierStokes also shows bounded learning exits followed by
specialized recompilation, which are distinct from those real guard failures.

Further work must address sustained coverage and generated-code cost, not raise
inline budgets blindly. General unsupported bytecode coverage, richer bounds
proofs, smaller reconstruction code, and preserving useful specialization after
call-target changes remain architectural work. The existing tier counters and
default compilation policy have not been changed.

Candidate SHA-256:
`320c6e6928cadebb2f9a95eb3ac3d9f55a87e1ad283b872f6e6130222d4a4fef`.
Profile SHA-256:
`c5acf8cde2475bcaf2278f861d709bef33cc4c6ae14f0de6522145694f89d7e8`.
Final artifacts: `/tmp/ant-ssa-tier-20260916/qualified/`. Earlier binaries and
logs remain in sibling directories and are not the final candidate.

Artifacts are under `/tmp/ant-ssa-tier-20260916/`. The first comparison covered
all eight workloads and pinned installed Node/V8 as well as Ant. The initial
`crypto-*.sample` experiment accidentally started overlapping suite runs and is
not causal evidence; use the single-suite `crypto-decrypt-*.sample`, sequential
suite logs, generated MIR and compile-time logs instead.

## Current limits

This does not claim full Maglev-level coverage or a 5x result. Unsupported
bytecodes retain the ordinary JIT. Inlining is bounded by recursion, code size,
frame count and reconstruction cost. Bounds elimination currently covers simple
increasing indices; arbitrary affine/multidimensional index proofs are not
implemented. Shape dependencies are guarded retained descriptors, not a new
runtime watcher system. No background compilation or native stack-map collector
is introduced. Nothing is committed or enabled by default.

A coercion test also exposed a baseline prefix-decrement bug: after warming a
nested loop with numeric parameters, a string initial value produces `0555`
instead of 360, and an object value converts three times instead of six. The
saved pre-change binary and the same binary with SSA off both reproduce it;
`sv_op_dec`/`sv_op_dec_local` reinterpret values with `tod` without ToNumeric.
This existing interpreter issue is outside this tier change. The new numeric
edge/deopt test uses explicit subtraction to verify six coercions without
depending on that broken prefix operation. Reproducer and baseline/Node evidence:
`/tmp/ant-ssa-tier-20260916/strict-final/phi-coercion.cjs` and its logs.

Reference algorithms: V8 Maglev SSA/phi/frame-state design
(https://v8.dev/blog/maglev), and the existing /tmp/v8 shallow checkout. No V8
implementation is copied into Ant; heap, bytecode and deoptimization contracts
are Ant-specific.
