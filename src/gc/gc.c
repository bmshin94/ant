#include <gc.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <math.h>
#include "shapes.h"

#include "gc/objects.h"
#include "gc/bigints.h"
#include "gc/strings.h"
#include "gc/ropes.h"
#ifdef ANT_WASM_EMBED
#include "wasm_embed.h"
#endif

bool gc_disabled = false;

static uint64_t gc_now_ns(void) {
#ifdef ANT_WASM_EMBED
  return (uint64_t)(ant_wasm_now_ms() * 1000000.0);
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

static uint64_t gc_now_ms(void) { return gc_now_ns() / 1000000u; }

void gc_policy_init(ant_t *js) {
  js->gc_policy.nursery_threshold = GC_NURSERY_THRESHOLD;
  js->gc_policy.major_every_n = GC_MAJOR_EVERY_N_MINOR;
  js->gc_policy.minor_surv_ewma = 128;
  js->gc_policy.major_end_ns = gc_now_ns();
  js->gc_policy.object_allocation_ceiling = SIZE_MAX;
}

// Aim to leave 97% of elapsed time to the mutator, subject to a 4x memory
// growth cap. This is a budget, not a guarantee of that utilization. A slow
// collector or fast allocator needs more headroom, not more frequent majors.
double gc_policy_growth_factor(double gc_speed, double allocation_speed) {
  if (!(gc_speed > 0) || !(allocation_speed > 0) ||
      !isfinite(gc_speed) || !isfinite(allocation_speed)) return 4.0;
  double a = (gc_speed / allocation_speed) * 0.03;
  if (!isfinite(a)) return 1.1;
  double b = a - 0.97;
  double factor = b > 0 ? a / b : 4.0;
  if (factor < 1.1) factor = 1.1;
  if (factor > 4.0) factor = 4.0;
  return factor;
}

// Count managed allocation between collections without adding a counter store
// to every object allocation. Pool counters include ropes; array growth is net
// backing-store growth. External/native allocation is deliberately not modeled.
static void gc_account_allocations(ant_t *js) {
  size_t objects = js->obj_arena.live_count, arrays = js->alloc_bytes.arrays;
  if (objects > js->gc_policy.observed_objects)
    js->gc_policy.allocated_since_major +=
      (double)(objects - js->gc_policy.observed_objects) * sizeof(ant_object_t);
  if (arrays > js->gc_policy.observed_arrays)
    js->gc_policy.allocated_since_major += arrays - js->gc_policy.observed_arrays;
  if (js->gc_pool_alloc > js->gc_policy.observed_pool_alloc)
    js->gc_policy.allocated_since_major += js->gc_pool_alloc - js->gc_policy.observed_pool_alloc;
  js->gc_policy.observed_objects = objects;
  js->gc_policy.observed_arrays = arrays;
  js->gc_policy.observed_pool_alloc = js->gc_pool_alloc;
}

static void gc_finish_policy_sample(ant_t *js, uint64_t start, bool major, size_t heap_before) {
  gc_reclaim_flush(js);
  uint64_t end = gc_now_ns();
  if (major) {
    uint64_t interval = start > js->gc_policy.major_end_ns ? start - js->gc_policy.major_end_ns : 0;
    uint64_t mutator = interval > js->gc_policy.minor_pause_ns ? interval - js->gc_policy.minor_pause_ns : 0;
    double factor = 4.0;
    if (js->gc_policy.has_major_sample && mutator && end > start &&
        js->gc_policy.allocated_since_major > 0) {
      double gc_speed = (double)heap_before / (double)(end - start);
      double alloc_speed = js->gc_policy.allocated_since_major / (double)mutator;
      js->gc_policy.gc_bytes_per_ns = js->gc_policy.gc_bytes_per_ns > 0
        ? (js->gc_policy.gc_bytes_per_ns * 3 + gc_speed) / 4 : gc_speed;
      js->gc_policy.allocation_bytes_per_ns = js->gc_policy.allocation_bytes_per_ns > 0
        ? (js->gc_policy.allocation_bytes_per_ns * 3 + alloc_speed) / 4 : alloc_speed;
      factor = gc_policy_growth_factor(js->gc_policy.gc_bytes_per_ns,
                                      js->gc_policy.allocation_bytes_per_ns);
    }
    // One byte budget covers the entire managed heap. Independent object-count
    // and pool budgets would trigger a full scan when just one component grows,
    // even when the measured collector budget has ample remaining headroom.
    double live_bytes = (double)js->obj_arena.live_count * sizeof(ant_object_t) +
      js->alloc_bytes.arrays + js->gc_pool_last_live;
    double limit = live_bytes * factor;
    js->gc_policy.major_heap_limit_bytes = limit >= (double)SIZE_MAX
      ? SIZE_MAX : (size_t)limit;
    if (js->gc_policy.major_heap_limit_bytes < GC_POOL_PRESSURE_FLOOR)
      js->gc_policy.major_heap_limit_bytes = GC_POOL_PRESSURE_FLOOR;
    js->gc_policy.has_major_sample = true;
    js->gc_policy.major_end_ns = end;
    js->gc_policy.last_major_pause_ns = end > start ? end - start : 0;
    js->gc_policy.minor_pause_ns = 0;
    js->gc_policy.allocated_since_major = 0;
  } else if (end > start) {
    js->gc_policy.minor_pause_ns += end - start;
  }
  js->gc_policy.observed_objects = js->obj_arena.live_count;
  js->gc_policy.observed_arrays = js->alloc_bytes.arrays;
  js->gc_policy.observed_pool_alloc = js->gc_pool_alloc;
}

static size_t gc_scaled_threshold(size_t base_live, uint32_t growth_x256, size_t floor) {
  size_t scaled = (base_live * (size_t)growth_x256) / 256u;
  if (scaled < floor) scaled = floor;
  return scaled;
}

static size_t gc_pool_live_bytes(ant_t *js) {
  ant_pool_stats_t rope_stats = js_pool_stats(&js->pool.rope);
  ant_pool_stats_t rope_young_stats = js_pool_stats(&js->rope_gc.young);
  ant_pool_stats_t rope_old_stats = js_pool_stats(&js->rope_gc.old);
  ant_pool_stats_t symbol_stats = js_pool_stats(&js->pool.symbol);
  ant_pool_stats_t bigint_stats = js_class_pool_stats(&js->pool.bigint);
  ant_string_pool_stats_t string_stats = js_string_pool_stats(&js->pool.string);

  return rope_stats.used
    + rope_young_stats.used
    + rope_old_stats.used
    + symbol_stats.used
    + bigint_stats.used
    + string_stats.total.used;
}

size_t gc_live_major_threshold(ant_t *js) {
  if (js->gc_policy.has_major_sample) {
    size_t limit = js->gc_policy.major_heap_limit_bytes;
    size_t pool = js->gc_pool_last_live;
    size_t arrays = js->alloc_bytes.arrays;
    size_t remaining = limit > pool ? limit - pool : 0;
    remaining = remaining > arrays ? remaining - arrays : 0;
    remaining = remaining > js->gc_pool_alloc ? remaining - js->gc_pool_alloc : 0;
    size_t threshold = remaining / sizeof(ant_object_t);
    if (threshold < GC_MAJOR_SCALE) threshold = GC_MAJOR_SCALE;
    if (threshold > js->gc_policy.object_allocation_ceiling)
      threshold = js->gc_policy.object_allocation_ceiling;
    return threshold;
  }
  size_t threshold = gc_scaled_threshold(
    js->gc_last_live, 
    384, GC_MAJOR_SCALE
  );

  bool nursery_churn = js->gc_policy.minor_surv_ewma <= 64;   // <= 25% young survival
  bool nursery_sticky = js->gc_policy.minor_surv_ewma >= 160; // >= 62.5% young survival

  if (js->gc_use_nursery_major_floor) {
    if (nursery_sticky) js->gc_use_nursery_major_floor = false;
  } else if (nursery_churn) js->gc_use_nursery_major_floor = true;

  if (js->gc_use_nursery_major_floor) {
    size_t nursery_floor = js->gc_last_live + js->gc_policy.nursery_threshold;
    if (threshold < nursery_floor) threshold = nursery_floor;
  }
  // Leave enough arena headroom for the scheduler's allocation tick interval.
  if (threshold > js->gc_policy.object_allocation_ceiling)
    threshold = js->gc_policy.object_allocation_ceiling;
  return threshold;
}

size_t gc_pool_major_threshold(ant_t *js) {
  if (js->gc_policy.has_major_sample) {
    size_t objects = js->obj_arena.live_count * sizeof(ant_object_t);
    size_t remaining = js->gc_policy.major_heap_limit_bytes;
    remaining = remaining > objects ? remaining - objects : 0;
    remaining = remaining > js->alloc_bytes.arrays ? remaining - js->alloc_bytes.arrays : 0;
    remaining = remaining > js->gc_pool_last_live ? remaining - js->gc_pool_last_live : 0;
    return remaining < GC_POOL_PRESSURE_FLOOR ? GC_POOL_PRESSURE_FLOOR : remaining;
  }
  return gc_scaled_threshold(js->gc_pool_last_live, 384, GC_POOL_PRESSURE_FLOOR);
}

static void gc_adapt_nursery(ant_t *js, size_t young_before, size_t survivors) {
  if (young_before == 0) return;
  uint32_t rate = (uint32_t)((survivors * 256) / young_before);
  js->gc_policy.minor_surv_ewma = (js->gc_policy.minor_surv_ewma * 3 + rate) >> 2;
  if (js->gc_policy.minor_surv_ewma < 64 && js->gc_policy.nursery_threshold > GC_NURSERY_THRESHOLD / 2)
    js->gc_policy.nursery_threshold -= js->gc_policy.nursery_threshold / 4;
  else if (js->gc_policy.nursery_threshold < GC_NURSERY_THRESHOLD)
    js->gc_policy.nursery_threshold = GC_NURSERY_THRESHOLD;
}

static void gc_mark_str(ant_t *js, ant_value_t root) {
  static const void *dispatch[] = {
    [STR_HEAP_TAG_FLAT] = &&l_flat,
    [STR_HEAP_TAG_ROPE] = &&l_rope,
    [STR_HEAP_TAG_BUILDER] = &&l_builder,
  };

  ant_value_t local[32];
  ant_value_t *stack = local;
  
  size_t sp = 0, cap = 32;
  ant_value_t v = root;

  l_next:
  if (!is_tagged(v)) goto l_pop;
  uint8_t t = vtype_tagged(v);
  if (t != kTypeString) goto l_pop;

  uintptr_t tag = (uintptr_t)(vdata(v) & STR_HEAP_TAG_MASK);
  uintptr_t data = (uintptr_t)vptr_masked(v, STR_HEAP_TAG_MASK);

  if (tag < sizeof(dispatch) / sizeof(*dispatch) && dispatch[tag])
    goto *dispatch[tag];
  goto l_flat;

  l_rope: {
    ant_rope_heap_t *rope = (ant_rope_heap_t *)data;
    constexpr size_t align_rope = _Alignof(ant_rope_heap_t);
    if (gc_ropes_mark(js, rope, sizeof(*rope), align_rope) != GC_ROPE_MARK_TRACE) goto l_pop;

    if (vtype(rope->cached) == kTypeString) {
      v = rope->cached;
      goto l_next;
    }

    if (!ant_value_stack_push_with_spill(&stack, &sp, &cap, local, rope->left)) 
      gc_mark_str(js, rope->left);
    
    v = rope->right;
    goto l_next;
  }

  l_builder: {
    ant_string_builder_t *builder = (ant_string_builder_t *)data;
    constexpr size_t align_string = _Alignof(ant_string_builder_t);
    if (gc_ropes_mark(js, builder, sizeof(*builder), align_string) != GC_ROPE_MARK_TRACE) goto l_pop;
    
    gc_mark_str(js, builder->snapshot);
    gc_mark_value(js, builder->cached);
    
    for (ant_builder_chunk_t *chunk = builder->head; chunk; chunk = chunk->next) {
      constexpr size_t align_builder = _Alignof(ant_builder_chunk_t);
      gc_rope_mark_result_t marked = gc_ropes_mark(js, chunk, sizeof(*chunk), align_builder);
      if (marked == GC_ROPE_MARK_INVALID) break;
      if (marked == GC_ROPE_MARK_TRACE) gc_mark_value(js, chunk->value);
    }
    
    goto l_pop;
  }

  l_flat:
    if (data && !js->rope_gc.minor_marking)
      gc_strings_mark(js, (const void *)data);
  l_pop:
    if (sp > 0) {
      v = stack[--sp];
      goto l_next;
    }
    if (stack != local) free(stack);
    return;
}

static void gc_mark_flat_str(ant_t *js, ant_value_t value) {
  if (!is_tagged(value) || vtype(value) != kTypeString) return;
  if ((vdata(value) & STR_HEAP_TAG_MASK) != STR_HEAP_TAG_FLAT) return;
  const void *ptr = vptr_masked(value, STR_HEAP_TAG_MASK);
  if (ptr) gc_strings_mark(js, ptr);
}

void gc_remember_builder(ant_t *js, ant_string_builder_t *builder) {
  if (!js || !builder || builder->in_remember_set) return;
  if (js->rope_gc.remembered_builder_len >= js->rope_gc.remembered_builder_cap) {
    size_t cap = js->rope_gc.remembered_builder_cap
      ? js->rope_gc.remembered_builder_cap * 2u : 64u;
    ant_string_builder_t **items = (ant_string_builder_t **)realloc(
      js->rope_gc.remembered_builders, cap * sizeof(*items)
    );
    if (!items) {
      js->gc_remember_overflow = true;
      return;
    }
    js->rope_gc.remembered_builders = items;
    js->rope_gc.remembered_builder_cap = cap;
  }
  builder->in_remember_set = 1;
  js->rope_gc.remembered_builders[js->rope_gc.remembered_builder_len++] = builder;
}

static void gc_clear_remembered_builders(ant_t *js) {
  for (size_t i = 0; i < js->rope_gc.remembered_builder_len; i++)
    js->rope_gc.remembered_builders[i]->in_remember_set = 0;
  js->rope_gc.remembered_builder_len = 0;
}

void gc_run(ant_t *js) {
  if (__builtin_expect(gc_disabled, 0)) return;
  uint64_t start_ns = gc_now_ns();
  gc_account_allocations(js);
  size_t heap_before = js->obj_arena.live_count * sizeof(ant_object_t) +
    js->alloc_bytes.arrays + gc_pool_live_bytes(js);
  js->gc_running = true;
  
  gc_ropes_begin_result_t rope_begin = gc_ropes_begin(js, false);
  ANT_ASSERT(
    rope_begin != GC_ROPES_BEGIN_RETRY_MAJOR,
    "major rope marking cannot request another major"
  );


  gc_bigints_begin(js);
  gc_strings_begin(js);
  
  bool conservative = rope_begin == GC_ROPES_BEGIN_CONSERVATIVE_MAJOR;
  gc_objects_run(
    js, conservative ? gc_mark_flat_str : gc_mark_str,
    conservative ? gc_ropes_mark_conservative_roots : NULL
  );
  
  gc_clear_remembered_builders(js);
  ant_ic_epoch_bump();
  ant_ic_obj_epoch_bump();

  gc_bigints_sweep(js);
  gc_strings_sweep(js);
  gc_ropes_sweep(js, false);

  js->gc_last_live = js->obj_arena.live_count;
  js->old_live_count = js->obj_arena.live_count;
  js->minor_gc_count = 0;

  js->gc_pool_last_live = gc_pool_live_bytes(js);
  js->gc_pool_alloc = 0;
  js->rope_gc.young_alloc = 0;
  js->gc_closure_alloc = 0;
  js->gc_closure_at_minor = 0;
  js->gc_closure_wm_at_major = js->closure_arena.watermark;
  js->gc_closure_wm_minor_tried = js->closure_arena.watermark;
  js->gc_closure_promoted_since_major = 0;
  js->gc_remember_overflow = false;

  js->gc_policy.last_run_ms = gc_now_ms();
  js->gc_policy.last_major_ms = js->gc_policy.last_run_ms;
  gc_finish_policy_sample(js, start_ns, true, heap_before);
  js->gc_running = false;
}

void gc_run_minor(ant_t *js) {
  if (__builtin_expect(gc_disabled, 0)) return;
  uint64_t start_ns = gc_now_ns();
  gc_account_allocations(js);
  js->gc_running = true;

  if (__builtin_expect(js->gc_remember_overflow, 0)) {
    gc_run(js);
    return;
  }
  if (gc_ropes_begin(js, true) != GC_ROPES_BEGIN_NORMAL) {
    gc_run(js);
    return;
  }

  size_t old_before   = js->old_live_count;
  size_t live_before  = js->obj_arena.live_count;
  size_t young_before = live_before > old_before ? live_before - old_before : 0;

  for (size_t i = 0; i < js->rope_gc.remembered_builder_len; i++)
    gc_mark_str(js, ant_mkbuilder_value(js->rope_gc.remembered_builders[i]));
  gc_objects_run_minor(js, gc_mark_str);
  gc_clear_remembered_builders(js);
  gc_ropes_sweep(js, true);

  ant_ic_obj_epoch_bump();

  // Bootstrap until a major provides a retained-size baseline.
  if (!js->gc_policy.has_major_sample) js->gc_last_live = js->obj_arena.live_count;
  js->minor_gc_count++;

  size_t survivors = js->obj_arena.live_count > old_before
    ? js->obj_arena.live_count - old_before : 0;

  js->gc_closure_at_minor = js->gc_closure_alloc;
  gc_adapt_nursery(js, young_before, survivors);
  js->gc_policy.last_run_ms = gc_now_ms();
  gc_finish_policy_sample(js, start_ns, false, 0);
  js->gc_running = false;
}

void gc_pressure(ant_t *js) {
  if (__builtin_expect(gc_disabled, 0)) return;
  js->gc_policy.tick = GC_MIN_TICK;
  gc_maybe(js);
}

static bool gc_has_remembered_owner_debt(ant_t *js) {
  // A few persistent owners do not justify repeatedly sweeping a large live
  // old heap. Amortize debt-only majors against observed minor collection work;
  // independent heap/pool/closure pressure checks still enforce their budgets.
  if (js->gc_policy.minor_pause_ns < js->gc_policy.last_major_pause_ns) return false;
  return js->gc_opaque_remembered_objects || js->remembered_upvalue_len ||
    js->remembered_closure_len || js->remembered_func_const_len ||
    js->remembered_coroutine_len;
}

void gc_maybe(ant_t *js) {
  if (__builtin_expect(gc_disabled, 0)) return;
  if (++js->gc_policy.tick < GC_MIN_TICK) return;
  
  size_t live = js->obj_arena.live_count;
  size_t young_count = live > js->old_live_count ? live - js->old_live_count : 0;
  size_t closure_young = js->gc_closure_alloc > js->gc_closure_at_minor
    ? js->gc_closure_alloc - js->gc_closure_at_minor : 0;

  if (young_count >= js->gc_policy.nursery_threshold ||
      js->rope_gc.young_alloc >= GC_ROPE_NURSERY_THRESHOLD ||
      closure_young >= GC_CLOSURE_NURSERY_THRESHOLD) {
    js->gc_policy.tick = 0;
    gc_run_minor(js);

    if (js->minor_gc_count >= js->gc_policy.major_every_n) {
      // These coarse remembered owners can only be proved dead by a major.
      // Bound that repeated work by measured collection cost: recovered nursery
      // headroom must not defer their liveness check indefinitely.
      bool major_due = gc_has_remembered_owner_debt(js);
      
      if (js->obj_arena.live_count >= gc_live_major_threshold(js)) major_due = true;
      else if (js->gc_pool_alloc >= gc_pool_major_threshold(js)) major_due = true;
      else if (js->closure_arena.watermark - js->gc_closure_wm_at_major >= GC_CLOSURE_MAJOR_GROWTH) major_due = true;
      else if (js->gc_closure_promoted_since_major >= GC_CLOSURE_PROMOTED_MAJOR) major_due = true;
      
      if (major_due) {
        js->minor_gc_count = 0;
        gc_run(js);
      }
    }

    return;
  }

  size_t threshold = gc_live_major_threshold(js);
  if (live >= threshold) {
    js->gc_policy.tick = 0;
    if (young_count >= live / 4) {
      gc_run_minor(js);
      if (js->obj_arena.live_count < gc_live_major_threshold(js) &&
          !(js->minor_gc_count >= js->gc_policy.major_every_n && gc_has_remembered_owner_debt(js))) return;
    }
    gc_run(js);
    return;
  }

  if (js->closure_arena.watermark - js->gc_closure_wm_at_major >= GC_CLOSURE_MAJOR_GROWTH) {
    js->gc_policy.tick = 0;
    if (js->closure_arena.watermark > js->gc_closure_wm_minor_tried) {
      js->gc_closure_wm_minor_tried = js->closure_arena.watermark;
      gc_run_minor(js);
      return;
    }
    gc_run(js);
    return;
  }

  if (js->gc_policy.tick < 8192) return;

  if (young_count == 0 && js->gc_pool_alloc == 0) {
    js->gc_policy.tick = 0;
    return;
  }

  if (gc_now_ms() - js->gc_policy.last_run_ms < GC_FORCE_INTERVAL_MS) {
    js->gc_policy.tick = 0;
    return;
  }

  js->gc_policy.tick = 0;
  if (gc_now_ms() - js->gc_policy.last_major_ms >= GC_FORCE_MAJOR_INTERVAL_MS) gc_run(js);
  else gc_run_minor(js);
}
