#ifndef ANT_SILVER_DEOPT_H
#define ANT_SILVER_DEOPT_H

#include "silver/engine.h"

// Immutable code-arena metadata. All offsets address one canonical, boxed
// value buffer supplied by generated code. A parent with a child is suspended
// after its call; the final stack slot is filled with the child's result.
typedef struct sv_deopt_frame {
  sv_func_t *func;
  const struct sv_deopt_frame *child;
  uint32_t offset, call_offset;
  uint32_t context_offset; // closure value, this, new.target, super
  uint32_t state_offset;   // current params, locals, operand stack
  uint32_t arguments_offset;
  uint16_t params, locals, stack, argc;
  bool root_arguments, return_child;
} sv_deopt_frame_t;

typedef struct {
  sv_func_t *owner;
  const sv_deopt_frame_t *frame;
  uint32_t value_count, owner_offset;
} sv_deopt_recipe_t;

// Lives on the deoptimizer's C stack while an interpreter frame runs. The VM
// consumes it before dispatch, so nested frames have normal interpreter
// ownership, exception handling and stack-trace parentage.
typedef struct sv_deopt_continuation {
  const sv_deopt_frame_t *frame;
  ant_value_t *values, *root_args;
  int root_argc;
  uint32_t parent_call_offset;
  bool return_to_parent;
} sv_deopt_continuation_t;

ant_value_t sv_ssa_resume_frame(sv_vm_t *vm, const sv_deopt_continuation_t *state);
ant_value_t jit_helper_ssa_deopt(sv_vm_t *vm, const sv_deopt_recipe_t *recipe,
                               ant_value_t *values, ant_value_t *args, int argc);
ant_value_t *jit_helper_ssa_deopt_alloc(ant_t *js, uint32_t count);
ant_value_t jit_helper_ssa_deopt_owned(sv_vm_t *vm, const sv_deopt_recipe_t *recipe,
                                     ant_value_t *values, ant_value_t *args, int argc, int learning);
int64_t jit_helper_ssa_record_element(sv_func_t *owner, sv_func_t *func, uint32_t offset,
                                     ant_value_t object, ant_value_t key, ant_value_t value, int write);
ant_value_t jit_helper_ssa_get_global(ant_t *js, const char *key, sv_func_t *func, int32_t offset);
ant_value_t jit_helper_ssa_eq(sv_vm_t *vm, ant_t *js, ant_value_t left, ant_value_t right);
ant_value_t jit_helper_ssa_ne(sv_vm_t *vm, ant_t *js, ant_value_t left, ant_value_t right);
ant_value_t jit_helper_ssa_put_field(sv_vm_t *vm, ant_t *js, ant_value_t object,
                                   ant_value_t value, const sv_atom_t *atom, sv_ic_entry_t *ic);
ant_value_t jit_helper_ssa_put_elem(sv_vm_t *vm, ant_t *js, ant_value_t object,
                                  ant_value_t key, ant_value_t value);
ant_value_t jit_helper_ssa_put_global(sv_vm_t *vm, ant_t *js, ant_value_t value,
                                    const char *key, uint32_t length, int strict);

#endif
