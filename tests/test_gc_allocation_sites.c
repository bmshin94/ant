// meson test -C build gc-allocation-sites
#include "internal.h"
#include "gc.h"
#include "gc/objects.h"
#include "gc/roots.h"
#include "silver/engine.h"
#include "silver/call.h"
#include "tokens.h"
#include <assert.h>
#include <stdio.h>
#include <math.h>
#include <float.h>

static_assert(sizeof(void *) != 8 || sizeof(sv_closure_t) == 128, "closure age must fit padding");
static_assert(sizeof(void *) != 8 || sizeof(sv_upvalue_t) == 40, "cell age must fit padding");
static_assert(sizeof(void *) != 8 || sizeof(sv_obj_site_cache_t) == 72, "initializer range must fit padding");

static void collect(ant_t *js, bool minor) {
  gc_disabled = false;
  if (minor) gc_run_minor(js); else gc_run(js);
  gc_disabled = true;
}

static void test_survivor_kinds(void) {
  ant_t *js = ant_create();
  assert(js);
  js_setstackbase(js, NULL); // Liveness below comes only from explicit roots.
  gc_disabled = true;
  GC_ROOT_SAVE(roots, js);
  ant_value_t owner = js_mkobj(js);
  GC_ROOT_PIN(js, owner);
  collect(js, false);

  sv_func_t func = {.upvalue_count = 1};
  sv_closure_t *closure = js_closure_alloc(js);
  assert(closure);
  closure->func = &func;
  closure->upvalues = closure->inline_upvals;
  closure->bound_this = closure->super_val = js_mkundef();
  sv_upvalue_t *cell = js_upvalue_alloc(js);
  assert(cell);
  cell->location = &cell->closed;
  cell->closed = js_mknum(42);
  closure->inline_upvals[0] = cell;
  js_set(js, owner, "fn", mkref(kTypeFunction, closure));
  js->gc_policy.minor_surv_ewma = 0;
  collect(js, true);
  assert(!closure->generation && !cell->generation);
  assert(js->young_closure_len == 1 && js->young_upvalue_len == 1);

  // On its second survival the cell/closure must remember a fresh child.
  ant_value_t child = js_mkobj(js);
  cell->closed = child;
  gc_upvalue_write_barrier(js, cell, child);
  js->gc_policy.minor_surv_ewma = 0;
  collect(js, true);
  assert(closure->generation && cell->generation);
  assert(!js_obj_ptr(child)->flags.generation);
  assert(cell->in_remember_set && closure->in_remember_set);
  collect(js, true);
  assert(gc_obj_is_marked(js_obj_ptr(child)));

  // Old cells must retain new values even when no ordinary object stays young.
  char bytes[128]; memset(bytes, 'r', sizeof(bytes));
  ant_value_t flat = js_mkstr(js, bytes, sizeof(bytes));
  size_t young_alloc = js->rope_gc.young_alloc;
  size_t pool_alloc = js->gc_pool_alloc;
  ant_value_t pretenured = js_string_concat(js, flat, flat, true);
  assert(!(ant_str_rope_ptr(pretenured)->flags & ANT_ROPE_FLAG_YOUNG));
  assert(js->rope_gc.young_alloc == young_alloc);
  assert(js->gc_pool_alloc == pool_alloc + sizeof(ant_rope_heap_t));
  js_set(js, owner, "old_rope", pretenured);
  ant_value_t rope = do_string_op(js, TOK_PLUS, flat, flat);
  ant_value_t fallback = js_string_concat(js, rope, pretenured, true);
  assert(ant_str_rope_ptr(fallback)->flags & ANT_ROPE_FLAG_YOUNG);
  js_set(js, owner, "fallback", fallback);
  cell->closed = rope;
  gc_upvalue_write_barrier(js, cell, rope);
  js->gc_policy.minor_surv_ewma = 0;
  collect(js, true);
  assert(ant_str_rope_ptr(rope)->flags & ANT_ROPE_FLAG_YOUNG);
  assert(js->rope_gc.survivor.head && !js->rope_gc.young.head);
  assert(cell->in_remember_set);
  collect(js, true);
  assert(!(ant_str_rope_ptr(rope)->flags & ANT_ROPE_FLAG_YOUNG));
  assert(!(ant_str_rope_ptr(fallback)->flags & ANT_ROPE_FLAG_YOUNG));

  // First-survival blocks are sealed: the next allocation is a fresh cohort.
  rope = do_string_op(js, TOK_PLUS, flat, flat);
  js_set(js, owner, "rope", rope);
  js->gc_policy.minor_surv_ewma = 0;
  collect(js, true);
  ant_pool_block_t *first_block = js->rope_gc.survivor.head;
  assert(first_block);
  ant_value_t newer = do_string_op(js, TOK_PLUS, rope, flat);
  js_set(js, owner, "rope", newer);
  assert(js->rope_gc.young.head != first_block);
  js->gc_policy.minor_surv_ewma = 0;
  collect(js, true);
  assert(!(ant_str_rope_ptr(rope)->flags & ANT_ROPE_FLAG_YOUNG));
  assert(ant_str_rope_ptr(newer)->flags & ANT_ROPE_FLAG_YOUNG);
  // The first-survival rope can die before promotion.
  js_set(js, owner, "rope", js_mkundef());
  collect(js, true);
  assert(!js->rope_gc.survivor.head);

  // A closure and captured cell can also die in their second young collection.
  closure = js_closure_alloc(js);
  assert(closure);
  closure->func = &func;
  closure->upvalues = closure->inline_upvals;
  closure->bound_this = closure->super_val = js_mkundef();
  cell = js_upvalue_alloc(js);
  assert(cell);
  cell->location = &cell->closed;
  cell->closed = js_mknum(7);
  closure->inline_upvals[0] = cell;
  js_set(js, owner, "temporary", mkref(kTypeFunction, closure));
  js->gc_policy.minor_surv_ewma = 0;
  collect(js, true);
  assert(!closure->generation && !cell->generation);
  size_t closures = js->closure_arena.live_count, cells = js->upvalue_arena.live_count;
  js_set(js, owner, "temporary", js_mkundef());
  collect(js, true);
  assert(js->closure_arena.live_count + 1 == closures);
  assert(js->upvalue_arena.live_count + 1 == cells);
  collect(js, false);
  ant_value_t old_text = js_get(js, owner, "old_rope");
  size_t len = 0;
  const char *text = js_getstr(js, old_text, &len);
  assert(text && len == 256);
  for (size_t i = 0; i < len; i++) assert(text[i] == 'r');
  GC_ROOT_RESTORE(js, roots);
  gc_disabled = false;
  js_destroy(js);
}

static void test_literal_concat_pretenuring(void) {
  ant_t *js = ant_create();
  assert(js);
  js_setstackbase(js, NULL);
  gc_disabled = true;
  const char *sources[] = {
    "(function(tag) { return { array: [0,1,2], text: 'long prefix for a rope: '+tag+' and a long suffix' }; })",
    "(function(tag) { return { text: 'long prefix for a rope: '+tag+' and a long suffix' }; })",
  };
  for (unsigned mode = 0; mode < 2; mode++) {
    ant_value_t fn = js_eval_bytecode_eval(js, sources[mode], strlen(sources[mode]));
    assert(vtype(fn) == kTypeFunction);
    sv_closure_t *closure = js_func_closure(fn);
    if (mode) closure->func->jit_compile_failed = true;
    gc_alloc_site_t *site = NULL;
    for (uint32_t i = 0; i < closure->func->obj_site_count; i++) {
      sv_obj_site_cache_t *entry = &closure->func->obj_sites[i];
      if (entry->initializer_end) site = &entry->allocation;
    }
    assert(site);
    ant_value_t flat = js_mkstr(js, "a sufficiently long tag", 23);
    ant_value_t young = js_string_concat(js, flat, flat, false);
    // A compiled site must observe both enabling and disabling without re-JIT.
    for (unsigned phase = 0; phase < 4; phase++) {
      site->pretenured = phase == 1 || phase == 2;
      ant_value_t arg = phase == 2 ? young : flat;
      for (unsigned i = 0; i < 1000; i++) {
        ant_value_t obj = sv_vm_call(js->vm, js, fn, js_mkundef(), &arg, 1, NULL, js_mkundef());
        assert(vtype(obj) == kTypeObject);
        ant_value_t result = js_get(js, obj, "text");
        assert(str_is_heap_rope(result));
        assert(((ant_str_rope_ptr(result)->flags & ANT_ROPE_FLAG_YOUNG) == 0) == (phase == 1));
      }
    }
    assert((closure->func->jit_code != NULL) == (mode == 0));
  }
  gc_disabled = false;
  js_destroy(js);
}

int main(void) {
  test_survivor_kinds();
  test_literal_concat_pretenuring();
  assert(gc_policy_growth_factor(0, 1) == 4.0);
  assert(gc_policy_growth_factor(NAN, 1) == 4.0);
  assert(gc_policy_growth_factor(1, 1) == 4.0);
  assert(gc_policy_growth_factor(10000, 1) == 1.1);
  assert(gc_policy_growth_factor(DBL_MAX, DBL_MIN) == 1.1);
  assert(gc_policy_growth_factor(100, 2) > gc_policy_growth_factor(100, 1));
  assert(gc_policy_growth_factor(50, 1) > gc_policy_growth_factor(100, 1));
  ant_t *js = ant_create();
  assert(js);
  js_setstackbase(js, NULL);
  gc_disabled = true;
  ant_value_t permanent_owner = js_mkobj(js);
  assert(ant_object_ensure_sidecar(js_obj_ptr(permanent_owner)));
  // Snapshot pinning moves every existing object out of the nursery before
  // the first major collection. Its generation accounting must move with it.
  gc_pin_existing_objects(js);
  assert(js->old_live_count == js->obj_arena.live_count);
  assert(js->objects == NULL && js->objects_old == NULL);
  // A permanent owner still needs its young-edge barrier, but a major cannot
  // prove it dead. Keeping it remembered must not create owner-liveness debt.
  ant_value_t permanent_child = js_mkobj(js);
  js_set(js, permanent_owner, "child", permanent_child);
  js->gc_policy.minor_surv_ewma = 0;
  collect(js, true);
  assert(js_obj_ptr(permanent_owner)->flags.in_remember_set);
  assert(js_obj_ptr(permanent_child)->flags.generation == 0);
  assert(js->gc_opaque_remembered_objects == 0);
  assert(js->old_live_count + 1 == js->obj_arena.live_count);
  collect(js, true);
  assert(js->old_live_count == js->obj_arena.live_count);
  assert(gc_obj_is_marked(js_obj_ptr(permanent_child)));
  GC_ROOT_SAVE(roots, js);
  ant_value_t keep = js_mkarr(js);
  GC_ROOT_PIN(js, keep);
  for (unsigned i = 0; i < GC_NURSERY_THRESHOLD; i++)
    js_arr_push(js, keep, js_mkobj(js));
  collect(js, false);
  size_t major_baseline = js->gc_last_live;
  size_t object_limit = gc_live_major_threshold(js);
  js->gc_pool_alloc += 1024u * 1024u;
  assert(gc_live_major_threshold(js) < object_limit);
  js->gc_pool_alloc -= 1024u * 1024u;

  gc_alloc_site_t hot = {0}, cold = {0};
  for (unsigned round = 0; round < 2; round++) {
    for (unsigned i = 0; i < 256; i++) {
      ant_value_t live = js_mkobj(js);
      gc_track_allocation(js, &hot, live);
      js_arr_push(js, keep, live);
      ant_value_t dead = js_mkobj(js);
      gc_track_allocation(js, &cold, dead);
    }
    collect(js, true);
    assert(!cold.pretenured); // Sample records must not retain their objects.
  }
  assert(hot.pretenured);
  assert(js->gc_last_live == major_baseline);
  assert(js->gc_allocation_samples_len == 0);

  // Already initialized young references must be remembered when allocated old.
  ant_value_t child = js_mkobj(js);
  js_set(js, child, "value", js_mknum(42));
  ant_value_t array = js_mkarr_dense_literal(js, &child, 1);
  size_t old_before = js->old_live_count;
  gc_track_allocation(js, &hot, array);
  assert(js_obj_ptr(array)->flags.generation == 1);
  assert(js->old_live_count == old_before + 1);
  js_arr_push(js, keep, array);
  collect(js, true);
  assert(js_getnum(js_get(js, child, "value")) == 42);
  assert(gc_obj_is_marked(js_obj_ptr(child)));

  // Stores performed after pretenuring still need the ordinary write barrier.
  ant_value_t owner = js_mkobj(js);
  gc_track_allocation(js, &hot, owner);
  assert(js_obj_ptr(owner)->flags.generation == 1);
  js_arr_push(js, keep, owner);
  child = js_mkobj(js);
  js_set(js, owner, "child", child);
  collect(js, true);
  assert(gc_obj_is_marked(js_obj_ptr(child)));

  // A first survivor remains young and can die without an intervening major.
  ant_value_t short_lived = js_mkobj(js);
  js_set(js, owner, "short", short_lived);
  js->gc_policy.minor_surv_ewma = 0;
  collect(js, true);
  assert(js_obj_ptr(short_lived)->flags.generation == 0);
  js_set(js, owner, "short", js_mkundef());
  collect(js, true);
  assert(js_obj_ptr(short_lived)->mark_epoch == ANT_GC_DEAD);

  // An old owner must retain a first survivor across minors. Promotion also
  // remembers a grandchild that is still in its first young survival cycle.
  child = js_mkobj(js);
  js_set(js, owner, "child", child);
  js->gc_policy.minor_surv_ewma = 0;
  collect(js, true);
  assert(js_obj_ptr(child)->flags.generation == 0);
  ant_value_t grandchild = js_mkobj(js);
  js_set(js, child, "grandchild", grandchild);
  collect(js, true);
  assert(js_obj_ptr(child)->flags.generation == 1);
  assert(js_obj_ptr(grandchild)->flags.generation == 0);
  collect(js, true);
  assert(gc_obj_is_marked(js_obj_ptr(grandchild)));
  assert(js_obj_ptr(grandchild)->flags.generation == 1);

  // Probe young allocations let a formerly long-lived site change its decision.
  for (unsigned round = 0; round < 32; round++) {
    for (unsigned i = 0; i < 128; i++) {
      ant_value_t dead = js_mkobj(js);
      gc_track_allocation(js, &hot, dead);
    }
    collect(js, true);
    if (round < 31) assert(hot.pretenured);
  }
  assert(!hot.pretenured);
  assert(!hot.samples && !hot.survivors);

  // Shared immediate storage is not independently freed with either wrapper.
  ant_value_t shared[] = {js_mknum(1), js_mknum(2), js_mknum(3)};
  ant_value_t a = js_mkarr_shared_literal(js, shared, 3);
  ant_value_t b = js_mkarr_shared_literal(js, shared, 3);
  js_arr_push(js, keep, a);
  js_arr_push(js, keep, b);
  assert(js_obj_ptr(a)->u.array.data == js_obj_ptr(b)->u.array.data);
  assert(js_array_ensure_writable(js, js_obj_ptr(a)));
  js_obj_ptr(a)->u.array.data[0] = js_mknum(99);
  assert(js_getnum(js_obj_ptr(b)->u.array.data[0]) == 1);
  collect(js, false);
  assert(js_getnum(js_obj_ptr(b)->u.array.data[2]) == 3);

  // Reclamation may free private buffers in batches off-thread, but drain and
  // isolate shutdown must complete ownership transfer without retaining buffers.
  for (unsigned i = 0; i < 10000; i++) (void)js_mkarr(js);
  collect(js, false);
  assert(gc_reclaim_pending_bytes(js) <= 8u * 1024u * 1024u);
  gc_reclaim_drain(js);
  assert(gc_reclaim_pending_bytes(js) == 0);

  // Both the nursery and live-pressure paths must reconsider the byte budget
  // after a minor frees transient arrays, rather than force an unnecessary major.
  size_t original_limit = js->gc_policy.major_heap_limit_bytes;
  js->gc_policy.major_heap_limit_bytes = js->obj_arena.live_count * sizeof(ant_object_t) +
    js->alloc_bytes.arrays + js->gc_pool_last_live + 1024u * 1024u;
  for (unsigned round = 0; round < 2; round++) {
    js->gc_policy.nursery_threshold = GC_NURSERY_THRESHOLD;
    js->minor_gc_count = GC_MAJOR_EVERY_N_MINOR;
    for (unsigned i = 0; i < (round ? 40000u : 20000u); i++) (void)js_mkarr(js);
    assert(js->obj_arena.live_count >= gc_live_major_threshold(js));
    uint64_t major_end = js->gc_policy.major_end_ns;
    gc_disabled = false;
    gc_pressure(js);
    gc_disabled = true;
    assert(js->gc_policy.major_end_ns == major_end);
  }
  js->gc_policy.major_heap_limit_bytes = original_limit;

  // Debt alone must not force an expensive major after cheap minors. Once
  // enough minor work has accumulated, a major establishes owner liveness.
  assert(ant_object_ensure_sidecar(js_obj_ptr(owner)));
  for (unsigned round = 0; round < 2; round++) {
    child = js_mkobj(js);
    js_set(js, owner, "child", child);
    js->gc_policy.minor_surv_ewma = 0;
    js->minor_gc_count = GC_MAJOR_EVERY_N_MINOR;
    js->gc_policy.last_major_pause_ns = round ? 0 : UINT64_MAX;
    for (unsigned i = 0; i < 40000; i++) (void)js_mkarr(js);
    uint64_t major_end = js->gc_policy.major_end_ns;
    gc_disabled = false;
    gc_pressure(js);
    gc_disabled = true;
    assert((js->gc_policy.major_end_ns != major_end) == (round != 0));
    assert(gc_obj_is_marked(js_obj_ptr(child)));
    if (!round) assert(js->gc_opaque_remembered_objects != 0);
  }
  assert(js->gc_opaque_remembered_objects == 0);

  // The allocation that triggers a pool-pressure major belongs to the new
  // interval. Clearing the previous interval must not lose its byte charge.
  char pending_string[256];
  memset(pending_string, 'q', sizeof(pending_string));
  js->gc_pool_alloc = gc_pool_major_threshold(js) - 1;
  uint64_t pool_major_end = js->gc_policy.major_end_ns;
  gc_disabled = false;
  ant_value_t charged_string = js_mkstr(js, pending_string, sizeof(pending_string));
  gc_disabled = true;
  assert(js->gc_policy.major_end_ns != pool_major_end);
  assert(!is_err(charged_string));
  assert(js->gc_pool_alloc >= sizeof(pending_string));

  GC_ROOT_RESTORE(js, roots);
  gc_disabled = false;
  js_destroy(js);
  puts("PASS allocation-site learning, weak samples, barriers and adaptation");
  return 0;
}
