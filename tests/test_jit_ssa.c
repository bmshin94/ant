#include "../src/jit/ssa.h"
#include <assert.h>
#include <string.h>
#include "gc/roots.h"
#include "silver/deopt.h"
#include "silver/feedback.h"
#include "silver/jit.h"
#include "silver/glue.h"
#include "silver/call.h"
#include <math.h>

sv_jit_func_t jit_ssa_compile(ant_t *js, sv_func_t *func);

static ant_value_t expected_root;
static int root_visits;
static void visit_root(ant_t *js, ant_value_t value) {
  (void)js;
  root_visits += value == expected_root;
}

static void check_borrowed_roots(ant_t *js) {
  gc_temp_root_scope_t owned, borrowed;
  gc_temp_root_scope_begin(js, &owned);
  assert(gc_temp_root_handle_valid(gc_temp_root_add(&owned, js_mkundef())));
  ant_value_t values[] = {tov(987654.25), js_mkundef()};
  gc_temp_root_scope_borrow(js, &borrowed, values, 2);
  assert(!gc_temp_root_handle_valid(gc_temp_root_add(&borrowed, js_true)));
  expected_root = values[0];
  root_visits = 0;
  gc_visit_roots(js, visit_root);
  assert(root_visits == 1);
  values[0] = tov(876543.25);
  expected_root = values[0];
  root_visits = 0;
  gc_visit_roots(js, visit_root);
  assert(root_visits == 1);
  gc_temp_root_scope_end(&borrowed);
  assert(js->temp_roots == &owned);
  root_visits = 0;
  gc_visit_roots(js, visit_root);
  assert(root_visits == 0);
  gc_temp_root_scope_end(&owned);
}

static void check_source(ant_t *js, const char *source, bool loop, bool call) {
  ant_value_t value = js_eval_bytecode_eval(js, source, strlen(source));
  assert(vtype(value) == kTypeFunction);
  sv_func_t *f = js_func_closure(value)->func;
  ssa_graph_t graph;
  if (!ssa_graph_build(&graph, f)) {
    fprintf(stderr, "SSA rejected %s: %s\n", source, graph.failure);
    assert(false);
  }
  assert(ssa_graph_verify(&graph, stderr));
  int phis = 0, calls = 0, backedges = 0;
  for (ssa_id_t i = 1; i < graph.node_count; i++) {
    ssa_node_t *n = &graph.nodes[i];
    phis += n->op == SSA_PHI && !n->replacement;
    calls += n->op == SSA_CALL;
  }
  for (uint32_t i = 2; i < graph.block_count; i++)
    for (uint32_t e = 0; e < graph.blocks[i].successors.count; e++)
      backedges += graph.blocks[i].successors.data[e] <= i;
  if (loop) assert(phis && backedges);
  if (call) assert(calls);
  for (uint32_t i = 1; i < graph.snapshot_count; i++) {
    ssa_snapshot_t *s = &graph.snapshots[i];
    assert(s->params == f->param_count && s->locals == f->max_locals);
    assert(s->values.count == (uint32_t)s->params + s->locals + s->stack);
  }
  for (ssa_id_t i = 1; i < graph.node_count; i++) {
    ssa_node_t *n = &graph.nodes[i];
    if (n->replacement || !n->snapshot) continue;
    ssa_snapshot_t *snapshot = &graph.snapshots[n->snapshot];
    if (!snapshot->values.count) continue;
    ssa_id_t saved = snapshot->values.data[0];
    snapshot->values.data[0] = i;
    assert(!ssa_graph_verify(&graph, NULL)); // Cannot deopt with a not-yet-produced result.
    snapshot->values.data[0] = saved;
    assert(ssa_graph_verify(&graph, stderr));
    break;
  }
  ssa_graph_destroy(&graph);
}

static void check_invalid(void) {
  uint8_t code[] = {OP_JMP, 1, 0, 0, 0, OP_CONST_I8, 1, OP_RETURN};
  sv_func_t f = {.code = code, .code_len = sizeof(code), .max_stack = 1};
  ssa_graph_t g;
  assert(!ssa_graph_build(&g, &f));
  assert(strcmp(g.failure, "branch into instruction") == 0);
  ssa_graph_destroy(&g);
  code[1] = 0;
  assert(ssa_graph_build(&g, &f));
  assert(ssa_graph_verify(&g, stderr));
  ssa_graph_destroy(&g);
  uint8_t underflow[] = {OP_POP, OP_RETURN_UNDEF};
  f.code = underflow;
  f.code_len = sizeof(underflow);
  assert(!ssa_graph_build(&g, &f));
  ssa_graph_destroy(&g);
}

static int find_op(sv_func_t *f, sv_op_t wanted) {
  for (int pc = 0; pc < f->code_len; pc += sv_op_size[f->code[pc]])
    if (f->code[pc] == wanted) return pc;
  return -1;
}

static void make_recipe_frame(sv_deopt_frame_t *frame, ant_value_t function,
                              ant_value_t *values, uint32_t *used, int offset, int stack) {
  sv_func_t *f = js_func_closure(function)->func;
  *frame = (sv_deopt_frame_t){.func = f, .offset = (uint32_t)offset,
      .context_offset = *used, .params = f->param_count, .locals = (uint16_t)f->max_locals,
      .stack = (uint16_t)stack, .argc = 1};
  values[(*used)++] = function;
  for (int i = 0; i < 3; i++) values[(*used)++] = js_mkundef();
  frame->state_offset = *used;
  for (int i = 0; i < f->param_count; i++) values[(*used)++] = tov(7);
  for (int i = 0; i < f->max_locals + stack; i++) values[(*used)++] = js_mkundef();
  frame->arguments_offset = *used;
  values[(*used)++] = tov(7);
}

static void check_deopt_frames(ant_t *js) {
  const char *sources[] = {
    "(function ssa_outer(a) { globalThis.ssa_marker+=1; return globalThis.ssa_middle(a)+4; })",
    "(function ssa_middle(a) { globalThis.ssa_marker+=10; return globalThis.ssa_inner(a)+2; })",
    "(function ssa_inner(a) { globalThis.ssa_marker+=100; return a*2; })"
  };
  const char *names[] = {"ssa_outer", "ssa_middle", "ssa_inner"};
  ant_value_t functions[3], values[256], arg = tov(7);
  sv_deopt_frame_t frames[3];
  uint32_t used = 0;
  for (int i = 0; i < 3; i++) {
    functions[i] = js_eval_bytecode_eval(js, sources[i], strlen(sources[i]));
    assert(vtype(functions[i]) == kTypeFunction);
    js_set(js, js->global, names[i], functions[i]);
    sv_func_t *f = js_func_closure(functions[i])->func;
    int pc = find_op(f, i < 2 ? OP_CALL_METHOD : OP_GET_ARG);
    assert(pc >= 0);
    int resume = i < 2 ? pc + sv_op_size[f->code[pc]] : pc;
    make_recipe_frame(&frames[i], functions[i], values, &used, resume, i < 2 ? 1 : 0);
    frames[i].call_offset = (uint32_t)pc;
    if (i < 2) frames[i].child = &frames[i + 1];
  }
  frames[0].root_arguments = true;
  sv_deopt_recipe_t recipe = {.owner = frames[0].func, .frame = frames,
      .owner_offset = frames[0].call_offset, .value_count = used};
  js_set(js, js->global, "ssa_marker", tov(111));
  int old_fp = js->vm->fp, old_sp = js->vm->sp;
  ant_value_t result = jit_helper_ssa_deopt(js->vm, &recipe, values, &arg, 1);
  assert(vtype(result) == kTypeNumber && tod(result) == 20);
  assert(tod(js_get(js, js->global, "ssa_marker")) == 111); // No effect replay.
  assert(js->vm->fp == old_fp && js->vm->sp == old_sp && !js->vm->jit_resume.active);

  const char *args_source = "(function ssa_args(a) { a=5; return a+1; })";
  ant_value_t fn = js_eval_bytecode_eval(js, args_source, strlen(args_source));
  assert(vtype(fn) == kTypeFunction);
  used = 0;
  sv_func_t *f = js_func_closure(fn)->func;
  int pc = find_op(f, OP_GET_ARG);
  assert(pc >= 0);
  make_recipe_frame(&frames[0], fn, values, &used, pc, 0);
  frames[0].root_arguments = true;
  values[frames[0].state_offset] = tov(5);
  ant_value_t original_args[] = {tov(7), tov(9)};
  recipe = (sv_deopt_recipe_t){.owner = f, .frame = frames, .owner_offset = (uint32_t)pc, .value_count = used};
  result = jit_helper_ssa_deopt(js->vm, &recipe, values, original_args, 2);
  assert(vtype(result) == kTypeNumber && tod(result) == 6);
  assert(js->vm->fp == old_fp && js->vm->sp == old_sp);

  // At entry the hidden arguments binding has not been created yet. Restore
  // the actual argument count even though the function has just one formal.
  args_source = "(function ssa_argc(a) { return arguments.length; })";
  fn = js_eval_bytecode_eval(js, args_source, strlen(args_source));
  assert(vtype(fn) == kTypeFunction);
  used = 0;
  f = js_func_closure(fn)->func;
  make_recipe_frame(&frames[0], fn, values, &used, 0, 0);
  frames[0].root_arguments = true;
  recipe = (sv_deopt_recipe_t){.owner = f, .frame = frames, .owner_offset = 0, .value_count = used};
  result = jit_helper_ssa_deopt(js->vm, &recipe, values, original_args, 2);
  assert(vtype(result) == kTypeNumber && tod(result) == 2);
  assert(js->vm->fp == old_fp && js->vm->sp == old_sp);
}

static void check_native(ant_t *js, const char *source, ant_value_t *args, int argc, ant_value_t expected) {
  ant_value_t fn = js_eval_bytecode_eval(js, source, strlen(source));
  assert(vtype(fn) == kTypeFunction);
  gc_temp_root_scope_t roots;
  gc_temp_root_scope_begin(js, &roots);
  assert(gc_temp_root_handle_valid(gc_temp_root_add(&roots, fn)));
  for (int i = 0; i < argc; i++) assert(gc_temp_root_handle_valid(gc_temp_root_add(&roots, args[i])));
  sv_closure_t *closure = js_func_closure(fn);
  sv_jit_func_t native = jit_ssa_compile(js, closure->func);
  if (!native) { fprintf(stderr, "SSA lowering rejected: %s\n", source); assert(native); }
  int old_fp = js->vm->fp, old_sp = js->vm->sp;
  sv_jit_enter(js);
  ant_value_t result = native(js->vm, js_mkundef(), js_mkundef(), js_mkundef(), args, argc, closure);
  sv_jit_leave(js);
  if (vtype(expected) == kTypeString)
    assert(js_truthy(js, jit_helper_seq(js->vm, js, result, expected)));
  else if (vtype(expected) == kTypeNumber && isnan(tod(expected)))
    assert(vtype(result) == kTypeNumber && isnan(tod(result)));
  else if (result != expected) {
    fprintf(stderr, "SSA result mismatch: %s got %a expected %a\n", source, tod(result), tod(expected));
    assert(result == expected);
  }
  assert(js->temp_roots == &roots);
  assert(js->vm->fp == old_fp && js->vm->sp == old_sp && !js->vm->jit_resume.active);
  // An invalid OSR offset must not run the entry prefix or touch locals.
  js->vm->jit_osr.active = true;
  js->vm->jit_osr.bc_offset = closure->func->code_len + 1;
  sv_jit_enter(js);
  result = native(js->vm, js_mkundef(), js_mkundef(), js_mkundef(), args, argc, closure);
  sv_jit_leave(js);
  assert(result == SV_JIT_RETRY_INTERP && !js->vm->jit_osr.active);
  assert(js->temp_roots == &roots);
  gc_temp_root_scope_end(&roots);
}

static void check_lowering(ant_t *js) {
  sv_jit_init(js);
  ant_value_t args[] = {tov(3), tov(7), tov(5)};
  check_native(js, "(function(a,b) { return a+b; })", args, 2, tov(10));
  check_native(js, "(function(a,n) { var s=0; for(var i=0;i<n;i++) s+=a; return s; })", args, 2, tov(21));
  check_native(js, "(function(a,b,n) { while(n--) { var t=a; a=b; b=t; } return a*10+b; })", args, 3, tov(73));
  check_native(js, "(function(a,b) { var s=0; for(var i=0;i<a;i++) for(var j=0;j<b;j++) s+=i+j; return s; })", args, 2, tov(84));
  check_native(js, "(function(a,b) { return (a<<b)^-1; })", args, 2, tov(-385));
  check_native(js, "(function(a) { if(a) return a*2; return 5; })", args, 1, tov(6));
  check_native(js, "(function(a,b) { return b===undefined; })", args, 1, js_true);
  args[0] = tov(0);
  check_native(js, "(function(a) { return -a; })", args, 1, tov(-0.0));
  check_native(js, "(function(a) { return a/a; })", args, 1, tov((double)NAN));
  check_native(js, "(function(a) { return !a; })", args, 1, js_true);
  args[0] = js_mknull();
  check_native(js, "(function(a) { return a==null; })", args, 1, js_true);
  check_native(js, "(function(a) { return a?.value; })", args, 1, js_mkundef());
  args[0] = js_mkundef();
  check_native(js, "(function(a) { return a!=null; })", args, 1, js_false);
  check_native(js, "(function(a) { return a?.value; })", args, 1, js_mkundef());
  args[0] = js_mkstr(js, "hello", 5);
  check_native(js, "(function(a) { return a+1; })", args, 1, js_mkstr(js, "hello1", 6));
  const char *array_source = "[1,2,'x',3]";
  args[0] = js_eval_bytecode_eval(js, array_source, strlen(array_source));
  args[1] = tov(4);
  check_native(js, "(function(a,n) { var s=0; for(var i=0;i<n;i++) s+=a[i]; return s; })",
               args, 2, js_mkstr(js, "3x3", 3));
}

static void check_nested_inline(ant_t *js) {
  const char *sources[] = {
    "(function ssaDriver(obj,n) { var sum=0; for(var i=0;i<n;i++) sum+=obj.middle(i); return sum; })",
    "(function ssaMiddle(x) { var y=this.leaf(x); return y*2; })",
    "(function ssaLeaf(x) { this.hits++; return x+this.delta; })"
  };
  ant_value_t functions[3];
  gc_temp_root_scope_t roots;
  gc_temp_root_scope_begin(js, &roots);
  for (unsigned i = 0; i < 3; i++) {
    functions[i] = js_eval_bytecode_eval(js, sources[i], strlen(sources[i]));
    assert(vtype(functions[i]) == kTypeFunction);
    assert(gc_temp_root_handle_valid(gc_temp_root_add(&roots, functions[i])));
  }
  for (unsigned i = 0; i < 2; i++) {
    sv_func_t *f = js_func_closure(functions[i])->func;
    int offset = find_op(f, OP_CALL_METHOD);
    assert(offset >= 0);
    sv_tfb_record_call_target(f, offset, js_func_closure(functions[i + 1])->func);
  }
  sv_func_t *f = js_func_closure(functions[0])->func;
  ssa_graph_t graph;
  assert(ssa_graph_build(&graph, f));
  assert(ssa_inline_calls(&graph));
  assert(graph.inlined_calls == 2 && graph.frame_count == 4);
  assert(ssa_graph_verify(&graph, stderr));
  ssa_graph_destroy(&graph);
  ant_value_t object = js_mkobj(js);
  assert(gc_temp_root_handle_valid(gc_temp_root_add(&roots, object)));
  js_set(js, object, "middle", functions[1]);
  js_set(js, object, "leaf", functions[2]);
  js_set(js, object, "hits", tov(0));
  js_set(js, object, "delta", tov(1));
  sv_jit_func_t native = jit_ssa_compile(js, f);
  assert(native);
  ant_value_t args[] = {object, tov(3)};
  sv_jit_enter(js);
  ant_value_t result = native(js->vm, js_mkundef(), js_mkundef(), js_mkundef(), args, 2, js_func_closure(functions[0]));
  sv_jit_leave(js);
  assert(vtype(result) == kTypeNumber && tod(result) == 12);
  assert(f->jit_bailout_count == 0); // The trained targets actually ran inline.
  assert(tod(js_get(js, object, "hits")) == 3);
  js_set(js, object, "hits", tov(0));
  js_set(js, object, "delta", js_mkstr(js, "1", 1));
  sv_jit_enter(js);
  result = native(js->vm, js_mkundef(), js_mkundef(), js_mkundef(), args, 2, js_func_closure(functions[0]));
  sv_jit_leave(js);
  assert(vtype(result) == kTypeNumber && tod(result) == 66);
  assert(tod(js_get(js, object, "hits")) == 3); // Leaf prefix was not replayed.
  assert(js->temp_roots == &roots);
  gc_temp_root_scope_end(&roots);
}

static void check_optimizer(ant_t *js) {
  const char *sources[] = {
    "(function(a) { return a.x+a.x; })",
    "(function(a,b) { var x=a.x; b.x=8; return x+a.x; })",
    "(function(a,n) { var s=0; for(var i=0;i<n;i++) s+=a[i]; return s; })",
    "(function(count,start) { var sum=0; while(--count>=0) { var n=start, product=n; while(--n>1) product*=n; sum+=product; } return sum; })"
  };
  for (unsigned i = 0; i < 4; i++) {
    ant_value_t fn = js_eval_bytecode_eval(js, sources[i], strlen(sources[i]));
    assert(vtype(fn) == kTypeFunction);
    ssa_graph_t g;
    sv_func_t *function = js_func_closure(fn)->func;
    if (i == 2) {
      sv_tfb_ensure(function);
      // Model the same dense-numeric samples that the interpreter records.
      int offset = find_op(function, OP_GET_ELEM);
      assert(offset >= 0);
      for (unsigned sample = 0; sample < 8; sample++) {
        uint8_t old = sv_func_type_feedback(function)[offset];
        sv_tfb_record_specialization_at(function, &sv_func_type_feedback(function)[offset], old, true);
      }
    }
    assert(ssa_graph_build(&g, function));
    assert(ssa_optimize(&g));
    assert(ssa_graph_verify(&g, stderr));
    if (i == 0) assert(g.eliminated_loads == 1);
    if (i == 1) assert(g.eliminated_loads == 0);
    if (i == 2) {
      bool unboxed = false, bounds = false;
      for (ssa_id_t id = 1; id < g.node_count; id++) {
        if (g.nodes[id].replacement) continue;
        unboxed |= g.nodes[id].op == SSA_PHI && g.nodes[id].rep == SSA_F64;
        bounds |= g.nodes[id].op == OP_GET_ELEM && (g.nodes[id].flags & SSA_F_NO_BOUNDS);
      }
      assert(g.hoisted_guards && unboxed && bounds);
    }
    if (i == 3) {
      unsigned recurrences = 0;
      for (ssa_id_t id = 1; id < g.node_count; id++) {
        const ssa_node_t *n = &g.nodes[id];
        if (n->replacement || (n->op != OP_DEC && n->op != OP_MUL)) continue;
        const ssa_node_t *input = &g.nodes[ssa_resolve(&g, n->inputs.data[0])];
        assert(input->op == SSA_PHI && input->rep == SSA_F64);
        recurrences++;
      }
      assert(recurrences == 3);
    }
    ssa_graph_destroy(&g);
  }
  const char *source = "(function(a) { return a.x+a.y; })";
  ant_value_t fn = js_eval_bytecode_eval(js, source, strlen(source));
  ant_value_t object = js_mkobj(js);
  gc_temp_root_scope_t roots;
  gc_temp_root_scope_begin(js, &roots);
  assert(gc_temp_root_handle_valid(gc_temp_root_add(&roots, fn)));
  assert(gc_temp_root_handle_valid(gc_temp_root_add(&roots, object)));
  js_set(js, object, "x", tov(2)); js_set(js, object, "y", tov(3));
  for (unsigned i = 0; i < 20; i++) {
    ant_value_t result = sv_vm_call(js->vm, js, fn, js_mkundef(), &object, 1, NULL, js_mkundef());
    assert(vtype(result) == kTypeNumber && tod(result) == 5);
  }
  ssa_graph_t g;
  assert(ssa_graph_build(&g, js_func_closure(fn)->func));
  assert(ssa_optimize(&g));
  unsigned guards = 0, loads = 0;
  for (ssa_id_t id = 1; id < g.node_count; id++) if (!g.nodes[id].replacement) {
    guards += g.nodes[id].op == SSA_SHAPE_GUARD;
    loads += g.nodes[id].op == SSA_LOAD_FIELD;
  }
  assert(guards == 1 && loads == 2);
  ssa_graph_destroy(&g);
  source = "(function(a,b) { return a==b; })";
  fn = js_eval_bytecode_eval(js, source, strlen(source));
  assert(gc_temp_root_handle_valid(gc_temp_root_add(&roots, fn)));
  sv_func_t *equality = js_func_closure(fn)->func;
  sv_jit_func_t native = jit_ssa_compile(js, equality);
  assert(native);
  ant_value_t arguments[] = {object, object};
  sv_jit_enter(js);
  ant_value_t equal = native(js->vm, js_mkundef(), js_mkundef(), js_mkundef(), arguments, 2, js_func_closure(fn));
  sv_jit_leave(js);
  assert(equal == js_true && equality->jit_bailout_count == 0);
  arguments[1] = js->global;
  sv_jit_enter(js);
  equal = native(js->vm, js_mkundef(), js_mkundef(), js_mkundef(), arguments, 2, js_func_closure(fn));
  sv_jit_leave(js);
  assert(equal == js_false && equality->jit_bailout_count == 0);
  gc_temp_root_scope_end(&roots);
}

int main(void) {
  ant_t *js = ant_create();
  assert(js);
  check_borrowed_roots(js);
  check_source(js, "(function(a) { return a + 1; })", false, false);
  check_source(js, "(function(a,n) { var s=0; for(var i=0;i<n;i++) s+=a; return s; })", true, false);
  check_source(js, "(function(a) { var x; if(a) x=1; else x=2; return x; })", false, false);
  check_source(js, "(function(a,n) { for(var i=0;i<n;i++) a.run(i); return a.value; })", true, true);
  check_source(js, "(function(a) { this.x=a; return this.get().value; })", false, true);
  check_source(js, "(function(a,n) { let s=0; for(let i=0;i<n;i++) s+=a[i]; return s; })", true, false);
  check_source(js, "(function(a,n) { var s=0; for(var i=0;i<n;i++) { for(var j=0;j<n;j++) s+=a[i]+a[j]; } return s; })", true, false);
  check_invalid();
  check_deopt_frames(js);
  check_lowering(js);
  check_nested_inline(js);
  check_optimizer(js);
  js_destroy(js);
  puts("SSA graph, native lowering and deopt tests passed");
  return 0;
}
