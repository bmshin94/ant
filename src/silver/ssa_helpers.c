#include "silver/deopt.h"
#include "silver/glue.h"
#include "silver/feedback.h"
#include "ops/property.h"
#include "ops/globals.h"
#include <math.h>

ant_value_t jit_helper_ssa_eq(sv_vm_t *vm, ant_t *js, ant_value_t left, ant_value_t right) {
  uint8_t lt = vtype(left), rt = vtype(right);
  bool nullish = lt == kTypeNull || lt == kTypeUndefined || rt == kTypeNull || rt == kTypeUndefined;
  bool object = is_object_type(left) || lt == kTypeBuiltin || is_object_type(right) || rt == kTypeBuiltin;
  if (lt != rt && object && !nullish) return SV_JIT_BAILOUT;
  return jit_helper_eq(vm, js, left, right);
}

ant_value_t jit_helper_ssa_ne(sv_vm_t *vm, ant_t *js, ant_value_t left, ant_value_t right) {
  ant_value_t equal = jit_helper_ssa_eq(vm, js, left, right);
  return equal == SV_JIT_BAILOUT || is_err(equal) ? equal : js_bool(vdata(equal) == 0);
}

// These helpers either finish without invoking user code or return the retry
// sentinel before the operation. The SSA memory model depends on that boundary;
// accessors, proxies, allocation of a new property, and readonly stores resume
// in a real interpreter frame with the original strictness and effects.
static bool own_writable_data(ant_value_t value, const sv_atom_t *atom) {
  uint8_t type = vtype(value);
  if (type != kTypeObject && type != kTypeArray) return false;
  ant_object_t *object = js_obj_ptr(value);
  if (!object || !object->shape || object->flags.is_exotic || object->flags.frozen ||
      (type == kTypeArray && is_length_key(atom->str, atom->len))) return false;
  int32_t index = ant_shape_lookup_interned(object->shape, atom->str);
  if (index < 0 || (uint32_t)index >= object->prop_count) return false;
  const ant_shape_prop_t *property = ant_shape_prop_at(object->shape, (uint32_t)index);
  return property && !property->has_getter && !property->has_setter &&
      (property->attrs & ANT_PROP_ATTR_WRITABLE);
}

ant_value_t jit_helper_ssa_get_global(ant_t *js, const char *key, sv_func_t *func, int32_t offset) {
  if (!func || offset < 0 || offset > func->code_len - 5 ||
      (func->code[offset] != OP_GET_GLOBAL && func->code[offset] != OP_GET_GLOBAL_UNDEF))
    return SV_JIT_BAILOUT;
  uint32_t index = sv_get_u32(func->code + offset + 1);
  if (index >= (uint32_t)func->atom_count) return SV_JIT_BAILOUT;
  ant_value_t result = js_mkundef();
  if (!sv_try_get_data_prop_no_effect_interned(js, js->global, key, func->atoms[index].len, &result))
    return SV_JIT_BAILOUT;
  if (func->code[offset] == OP_GET_GLOBAL && is_undefined(result) && !lkp_interned(js->global, key).obj)
    return SV_JIT_BAILOUT;
  return result;
}

ant_value_t jit_helper_ssa_put_field(sv_vm_t *vm, ant_t *js, ant_value_t object,
                                   ant_value_t value, const sv_atom_t *atom, sv_ic_entry_t *ic) {
  if (!own_writable_data(object, atom)) return SV_JIT_BAILOUT;
  return jit_helper_put_field_ic(vm, js, object, value, atom, ic);
}

ant_value_t jit_helper_ssa_put_elem(sv_vm_t *vm, ant_t *js, ant_value_t object,
                                  ant_value_t key, ant_value_t value) {
  const char *interned = NULL;
  size_t length = 0;
  if (vtype(key) == kTypeNumber) {
    double number = tod(key);
    if (!(number >= 0 && number < UINT32_MAX && number == (double)(uint32_t)number))
      return SV_JIT_BAILOUT;
    char text[16];
    length = uint_to_str(text, sizeof(text), (uint32_t)number);
    interned = intern_find(text, length);
  } else if (vtype(key) == kTypeString) {
    ant_offset_t size = 0;
    const char *text = (const char *)(uintptr_t)vstr(js, key, &size);
    length = size;
    interned = intern_find(text, length);
  } else return SV_JIT_BAILOUT;
  if (!interned) return SV_JIT_BAILOUT;
  sv_atom_t atom = {.str = interned, .len = (uint32_t)length};
  return jit_helper_ssa_put_field(vm, js, object, value, &atom, NULL);
}

ant_value_t jit_helper_ssa_put_global(sv_vm_t *vm, ant_t *js, ant_value_t value,
                                    const char *key, uint32_t length, int strict) {
  (void)vm;
  (void)strict;
  sv_atom_t atom = {.str = key, .len = length};
  if (!own_writable_data(js->global, &atom)) return SV_JIT_BAILOUT;
  return sv_global_try_store_own_data(js, js->global, key, length, value) ? value : SV_JIT_BAILOUT;
}
