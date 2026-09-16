// meson test -C build gc-event-loop-stack
#include "internal.h"
#include "gc.h"
#include "gc/objects.h"
#include "gc/roots.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <uv.h>

#define CHILDREN 2048

typedef struct {
  ant_t *js;
  ant_value_t keep;
  size_t baseline;
  int fd;
  bool failed;
} test_state_t;

// Leave old object-pointer bits well below a normal callback frame. Inlining
// the 32 KiB kqueue buffer into uv_run makes that dead storage GC-visible.
static __attribute__((noinline)) void leave_dead_stack_values(ant_t *js) {
  volatile uintptr_t scratch[4096];
  GC_ROOT_SAVE(mark, js);
  ant_value_t root = js_mkobj(js);
  GC_ROOT_PIN(js, root);
  ant_value_t array = js_mkarr(js);
  js_set(js, root, "children", array);
  for (unsigned i = 0; i < CHILDREN; i++) js_arr_push(js, array, js_mkobj(js));
  uintptr_t raw = (uintptr_t)js_obj_ptr(root);
  for (unsigned i = 0; i < 4096; i++) scratch[i] = i >= 1024 && i < 2048 ? raw : 0;
  GC_ROOT_RESTORE(js, mark);
  // Do not let the test's own expired local slots provide competing roots.
  *(volatile ant_value_t *)&root = js_mkundef();
  *(volatile ant_value_t *)&array = js_mkundef();
  *(volatile uintptr_t *)&raw = 0;
  (void)scratch;
}

// Keep uv_run out of the caller's frame even if the test is linked with LTO.
static __attribute__((noinline)) void run_loop(uv_loop_t *loop) {
  uv_run(loop, UV_RUN_DEFAULT);
}

static void collect(test_state_t *state, const char *kind) {
  gc_run(state->js);
  assert(gc_obj_is_marked(js_obj_ptr(state->keep)));
  assert(js_getnum(js_get(state->js, state->keep, "value")) == 42);
  size_t retained = state->js->obj_arena.live_count;
  fprintf(stderr, "%s: baseline=%zu retained=%zu\n", kind, state->baseline, retained);
  state->failed = retained > state->baseline + 64;
}

static void on_timer(uv_timer_t *timer) {
  collect(timer->data, "timer");
  uv_close((uv_handle_t *)timer, NULL);
}

static void on_readable(uv_poll_t *poll, int status, int events) {
  test_state_t *state = poll->data;
  assert(status == 0 && (events & UV_READABLE));
  char byte;
  assert(read(state->fd, &byte, 1) == 1);
  collect(state, "io");
  uv_poll_stop(poll);
  uv_close((uv_handle_t *)poll, NULL);
}

static bool run_case(bool io) {
  char stack_base;
  ant_t *js = ant_create();
  assert(js);
  js_setstackbase(js, &stack_base);
  GC_ROOT_SAVE(roots, js);
  ant_value_t keep = js_mkobj(js);
  GC_ROOT_PIN(js, keep);
  js_set(js, keep, "value", js_mknum(42));
  gc_run(js);
  test_state_t state = {.js = js, .keep = keep, .baseline = js->obj_arena.live_count};
  uv_loop_t loop;
  assert(uv_loop_init(&loop) == 0);
  uv_timer_t timer;
  uv_poll_t poll;
  int fds[2] = {-1, -1};
  if (io) {
    assert(pipe(fds) == 0);
    state.fd = fds[0];
    assert(uv_poll_init(&loop, &poll, state.fd) == 0);
    poll.data = &state;
    assert(uv_poll_start(&poll, UV_READABLE, on_readable) == 0);
    assert(write(fds[1], "x", 1) == 1);
  } else {
    assert(uv_timer_init(&loop, &timer) == 0);
    timer.data = &state;
    assert(uv_timer_start(&timer, on_timer, 0, 0) == 0);
  }
  leave_dead_stack_values(js);
  run_loop(&loop);
  assert(uv_loop_close(&loop) == 0);
  if (io) { close(fds[0]); close(fds[1]); }
  GC_ROOT_RESTORE(js, roots);
  js_destroy(js);
  return !state.failed;
}

int main(void) {
  bool timer_ok = run_case(false);
  bool io_ok = run_case(true);
  if (!timer_ok || !io_ok) return 1;
  puts("PASS event-loop scratch storage does not retain dead object graphs");
  return 0;
}
