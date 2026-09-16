// meson test -C build gc-exact-edges
#include "internal.h"
#include "gc.h"
#include "gc/objects.h"
#include "gc/roots.h"
#include "modules/bigint.h"
#include <string.h>
#include <assert.h>
#include <stdio.h>

static uint64_t conservative_words[2];

static void mark_conservative_words(ant_t *js) {
  gc_mark_conservative_range(js, conservative_words, sizeof(conservative_words));
}

int main(void) {
  ant_t *js = ant_create();
  assert(js);
  // Exact heap edges must retain children without help from C-stack copies.
  js_setstackbase(js, NULL);
  GC_ROOT_SAVE(roots, js);
  ant_value_t root = js_mkobj(js);
  GC_ROOT_PIN(js, root);
  ant_value_t array = js_mkarr(js);
  js_set(js, root, "children", array);
  ant_value_t children[32];
  for (unsigned i = 0; i < 32; i++) {
    children[i] = js_mkobj(js);
    js_set(js, children[i], "parent", root);
    js_arr_push(js, array, children[i]);
  }
  gc_run(js);
  assert(gc_obj_is_marked(js_obj_ptr(array)));
  for (unsigned i = 0; i < 32; i++)
    assert(gc_obj_is_marked(js_obj_ptr(children[i])));

  // The old array's remembered edge must still retain a newly allocated child.
  ant_value_t young = js_mkobj(js);
  js_arr_push(js, array, young);
  gc_run_minor(js);
  assert(gc_obj_is_marked(js_obj_ptr(young)));
  gc_run(js);
  assert(gc_obj_is_marked(js_obj_ptr(young)));

  // Mixed ordinary slots must retain every reference kind without C-stack
  // copies acting as roots, while immediate values remain unchanged.
  js_set(js, root, "text", js_mkstr(js, "retained text", 13));
  js_set(js, root, "bigint", js_mkbigint(js, "1234567890123", 13, false));
  js_set(js, root, "symbol", js_mksym(js, "retained symbol"));
  js_set(js, root, "number", js_mknum(42));
  js_set(js, root, "boolean", js_true);
  js_set(js, root, "null", js_mknull());
  js_set(js, root, "undefined", js_mkundef());
  gc_run(js);
  assert(strcmp(js_getstr(js, js_get(js, root, "text"), NULL), "retained text") == 0);
  uint64_t integer = 0;
  assert(bigint_to_uint64_checked(js, js_get(js, root, "bigint"), &integer));
  assert(integer == UINT64_C(1234567890123));
  assert(strcmp(js_sym_desc(js_get(js, root, "symbol")), "retained symbol") == 0);
  assert(js_getnum(js_get(js, root, "number")) == 42);
  assert(js_get(js, root, "boolean") == js_true);
  assert(js_get(js, root, "null") == js_mknull());
  assert(js_get(js, root, "undefined") == js_mkundef());

  // Raw and tagged conservative candidates still take the validated path.
  ant_value_t raw_root = js_mkobj(js), tagged_root = js_mkobj(js);
  ant_value_t raw_child = js_mkobj(js), tagged_child = js_mkobj(js);
  js_set(js, raw_root, "child", raw_child);
  js_set(js, tagged_root, "child", tagged_child);
  conservative_words[0] = (uintptr_t)js_obj_ptr(raw_root);
  conservative_words[1] = tagged_root;
  gc_objects_run(js, NULL, mark_conservative_words);
  assert(gc_obj_is_marked(js_obj_ptr(raw_root)));
  assert(gc_obj_is_marked(js_obj_ptr(tagged_root)));
  assert(gc_obj_is_marked(js_obj_ptr(raw_child)));
  assert(gc_obj_is_marked(js_obj_ptr(tagged_child)));
  conservative_words[0] = conservative_words[1] = 0;

  GC_ROOT_RESTORE(js, roots);
  js_destroy(js);
  puts("PASS exact object/array edges, remembered children and conservative roots");
}
