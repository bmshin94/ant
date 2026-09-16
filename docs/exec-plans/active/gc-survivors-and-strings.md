# Survivor aging and literal string pretenuring

Status: active
Date: 2026-09-16

Baseline: user experiment commit 71158b93. Work is authorized in this checkout;
no commits requested. Pinned binary, profile and measurements live in
/tmp/ant-gc-completion-20260916. Preserve the user's benchmark score file.

## Scope

Complete the existing adaptive second-survival policy for closures, captured
cells and rope blocks. Remembered owners must remain valid while any referenced
heap kind stays young. Closure/cell age fits existing padding. Rope survivor
blocks must be separate from allocation blocks so new allocations do not inherit
an older cohort's age.

Investigate propagating learned object-literal pretenuring to concatenations
inside its initializer. Allocation must choose the destination before exposing
the rope; an old rope cannot hide a young child from minor marking. Prefer a
bounded propagation path with a conservative young fallback.

## Validation

Add native lifetime/barrier regressions, rebuild and run focused tests, then all
specs and native tests. Compare all eight bench-v8 workloads against the pinned
commit and installed Node/V8 using identical fixtures and serial alternation.
Diagnostic collection counts are separate from untraced timing. Use only the
standard PGO script; bench-v8 and Game of Life remain holdouts. Audit abandoned
Ant processes. Do not retain changes merely because a microbenchmark improves.

## Current checkpoint

Closures and cells now carry age in existing padding; both keep their young
roster entries after a first survival when the existing low-survival policy is
active. Promoted owners retain younger references. Rope first-survival blocks
move into a sealed survivor pool and promote on their next survival. Remembered
sets clear only when no relevant young survivors remain. Major marking, pool
accounting, conservative fallback and shutdown include the survivor pool.

Shaped literal sites store their initializer end in existing padding. String
concatenations in the lexical initializer inherit the innermost site's live
pretenuring decision in the interpreter and JIT. This is a lifetime heuristic,
not full escape analysis: an intermediate string in an initializer may be short
lived. The immutable rope path allocates old only when both rope children are
already old; otherwise it stays young. Builder operands flatten on the runtime
path. General unshaped literals and called functions do not inherit the hint.
No old-to-young rope remembered set is needed under this allocation invariant.

Native regressions cover survival, second-cycle death, promotion barriers, pool
accounting, major tracing of old ropes, and enabling/disabling hints after JIT
compilation. The interpreted control explicitly disables JIT eligibility;
`debugger` alone does not do so. Closure/cell/site-cache sizes remain 128/40/72
bytes on 64-bit builds, asserted by the test.

Initial untraced all-eight AVBBVA comparisons (two processes per engine): aging
alone reduced Splay from 7668 to 6614. Combined with string propagation, Splay
reached 8997 versus 7774 baseline. Other combined results were Richards
6480/6508, DeltaBlue 6410/6483, Crypto 13638/13566, RayTrace 12290/12474,
EarleyBoyer 15383/15330, RegExp 7678/7641, NavierStokes 24733/24672
(candidate/baseline). These use the previous PGO and are not final qualification.

Fixed-work traces over all eight show the Splay mechanism: 22 minor/3 major
collections become 6/3, diagnostic GC time about 423 ms becomes 285 ms. Peak tracked
objects are 4.47M/4.52M. Other workloads' counts stay broadly unchanged. Tracing
has been removed. 18 native tests and 4240 specs passed before the final
remembered-set cleanup refinement; focused native coverage passes afterwards.
Fresh standard PGO and final validation are complete; results follow. The initial aging-only number above is from the initial prototype, before remembered-set cleanup was refined, and is not a final isolated ablation.


## Final qualification

Baseline is commit 71158b93, binary bbf1f194aec25c302901c8db4c12e6bfa8657506a998e855b413b636c9537c5e.
Candidate is 8fbd29b550d52668c4aabcf99fb8ad20322461fbc2987396a46791a0d430b11b,
built with the unmodified standard PGO script. Training had no benchmark-v8 or
Game-of-Life inputs, reported no failed/time-out training cases, and completed
both build stages. Node 26.8.1 uses V8 14.6.202.34-node.28.

Scores below use identical pinned fixtures. RayTrace, EarleyBoyer and Splay have
20 processes per Ant binary in alternating ABBA/BAAB blocks. Other Ant rows and
all Node rows have two processes per engine in AVBBVA order. Small differences
in the two-process rows are not established wins or regressions. The apparent
RayTrace/EarleyBoyer losses in the first pair disappeared in the 20-run repeats.

| Workload | Committed baseline | Candidate | Node/V8 |
| --- | ---: | ---: | ---: |
| richards | 6478 | 6439 | 74646 |
| deltablue | 6410 | 6404 | 205911 |
| crypto | 13437 | 13394 | 90446 |
| raytrace | 12580 | 12659 | 145926 |
| earley-boyer | 15003 | 15083 | 152873 |
| regexp | 7467 | 7493 | 21785 |
| splay | 7703 | 8870 | 82489 |
| navier-stokes | 24141 | 24105 | 72864 |

Splay's repeated median improves 15.16%, reaching 8870 rather than a 9000 median.
Peak RSS in a separate 1000-iteration fixed-work ABBA is 798.10 MiB baseline and
795.73 MiB candidate. The earlier diagnostic counter comparison (before fresh PGO
and the final remembered-set cleanup refinement) was 22 minor/3 major versus 6/3;
those diagnostic timings are not the clean benchmark timings.

Truthiness was repeated 20 times per binary, 30M iterations per sample, with the
same warmup order. Medians below are normalized to 10M iterations. All four frozen
targets remain met; this does not assert every individual timing is lower.

| Type | Baseline ms | Candidate ms | Accepted target ms |
| --- | ---: | ---: | ---: |
| object | 27.523 | 27.591 | 30 |
| array | 27.973 | 28.188 | 30 |
| string | 33.217 | 32.783 | 40 |
| symbol | 30.120 | 29.853 | 40 |

Game of Life reaches tick 5000 with identical fixed-world checksums. Original
world-tick L/A medians are 0.294/0.3325 ms baseline and 0.2915/0.329 ms candidate;
rendering is 0.3765/0.431 ms versus 0.376/0.4305 ms, preserving the accepted band.
Short-server medians differ by less than 0.5%. Sustained Hono/Express/h3 medians
are slightly higher; Elysia differs by -0.2%. Four runs per binary per server mode.
yt-dlp wall time is 2.451/2.469 s (same output); Ink reaches 47 tests with both
binaries in the same 5 s PTY window. These small differences are not evidence of
universal improvement. Optional-missing reads are 39.4/37.9 ms per 10M; ordinary
reads 24.4/24.8 ms. Parameter-write rows are essentially unchanged. bench_dec medians are 204.29/204.03 ms in an eight-process ABBA/BAAB comparison.

Validation on the pinned fresh-PGO candidate:

- 18 Meson tests pass, including native generation, barrier and layout assertions.
- 4240 specs/102 files pass; the array COW regression passes.
- Harness: 241 pass/1 known Rolldown react/jsx-runtime dependency failure.
- Stack-depth scan emits no analysis-rejection or JIT-overflow warnings, but exits 2
 because test_throw_stack.cjs and test_with_strict.cjs intentionally exit 1. Both
 error fixtures reproduce with the committed baseline; coverage is incomplete.
- Preflight passes. Its fresh-build/reconfigure recommendation was covered by the
 standard PGO setup/build; spec and native recommendations were executed.
- Source/profile and binary hashes match the validated artifacts. The user's
 benchmark score.json and application fixtures are unchanged. No profiling code
 remains in source. No abandoned Ant CPU processes were found.

The changes and regenerated profile are uncommitted. This experiment establishes
a substantial Splay win; it does not establish a broad throughput win from aging
alone or remove the remaining gap to V8. Artifacts, complete samples, fixtures,
logs and manifests are under /tmp/ant-gc-completion-20260916.
