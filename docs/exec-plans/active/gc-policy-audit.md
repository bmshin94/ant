# GC policy audit against V8

Status: active
Date: 2026-09-16

## Goal and scope

Audit the new generational policies against all eight bench-v8 workloads, using
installed Node 26.8.1 / V8 14.6.202.34-node.28 and current Ant in this worktree.
Do not use the implementation-review skill or subagents. Preserve the user's
examples/bench-v8/score.json. No commits requested. No bench-v8/Game-of-Life PGO
training. Diagnostic tracing is temporary and is not timing qualification.

Artifacts: /tmp/ant-gc-audit-20260916. The initial binary/profile/source are pinned.

## Method

Use identical fixtures, deterministic benchmark RNG, fixed warmup/work counts,
and separate setup/warm/work/teardown markers. Split Crypto into Encrypt/Decrypt
and EarleyBoyer into Earley/Boyer when analyzing collection events. Run each
trace twice. Node has default and no-allocation-site-pretenuring controls.
Ant tracing records collection reason, generation counts, allocation-site
feedback, old allocations and the managed-byte policy. Keep diagnostic timings
separate from untraced score measurements; V8 pause durations omit substantial
background GC work.

## Confirmed mistakes and fixes

1. Snapshot pinning set generation/permanent flags and moved lists, but did not
   synchronize old_live_count. Incremental promotion accounting preserved the
   missing startup count until a major. Set old_live_count to the allocated live
   count after all existing objects are pinned. A native regression failed before
   the fix and passes after it.
2. Permanent opaque owners contributed to owner-liveness debt. Keep their
   remembered barriers, but exclude them from the debt count: a major cannot
   establish that a permanent owner is dead. A native test verifies child
   survival, the remembered flag, and zero debt; it failed before the fix.
3. js_type_alloc charged the allocation triggering a major before collection;
   collection reset the charge although the allocation occurred afterwards.
   Check projected pressure without overflowing, then charge the new interval.
   A native regression reproduced the lost charge.
4. Optional object/array site-cache allocation was dereferenced without checking
   failure. Return with no site cache if allocation fails, preserving generic
   literal execution. This is source-verified hardening, not a fault-injected test.

## Important V8 behavior that was missed

Ant learns all three Splay literal sites during setup and keeps them pretenured.
The same fixed 1000-work fixture still triggers 22 minors/3 majors in Ant;
16 minors are rope-pressure triggered. Disabling young probes changes trigger
labels but not the total (22/3). Disabling Ant pretenuring also yields 22/3:
other nursery pressure still controls collection timing.

Node's equivalent pretenuring control repeats 0 minors/2 majors enabled versus
25/1 disabled. Native generation inspection after setup reports the fresh leaf,
its array, and its newly concatenated string all old with pretenuring enabled;
all three are young when disabled. Cloned V8 source contains propagation of
old allocation into inner allocations (src/compiler/memory-optimizer.cc:296).
Ant currently pretenures only object/array literal wrappers; js_mkrope always
allocates young ropes. This missed interaction explains why literal pretenuring
alone does not reproduce V8's nursery avoidance. It is not fixed by merely
relinking wrappers to the old list.

V8 also reconsiders tenuring after low old-generation survival
(src/heap/heap.cc:2783). Ant's periodic young probes are a different strategy;
the no-probe control shows they are not the primary Splay collection-count cause.

Aging also remains incomplete across heap kinds. The new two-survival policy
covers ant_object_t only. gc_sweep_young_closures promotes closures immediately;
gc_sweep_young_upvalues removes survivors from its young roster immediately;
gc_ropes_sweep promotes a whole surviving rope block on its first collection.
This scope limitation is relevant to closure-heavy workloads and was understated
in the earlier implementation summary.

## Broader workload findings

Only Splay activates pretenured sites in this fixed-work study. Disabling V8
pretenuring barely changes collection counts for the other seven workloads.
Richards and Crypto spend very little time in Ant GC; NavierStokes has no measured
Ant collections. Their throughput differences therefore require work outside
these GC policies. DeltaBlue, RayTrace and EarleyBoyer incur repeated Ant majors
from conservative owner debt while V8 collects their temporaries primarily young.
RegExp reaches major collection through pool-allocation pressure; fewer but
heavier Ant collections do not imply lower GC cost.

## Validation

Three new native failures reproduced before their corresponding fixes. Final corrected traces, fresh-PGO production comparisons across all eight workloads, native/spec validation and instrumentation-restoration checks are complete.


## Final measurements

Untraced version-7 benchmark scores, higher is faster. Serial AVBBVA order gives two processes per engine per workload; A is the audit-start Ant binary, B is corrected Ant with fresh standard PGO, V is installed Node/V8. Small before/after differences are not treated as established speedups/regressions.

| Workload | Ant before | Ant corrected | Node/V8 |
| --- | ---: | ---: | ---: |
| Richards | 6538 | 6517 | 76967 |
| DeltaBlue | 6473 | 6499 | 205839 |
| Crypto | 13417 | 13551 | 90104 |
| RayTrace | 12974 | 12800 | 147480 |
| EarleyBoyer | 13374 | 15208 | 154900 |
| RegExp | 7652 | 7531 | 22069 |
| Splay | 7852 | 7800 | 84225 |
| NavierStokes | 24573 | 24475 | 74274 |

The separate fixed-work diagnostic fixtures produced these **minor / major** collection counts during work only. Two repetitions per engine; setup and warmup excluded. These counts are not a speed ranking; Node performs concurrent/parallel GC work.

| Workload | Work iterations | Ant corrected | Node/V8 |
| --- | --- | ---: | ---: |
| Richards | 1000 | 1 / 0 | 2 / 0 |
| DeltaBlue | 500 | 72 / 9 | 39–40 / 0 |
| Crypto | 100 + 100 | 2 / 1 | 7–8 / 0 |
| RayTrace | 100 | 336 / 42 | 228–229 / 0 |
| EarleyBoyer | 300 + 50 | 362–367 / 48 | 62–63 / 0 |
| RegExp | 100 | 50–51 / 25 | 279 / 0 |
| Splay | 1000 | 22 / 3 | 0 / 2 |
| NavierStokes | 200 | 0 / 0 | 0–1 / 0 |

Crypto iterations are Encrypt + Decrypt; EarleyBoyer iterations are Earley + Boyer. Full phase counters, literal-site maps, generation inspection and control logs are under /tmp/ant-gc-audit-20260916.

Final fresh-PGO build,18 Meson tests,4240 specs and COW regression pass. Three targeted assertions reproduced before their fixes and pass afterwards. Final harness:241 pass plus the known Rolldown react/jsx-runtime dependency failure. Preflight passes. Three new native assertions failed before their corresponding corrections and pass afterwards. No fault injection was added for the optional cache-allocation guard. All audit tracing has been removed from source. No changes committed.
