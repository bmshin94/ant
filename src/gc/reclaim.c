#include "gc.h"
#include <stdlib.h>

#ifdef ANT_WASM_EMBED

void gc_reclaim_array_buffer(ant_t *js, void *buffer, size_t bytes) { free(buffer); }
void gc_reclaim_flush(ant_t *js) {}
void gc_reclaim_drain(ant_t *js) {}
void gc_reclaim_shutdown(ant_t *js) { js->gc_reclaimer_stopped = true; }
size_t gc_reclaim_pending_bytes(ant_t *js) { return 0; }

#else

#include <stdatomic.h>
#include <uv.h>

enum { RECLAIM_BATCH_COUNT = 256, RECLAIM_MIN_BATCH = 64 };
static constexpr size_t RECLAIM_MAX_PENDING_BYTES = 8u * 1024u * 1024u;

typedef struct reclaim_batch {
  struct reclaim_batch *next;
  size_t count;
  size_t bytes;
  void *buffers[RECLAIM_BATCH_COUNT];
} reclaim_batch_t;

typedef struct ant_gc_reclaimer {
  uv_thread_t thread;
  uv_mutex_t mutex;
  uv_cond_t condition;
  reclaim_batch_t *head;
  reclaim_batch_t *tail;
  reclaim_batch_t *staging; // Accessed only by the owning isolate thread.
  _Atomic size_t pending_bytes;
  bool started;
  bool unavailable;
  bool stopping;
  bool active;
} ant_gc_reclaimer_t;

static void free_batch_buffers(reclaim_batch_t *batch) {
  for (size_t i = 0; i < batch->count; i++) free(batch->buffers[i]);
}

static void reclaim_worker(void *data) {
  ant_gc_reclaimer_t *state = data;
  uv_mutex_lock(&state->mutex);
  for (;;) {
    while (!state->head && !state->stopping)
      uv_cond_wait(&state->condition, &state->mutex);
    if (!state->head && state->stopping) break;
    reclaim_batch_t *batch = state->head;
    state->head = batch->next;
    if (!state->head) state->tail = NULL;
    state->active = true;
    uv_mutex_unlock(&state->mutex);

    // No runtime or object-graph access is permitted on this thread.
    free_batch_buffers(batch);
    atomic_fetch_sub_explicit(&state->pending_bytes, batch->bytes, memory_order_relaxed);
    free(batch);

    uv_mutex_lock(&state->mutex);
    state->active = false;
    uv_cond_broadcast(&state->condition);
  }
  uv_mutex_unlock(&state->mutex);
}

static bool start_worker(ant_gc_reclaimer_t *state) {
  if (state->started) return true;
  if (state->unavailable) return false;
  state->unavailable = true;
  if (uv_mutex_init(&state->mutex)) return false;
  if (uv_cond_init(&state->condition)) {
    uv_mutex_destroy(&state->mutex);
    return false;
  }
  if (uv_thread_create(&state->thread, reclaim_worker, state)) {
    uv_cond_destroy(&state->condition);
    uv_mutex_destroy(&state->mutex);
    return false;
  }
  state->started = true;
  state->unavailable = false;
  return true;
}

static void submit_staging(ant_gc_reclaimer_t *state) {
  reclaim_batch_t *batch = state->staging;
  if (!batch || !batch->count) return;
  if (batch->count < RECLAIM_MIN_BATCH || !start_worker(state)) {
    free_batch_buffers(batch);
    batch->count = batch->bytes = 0;
    return;
  }
  state->staging = NULL;
  batch->next = NULL;
  uv_mutex_lock(&state->mutex);
  atomic_fetch_add_explicit(&state->pending_bytes, batch->bytes, memory_order_relaxed);
  if (state->tail) state->tail->next = batch;
  else state->head = batch;
  state->tail = batch;
  uv_cond_signal(&state->condition);
  uv_mutex_unlock(&state->mutex);
}

void gc_reclaim_array_buffer(ant_t *js, void *buffer, size_t bytes) {
  if (!buffer) return;
  if (!js->gc_running || js->gc_reclaimer_stopped || bytes >= RECLAIM_MAX_PENDING_BYTES) {
    free(buffer);
    return;
  }
  ant_gc_reclaimer_t *state = js->gc_reclaimer;
  if (!state) {
    state = calloc(1, sizeof(*state));
    if (!state) { free(buffer); return; }
    atomic_init(&state->pending_bytes, 0);
    js->gc_reclaimer = state;
  }
  size_t staged_bytes = state->staging ? state->staging->bytes : 0;
  size_t pending = atomic_load_explicit(&state->pending_bytes, memory_order_relaxed);
  if (state->unavailable || pending + staged_bytes > RECLAIM_MAX_PENDING_BYTES - bytes) {
    free(buffer);
    return;
  }
  if (!state->staging) {
    state->staging = calloc(1, sizeof(*state->staging));
    if (!state->staging) { free(buffer); return; }
  }
  state->staging->buffers[state->staging->count++] = buffer;
  state->staging->bytes += bytes;
  if (state->staging->count == RECLAIM_BATCH_COUNT) submit_staging(state);
}

void gc_reclaim_flush(ant_t *js) {
  if (js->gc_reclaimer) submit_staging(js->gc_reclaimer);
}

void gc_reclaim_drain(ant_t *js) {
  ant_gc_reclaimer_t *state = js->gc_reclaimer;
  if (!state) return;
  submit_staging(state);
  if (!state->started) return;
  uv_mutex_lock(&state->mutex);
  while (state->head || state->active)
    uv_cond_wait(&state->condition, &state->mutex);
  uv_mutex_unlock(&state->mutex);
}

size_t gc_reclaim_pending_bytes(ant_t *js) {
  ant_gc_reclaimer_t *state = js->gc_reclaimer;
  if (!state) return 0;
  return atomic_load_explicit(&state->pending_bytes, memory_order_relaxed) +
    (state->staging ? state->staging->bytes : 0);
}

void gc_reclaim_shutdown(ant_t *js) {
  js->gc_reclaimer_stopped = true;
  ant_gc_reclaimer_t *state = js->gc_reclaimer;
  if (!state) return;
  submit_staging(state);
  if (state->started) {
    uv_mutex_lock(&state->mutex);
    state->stopping = true;
    uv_cond_signal(&state->condition);
    uv_mutex_unlock(&state->mutex);
    uv_thread_join(&state->thread);
    uv_cond_destroy(&state->condition);
    uv_mutex_destroy(&state->mutex);
  }
  free(state->staging);
  free(state);
  js->gc_reclaimer = NULL;
}

#endif
