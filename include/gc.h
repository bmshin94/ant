#ifndef ANT_GC_H
#define ANT_GC_H

#include "internal.h"
#include <stdbool.h>
#include <stdint.h>

static constexpr size_t GC_MAJOR_SCALE = 2048;
static constexpr size_t GC_MIN_TICK    = 1024;

static constexpr uint32_t GC_MAJOR_EVERY_N_MINOR     = 8;
static constexpr uint64_t GC_FORCE_INTERVAL_MS       = 50;
static constexpr uint64_t GC_FORCE_MAJOR_INTERVAL_MS = 1000;

static constexpr size_t GC_NURSERY_THRESHOLD         = 32768;
static constexpr size_t GC_CLOSURE_NURSERY_THRESHOLD = 131072;
static constexpr size_t GC_CLOSURE_PROMOTED_MAJOR    = 262144;

static constexpr size_t GC_CLOSURE_MAJOR_GROWTH   = 16u * 1024u * 1024u;
static constexpr size_t GC_POOL_PRESSURE_FLOOR    = 8u * 1024u * 1024u;
static constexpr size_t GC_ROPE_NURSERY_THRESHOLD = 8u * 1024u * 1024u;

#define GC_OBJ_TYPE_MASK (T_FLAG_FIND(kTypeObject) \
  | T_FLAG_FIND(kTypeArray)                        \
  | T_FLAG_FIND(kTypePromise)                      \
  | T_FLAG_FIND(kTypeGenerator))

typedef struct gc_func_mark_profile {
  bool enabled;
  uint64_t collections;
  uint64_t func_visits;
  uint64_t child_edges;
  uint64_t const_slots;
  uint64_t time_ns;
} gc_func_mark_profile_t;

void gc_run(ant_t *js);
void gc_run_minor(ant_t *js);
void gc_maybe(ant_t *js);
void gc_policy_init(ant_t *js);
double gc_policy_growth_factor(double gc_speed, double allocation_speed);
void gc_pressure(ant_t *js);

void gc_remember_add(ant_t *js, ant_object_t *obj);
void gc_remember_func_const(ant_t *js, sv_func_t *func, uint32_t slot, ant_value_t value);
void gc_remember_upvalue(ant_t *js, struct sv_upvalue *uv);
bool gc_upvalue_is_live(ant_t *js, const struct sv_upvalue *uv);
void gc_remember_coroutine(ant_t *js, struct coroutine *coro);
void gc_forget_coroutine(ant_t *js, struct coroutine *coro);
void gc_remember_closure(ant_t *js, struct sv_closure *c);
void gc_remember_builder(ant_t *js, ant_string_builder_t *builder);
void gc_track_young_closure_slow(ant_t *js, struct sv_closure *c);
void gc_track_young_upvalue_slow(ant_t *js, struct sv_upvalue *uv);

size_t gc_live_major_threshold(ant_t *js);
size_t gc_pool_major_threshold(ant_t *js);

void gc_func_mark_profile_enable(bool enabled);
void gc_func_mark_profile_reset(void);

extern bool gc_disabled;
gc_func_mark_profile_t gc_func_mark_profile_get(void);

// Literal-site feedback lives with bytecode, not with every allocated object.
// Samples are weak: the collector inspects them only after marking, before sweep.
typedef struct {
  uint16_t samples;
  uint16_t survivors;
  uint16_t probe_samples;
  uint16_t probe_survivors;
  uint8_t high_survival_epochs;
  uint8_t probe_tick;
  bool pretenured;
} gc_alloc_site_t;

void gc_track_allocation(ant_t *js, gc_alloc_site_t *site, ant_value_t value);
void gc_allocation_feedback_cleanup(ant_t *js);

// Only unreachable, exclusively owned array backing may cross to the worker.
// Object headers, shapes, references, and finalizers remain on the isolate thread.
void gc_reclaim_array_buffer(ant_t *js, void *buffer, size_t bytes);
void gc_reclaim_flush(ant_t *js);
void gc_reclaim_drain(ant_t *js);
void gc_reclaim_shutdown(ant_t *js);
size_t gc_reclaim_pending_bytes(ant_t *js);

static inline bool gc_value_is_heap_ref(ant_value_t v) {
  if (!is_tagged(v)) return false;
  uint8_t type = vtype_tagged(v);
  return 
    type == kTypeFunction || 
    type == kTypeString   || 
    (((1u << type) & GC_OBJ_TYPE_MASK) != 0);
}

static inline bool gc_value_ref_is_young(ant_value_t v) {
  uint8_t type = vtype_tagged(v);
  if (type == kTypeFunction) return true;
  if (type == kTypeString) return str_is_heap_rope(v) && (ant_str_rope_ptr(v)->flags & ANT_ROPE_FLAG_YOUNG) != 0;
  ant_object_t *ref = (ant_object_t *)vptr(v);
  return ref && ref->flags.generation == 0;
}

static inline void gc_write_barrier(ant_t *js, ant_object_t *writer_obj, ant_value_t new_val) {
  if (writer_obj->flags.generation != 1) return;
  if (gc_value_is_heap_ref(new_val) && gc_value_ref_is_young(new_val)) gc_remember_add(js, writer_obj);
}

#endif
