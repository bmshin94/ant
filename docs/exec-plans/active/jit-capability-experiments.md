# Generated-code capability experiments

Status: active
Date: 2026-09-16

Baseline: user commit 10bd09ae. Work stays in this checkout. No commits requested,
no implementation-review skill or subagents. Baseline binary/profile/score hash
are pinned under /tmp/ant-jit-capabilities-20260916.

## Sequence

1. Keep a bounded set of call targets and test guarded method-call inlining on
   DeltaBlue/Richards. Preserve generic fallback, actual closure/receiver context,
   and bail-before-effect constraints. Keep struct sizes unchanged where possible.
2. Investigate safe loop array-guard reuse in NavierStokes, including mutations,
   aliasing, effectful calls, OSR entries and fallback state.
3. Try persistent integer facts for reassigned locals in Crypto. Require bounded
   arithmetic and correct negative-zero/overflow behavior at every entry and join.

Verify findings against current source and generated code. Test each candidate
separately, record/discard regressions, then qualify retained changes across all
eight workloads and installed Node/V8. Preserve the frozen truthiness/Life gates.
Use serial ABBA/BAAB, binary/source/profile identities and sufficient repeats for
small differences. Diagnostic dumps are not timing measurements. Use standard
PGO only, with bench-v8 and Game of Life excluded from training. Audit abandoned
Ant processes. Follow focused tests with native/spec suites and preflight.

## Current implementation

- Call feedback retains up to four distinct targets, using the existing 32-row
  allocation and 16-byte entries. Method dispatch inlines eligible targets under
  a 384-byte aggregate bytecode budget. A new target at an already-specialized
  hot-tier site updates feedback and resumes at CALL, preserving preceding getter effects.
  Captured slots are refreshed before that bailout. Untrained sites and
  megamorphic/native calls keep their generic paths; this is not a full V8 PIC.
- Forward inline eligibility/emission share may-effect control-flow information.
  Mutually exclusive branches no longer inherit each other's effects. Numeric
  equality bails before coercion when treated as pure; unknown coercion remains
  effectful. Instruction-specific temporary names fix repeated INSERT2/INSERT3.
- Field-walking loops such as Packet.addTo get a label pre-pass and use
  non-restarting read helpers on every iteration. Numeric/general loops remain
  excluded. The one-pass empty-object reuse analysis still rejects all backedges.
- Array parameter identity and header proofs are compiler facts inside a
  straight-line region. Calls, effects, joins and argument replacement discard
  them; new values lose identity provenance. Bounds and element guards remain
  per access. This is reuse within a loop body, not loop-preheader hoisting.
- The OWN fallback follows the live IC index and actual inline capacity. The
  retained-shape fast path is unchanged. Both inline and overflow slot changes
  are exercised by generated native code.

## Experiments and findings

Initial target retention alone gave no clear DeltaBlue win. Allowing implemented
stack shuffles after effects exposed a duplicate MIR temporary name in Richards;
per-instruction naming corrected it and a repeated-assignment regression covers
it. The first field-loop trial moved Richards 6616→6723. Array-header reuse with
prefix increments moved NavierStokes 25017→25609 in an initial pair.

A bounded integer-range union prototype passed semantic/native tests but showed
no measurable Crypto win (13615→13654). It was removed; source and tests are
saved in /tmp/ant-jit-capabilities-20260916. It did not solve general loop range
inference or Crypto's parameter/limb representations.

Adding target learning and branch-sensitive inlining initially dropped
DeltaBlue 6513→5729. A 3-second sample showed get_field_inline self samples rise
from 23 to 302. Per-site counters found about 11M calls each for shared input/output
reader fields, almost all with matching live IC shape, epoch and activation.

The OWN fallback had baked the old slot index. EqualityConstraint and
ScaleConstraint initialize their fields in different orders; after retraining,
the shared reader's correct live IC could no longer satisfy that baked check.
A native regression fails against the old linked implementation and passes after
making the fallback follow the live index. The next initial pair recovered
DeltaBlue 6539→6671 and also moved Splay 8801→10402. Splay creates left/right as
own properties in varying orders after initializing them on its prototype; that
source pattern is consistent with the same slot-adaptation benefit. This is an
inference until a dedicated Splay ablation, not separate causal proof.

## Validation checkpoint

Focused regressions cover distinct-target bounds, closure/receiver context,
method getters during deoptimization, captured-slot mutation, branch effects,
field-loop getter effects/errors, argument replacement, array mutation/frozen
and COW storage, and native OWN fallback changes.

The standard PGO flow completed without training on bench-v8 or Game of Life.
The final binary passed all 18 native tests, 4240 specs, the three new runtime
regressions and array-literal COW coverage. The final harness run passed 244
entries; Rolldown alone failed because its react/jsx-runtime dependency could
not be resolved. The three new tests are included in the harness
manifest and also pass under Node. A focused pass of 18 inlining, array and missing-property regression
files also passes. Preflight passes. The stack-depth
sweep reported no rejected operand depths or JIT stack-overflow warnings, but
exited 2 for the intentional-error throw_stack and with_strict fixtures, so
that sweep is not a clean pass. Both fixtures also exit 1 in the final baseline
and candidate controls.

Final binary/profile/source hashes and serial comparison results are saved in
/tmp/ant-jit-capabilities-20260916/qualified. The final candidate SHA-256 is
68fc1c5be660408ababbee40a7cebe81cbb97602a316dfad09b3afeff53e730e.
Diagnostic instrumentation has been removed. No commits made.

## Fresh-PGO score comparison

This table records the final candidate, including the hot-tier restriction below.

Twenty fresh processes per Ant binary, ordered ABBA/BAAB in one serial run.
The V8 column is the median of two interleaved runs using installed
Node 26.8.1 / V8 14.6.202.34-node.28 with the identical score fixtures.
Higher scores are better. These are comparison measurements, not PGO inputs.

| Workload | Baseline 10bd09ae | Candidate | Node/V8 |
| --- | ---: | ---: | ---: |
| Richards | 6459 | 6517 | 75672 |
| DeltaBlue | 6430 | 6635 | 196301 |
| Crypto | 13490 | 13402 | 89699 |
| RayTrace | 12339 | 12425 | 146370 |
| EarleyBoyer | 14912 | 14900 | 152532 |
| RegExp | 7501 | 7539 | 21809 |
| Splay | 8760 | 9327 | 82436 |
| NavierStokes | 24241 | 24660 | 72681 |

The largest gains are DeltaBlue (+3.19%), Splay (+6.47%) and NavierStokes
(+1.73%). Crypto is 0.65% lower; EarleyBoyer is essentially level (-0.08%).
This is not a strict zero-regression result. The measurements do not isolate
individual optimizations after regenerated PGO. Earlier ablations and the
native OWN-fallback regression establish more specific mechanisms, but cannot
attribute the entire final score change.

Truthiness met the frozen Object/Array/String/Symbol targets at
28.58/28.46/33.12/30.35 ms per 10M iterations, all ahead of baseline.
Game of Life at tick 5000 measured tick L/A 0.296/0.337 ms and rendering
0.3825/0.4405 ms; average differences from baseline were +0.005/+0.003 ms.
Fixed-work checksums matched. Splay used about 835 MB peak RSS for either binary.

The original field benchmark was repeated twenty times per binary. Per 10M
reads: normal 24.6→24.9 ms, missing 39.0→39.3, null 17.6→17.6,
present 27.3→27.2, undefined 19.0→18.2. Sample ranges overlap. The
missing-parameter benchmark had small median differences (0.17–1.33 ms per
fixture run), with four processes per binary; these were not expanded further.

## Startup follow-up

Twenty yt-dlp runs per binary showed 2.496→2.532 seconds. Compilation traces
isolated eight additional cheap-tier compilations of AST handlers, with reported
compile time 903→952 ms. Target discovery was recompiling short-lived callers
before the wider inline sets could repay that cost.

Restricting that new learning/recompilation path to the hot tier removes all
eight additional compilations (136 total, with matching tier counts). The
cheap tier retains ordinary fallback for unseen targets. Focused regressions
pass, and initial comparisons retain Richards/DeltaBlue/Splay/NavierStokes
gains. The standard PGO rebuild and qualification are now complete. Final
traces still show 136 compilations for both binaries, with reported compile
time about 911→923 ms. Clean twenty-run yt-dlp medians are 2.488→2.507
seconds: the remaining 18 ms difference is not fully isolated. Ink completed
47 tests in five seconds in every run (four per binary).

Twenty short-server runs per binary used the fixed baseline load generator
and ANT_TEST_BIN to select the server. Eight-second cooldowns separate runs.
Oha used four runs per binary. Rates below are median requests per second.

| Workload | Baseline | Candidate |
| --- | ---: | ---: |
| servers: hono | 9670.5 | 9488.0 |
| servers: express | 2641.5 | 2612.0 |
| servers: h3 | 3741.5 | 3706.0 |
| servers: elysia | 8880.5 | 8850.0 |
| oha: hono rps | 49854.5 | 50207.5 |
| oha: express rps | 25183.0 | 25143.0 |
| oha: h3 rps | 29082.5 | 28908.5 |
| oha: elysia rps | 87347.5 | 88153.5 |

These results leave small startup/server losses and do not satisfy a strict
zero-regression gate. No further speculative tuning was retained. The tree is
uncommitted so the experiment remains reviewable. The first candidate and its
diagnostic traces remain under /tmp/ant-jit-capabilities-20260916/final;
current binary identities, all samples and the full report are under
/tmp/ant-jit-capabilities-20260916/qualified/report.md. Runtime source and the
PGO profile match the tested binary; only these notes changed after pinning.
