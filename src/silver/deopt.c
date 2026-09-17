#include "silver/deopt.h"
#include "silver/feedback.h"
#include "gc/roots.h"
#include <string.h>
#include <stdlib.h>

static bool instruction_boundary(const sv_func_t *f, uint32_t offset) {
  if (!f || !f->code || offset >= (uint32_t)f->code_len) return false;
  for (uint32_t pc = 0; pc <= offset;) {
    unsigned op = f->code[pc];
    int size = op < OP__COUNT ? sv_op_size[op] : 0;
    if (!size || pc + (uint32_t)size > (uint32_t)f->code_len) return false;
    if (pc == offset) return true;
    pc += (uint32_t)size;
  }
  return false;
}

static bool range_fits(uint32_t offset, uint32_t count, uint32_t total) {
  return offset <= total && count <= total - offset;
}

static bool recipe_valid(const sv_deopt_recipe_t *recipe, const ant_value_t *values,
                         const ant_value_t *args, int argc) {
  if (!recipe || !values || !recipe->frame || !recipe->owner || argc < 0 ||
      (argc && !args) || recipe->value_count > 4096 ||
      !instruction_boundary(recipe->owner, recipe->owner_offset)) return false;
  unsigned depth = 0;
  for (const sv_deopt_frame_t *f = recipe->frame; f; f = f->child) {
    if (++depth > 32 || !instruction_boundary(f->func, f->offset) ||
        f->func->is_async || f->func->has_await || f->func->is_generator ||
        f->params != f->func->param_count || f->locals != f->func->max_locals ||
        f->stack > f->func->max_stack ||
        !range_fits(f->context_offset, 4, recipe->value_count) ||
        !range_fits(f->state_offset, (uint32_t)f->params + f->locals + f->stack, recipe->value_count) ||
        (!f->root_arguments && !range_fits(f->arguments_offset, f->argc, recipe->value_count))) return false;
    ant_value_t closure = values[f->context_offset];
    if (vtype(closure) != kTypeFunction || js_func_closure(closure)->func != f->func) return false;
    if (f->child) {
      if (!instruction_boundary(f->func, f->call_offset) || (!f->return_child && !f->stack)) return false;
      unsigned op = f->func->code[f->call_offset];
      if (op != OP_CALL && op != OP_CALL_METHOD && op != OP_TAIL_CALL && op != OP_TAIL_CALL_METHOD) return false;
      if (!f->return_child && f->offset != f->call_offset + sv_op_size[op]) return false;
    }
  }
  return true;
}

ant_value_t sv_ssa_resume_frame(sv_vm_t *vm, const sv_deopt_continuation_t *state) {
  const sv_deopt_frame_t *f = state->frame;
  ant_value_t *context = state->values + f->context_offset;
  ant_value_t *slots = state->values + f->state_offset;
  sv_closure_t *closure = js_func_closure(context[0]);
  sv_deopt_continuation_t child = *state;
  child.frame = f->child;
  child.parent_call_offset = f->call_offset;
  child.return_to_parent = f->return_child;
  vm->jit_resume.active = true;
  vm->jit_resume.ip_offset = f->offset;
  vm->jit_resume.params = slots;
  vm->jit_resume.n_params = f->params;
  vm->jit_resume.locals = slots + f->params;
  vm->jit_resume.n_locals = f->locals;
  vm->jit_resume.vstack = slots + f->params + f->locals;
  vm->jit_resume.vstack_sp = f->stack;
  vm->jit_resume.child = f->child ? &child : NULL;
  ant_value_t result = sv_execute_closure_entry(vm, closure, context[0], context[3], context[2], context[1],
      f->root_arguments ? state->root_args : state->values + f->arguments_offset,
      f->root_arguments ? state->root_argc : f->argc, NULL);
  // Frame staging can fail before the interpreter consumes the resume state.
  // None of these borrowed pointers may survive this C invocation.
  if (vm->jit_resume.active) memset(&vm->jit_resume, 0, sizeof(vm->jit_resume));
  return result;
}

static ant_value_t resume_deopt(sv_vm_t *vm, const sv_deopt_recipe_t *recipe,
                               ant_value_t *values, ant_value_t *args, int argc, bool learning) {
  if (!vm || !recipe_valid(recipe, values, args, argc)) return mkval(kTypeError, 0);
  gc_temp_root_scope_t roots;
  gc_temp_root_scope_borrow(vm->js, &roots, values, recipe->value_count);
  sv_jit_on_bailout_at(recipe->owner, learning ? "ssa-learning" : "ssa", (int)recipe->owner_offset);
  sv_func_sidecar_t *sidecar = sv_func_ensure_sidecar(recipe->owner);
  if (sidecar) {
    sidecar->ssa_failed = !learning;
    sidecar->ssa_failed_tfb_version = recipe->owner->tfb_version;
    // A rejected optimizing tier must still allow the ordinary compiler to
    // compile the same feedback. Otherwise one speculative miss disables JIT.
    recipe->owner->jit_compiled_tfb_ver = 0;
  }
  sv_deopt_continuation_t state = {
    .frame = recipe->frame, .values = values, .root_args = args, .root_argc = argc
  };
  ant_value_t result = sv_ssa_resume_frame(vm, &state);
  gc_temp_root_scope_end(&roots);
  return result;
}

ant_value_t jit_helper_ssa_deopt(sv_vm_t *vm, const sv_deopt_recipe_t *recipe,
                               ant_value_t *values, ant_value_t *args, int argc) {
  return resume_deopt(vm, recipe, values, args, argc, false);
}

ant_value_t *jit_helper_ssa_deopt_alloc(ant_t *js, uint32_t count) {
  ant_value_t *values = count && count <= 4096 ? malloc(count * sizeof(*values)) : NULL;
  if (!values) (void)js_mkerr(js, "could not allocate deoptimization state");
  return values;
}

ant_value_t jit_helper_ssa_deopt_owned(sv_vm_t *vm, const sv_deopt_recipe_t *recipe,
                                     ant_value_t *values, ant_value_t *args, int argc, int learning) {
  ant_value_t result = resume_deopt(vm, recipe, values, args, argc, learning != 0);
  free(values);
  return result;
}

int64_t jit_helper_ssa_record_element(sv_func_t *owner, sv_func_t *func, uint32_t offset,
                                     ant_value_t object, ant_value_t key, ant_value_t value, int write) {
  uint8_t *feedback = sv_func_type_feedback(func);
  if (!feedback || offset >= (uint32_t)func->code_len) return 0;
  uint8_t old = feedback[offset];
  if (sv_tfb_specialization_ready(old) || (old & SV_TFB_SPEC_MISMATCH)) return 0;
  if (write) sv_tfb_record_dense_numeric_put(func, func->code + offset, object, key, value);
  else sv_tfb_record_specialization_at(func, feedback + offset, old, sv_tfb_dense_numeric_get(object, key));
  uint8_t now = feedback[offset];
  bool ready = now != old && (sv_tfb_specialization_ready(now) || (now & SV_TFB_SPEC_MISMATCH));
  if (ready && owner != func) owner->tfb_version++;
  return ready;
}
