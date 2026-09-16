# Splay toward 9k

Status: active
Last reviewed: 2026-09-15
Owner: theMackabu

Profile current Splay before optimizing. Baseline HEAD f60e5e0e and existing
binary/profile are pinned in /tmp/ant-splay-9k-20260915/manifest.json.
Five fresh official-harness runs: median 7953.52, range 7711.24-7994.27.
Target 9000 requires about 13% score improvement. No Splay/bench-v8 or Game
of Life in PGO training. Do not change benchmark work or GC thresholds.

Three two-second steady-state profiles attribute about 46% to GC, 33-34% to
native runtime, 19-20% to generated code. gc_mark_value alone accounts for
12-15% of self samples. Native assembly shows a 64-byte register frame even
for immediately skipped values; closure tracing and mark-stack allocation
are inlined into this dispatch.

First experiment: make gc_mark_closure and gc_mark_stack_push noinline to
keep this dispatch small. No marking, root, generation or barrier changes.
Measure native assembly and serial baseline/candidate Splay first. Keep only
measured gains; run relevant GC correctness coverage and broad workloads
before qualification. Record PGO mismatch warnings separately.


Outlining alone: matched control 7741.64 versus 7788.17, only +0.60%; rejected.
The build reconfigured after the initial saved binary, so control-ant was
rebuilt from unchanged source with the same configuration/profile. Outlining
invalidated PGO counts for three GC functions. Original source restored.

Second experiment: inline dispatch for common object-scanner edges (objects,
strings, immediate values); forward closure/BigInt/Symbol edges to the existing
full marker. Preserve arena bounds, mark epochs, generation filtering and weak
notifications. No roots, barriers, pacing or object layouts change.


Common-edge inline candidate: matched ABBA+BAAB 7685.80 -> 7855.73 (+2.21%).
GC exact-edge, plain-array, shape-reference and rope native tests passed.
Temporary telemetry confirmed large allocation/reclamation batches (for example,
9.05M tracked objects before a major collection, 1.42M afterward). This alone
does not prove a leak. Diagnostic source/binary were archived and removed;
GC pacing remains unchanged. Retrain standard PGO for the edge-scanner candidate
before deciding whether to retain it or claiming any score target.


Fresh-PGO scanner result: 20 fresh processes per binary, balanced alternating
order, matched control median 7456.43 versus candidate 7689.83 (+3.13%).
9k was not reached. Four native GC tests, the expanded exact-edge mixed-value
test, 4,240 specs before retraining, the comprehensive GC test, and 240 harness
entries pass; the harness retains only the known Rolldown dependency failure.
Other bench-v8 ABBA medians were within about 0.4%. Life checksums match.
Truthiness was extended to 20 processes per binary after short-sample variation;
full rows and summary are in /tmp/ant-splay-9k-20260915/validation.

## Retention investigation

The user questioned the extended-profile heap growth. A temporary gated
force-GC hook was added, used after fresh event-loop turns, then removed.
After 1,000 SplayRun calls, major GC reduces 4.68M tracked objects to the
expected ~768.7K live tree/payload objects. After SplayTearDown and verifying
splayTree === null, repeated normal major collections still retain ~768.7K.

In one diagnostic binary, serial normal/no-conservative-stack/normal probes
showed ~768.7K / 591 / ~768.7K objects after teardown. RSS was ~945 MB with
normal scans versus ~15 MB without them; live-tree checks still retained the
expected graph in both modes. This implicates conservative native-stack roots
in post-teardown graph retention. Exact retaining stack slots/frames have not
been traced; stale pointer-shaped values remain an inference. Do not disable
stack scanning in production: live JIT references rely on it.

Major-GC pacing is a separate suspected contributor to pre-teardown growth:
minor collections update the major threshold base, and the nursery branch
returns before timed-major fallback. No pacing ablation or fix was retained.
Diagnostic code and binary were archived under /tmp/ant-splay-gc-probe-20260915;
normal source and the exact pre-probe fresh-PGO binary were restored. Only the
scanner experiment, test, profile and plan remain modified. Nothing committed.


### Retaining slots identified

Temporary stack-word and frame tracing identified the primary post-teardown
root in libuv's `struct kevent events[1024]` (kqueue.c:159). LTO inlines
uv__io_poll into uv_run, leaving this 32 KiB region inside uv_run's frame while
timer callbacks execute. In the traced ARM64 layout, events[839].data contains
the old raw SplayTree pointer. GC mistakes that stale word and other old
node/payload words for roots.

Excluding only this verified buffer region in a diagnostic collection, while
keeping every other conservative stack scan, changes post-teardown retention
from 768,705 to 791 objects (repeat 768,703 to 789). A secondary stale boxed
Node pointer remains in sv_execute_frame at SP+0xc0; exact last writer is not
yet isolated. No vendor edits or root exclusions are retained. Normal source
and the exact pre-probe binary were restored. Full tracing, native layout,
assembly, raw values and limitations:
`/tmp/ant-splay-root-trace-20260915/README.md`.


## Kqueue false-root fix

User approved trying noinline plus event-buffer initialization, rather than
teaching the GC a build-specific exclusion. New native macOS test covers timer
and I/O callbacks, preserves an explicitly rooted live object, and leaves a
dead graph's pointer bits in expired stack storage. Before the vendor patch,
both cases retain 2098 objects versus a baseline of 48 and fail. The durable
patch lives in vendor/packagefiles/patches/libuv-kqueue-gc-stack.patch and is
registered in libuv.wrap. No GC root scanning is disabled.


Validation on 2026-09-16: both native callback cases now retain exactly their
48-object baseline, while the explicitly rooted object survives. The final
ARM64 assembly keeps uv__io_poll separate and shrinks uv_run's frame from
roughly 33 KiB to 128 bytes. The durable patch reproduces the built vendor file
when applied to a pristine copy.

The real Splay teardown probe, with a temporary force-major-GC hook but no
scanner exclusions, retains 594 objects after the first post-teardown major
and 591 after the second, versus about 768,700 before the patch. RSS falls to
about 15 MB after the second major. The live tree retains its expected roughly
768,600 objects. The hook was removed and the pinned production candidate
restored before performance tests. This fixes the identified event-buffer
retention; it does not establish the cause of all synchronous churn growth.
Artifacts and pinned pre/post binaries: /tmp/ant-kqueue-gc-fix-20260915.

Production score comparison: 20 fresh processes per binary in ABBABAAB order
repeated five times, same existing PGO profile, baseline median 7908.70
(range 6638.42-8027.23), patched median 7791.63 (7723.16-8043.16).
That is a 1.48% lower median, not a score win or the 9k target. Final-source PGO
has not been regenerated for the vendor fix; separate its retention benefit
from any throughput claim. Binary and profile hashes are in manifest.json.

Final validation: 4240 specs / 102 files pass; native gc-event-loop-stack passes;
full harness 240 pass and one pre-existing Rolldown unresolved react/jsx-runtime
failure. Preflight and diff whitespace checks pass. Reconfigured and built ant
plus the native test; ant-runtime was not rebuilt. No abandoned task-owned Ant
processes found.

Server ABBA/BAAB (four processes per binary): sustained oha median rates improve
0.87-2.22%; short 600-request medians improve for Hono/Express/h3, while Elysia
is 2.83% lower. These small-sample server figures are observations, not a strict
non-regression certification. Full values are in server-summary.txt under the
artifact directory. No final-source PGO retraining or commit for this fix.


## Major-baseline experiment (2026-09-16)

Investigate scheduler accounting separately from promotion. First ablation:
stop minor collections overwriting gc_last_live, so only a major sets the
retained-size baseline. Preserve the existing promotion and other thresholds.
Pin the current production binary, compare fixed-work collection counts/memory
and scored Splay, and reject or refine based on evidence. Artifacts:
/tmp/ant-splay-major-baseline-20260916. No final-source PGO claims until retrained.

The isolated last-major-baseline ablation is rejected. Four clean runs/binary:
fixed1000-iteration median922.06->1887.25ms, tracked objects4.68M->0.88M,
scored Splay7800.09->2547.60. Same PGO profile, no discarded counts in the
clean experiment build. Diagnostics show majors2->21 and major object
scans1.61M->16.16M, while minor survival remains95%. The adaptive growth
rule tightens toward1.25x when reclamation exceeds20%, causing frequent full
scans of the~768K live graph. This demonstrates a scheduler-policy interaction,
not a safe one-line performance fix. Restored original sources and production
binary exactly; earlier scanner/libuv work preserved. Full evidence in the
experiment report.md; no new production change retained.
