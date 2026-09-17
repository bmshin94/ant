#include "compile.h"
#include "ssa.h"
#include "silver/deopt.h"
#include "gc/roots.h"
#include "runtime.h"
#include "debug.h"
#include "shapes.h"

// Lower selected representations and box only at JS boundaries or deopt. Each
// checked operation owns a pre-effect snapshot. Unsupported graphs decline
// before opening a MIR module; the ordinary compiler remains the fallback.
typedef struct {
  jit_compile_t c;
  ssa_graph_t *g;
  MIR_reg_t *values, *copies, *boxed;
  MIR_label_t *blocks, *bailouts;
  const sv_deopt_recipe_t **recipes;
  uint32_t spill_count;
  unsigned argument_count;
  int conversion_site;
  MIR_item_t deopt_proto, deopt, roots_proto, roots, end_proto, end;
  MIR_item_t spill_proto, allocate_spill;
  MIR_item_t record_proto, record_element;
  MIR_reg_t learning;
  MIR_item_t global, put_field, put_elem, put_global;
  MIR_item_t equal, not_equal;
  MIR_reg_t args_buf, epoch;
  MIR_reg_t scratch, null_cmp, left, right, number, result, spill, scope, contexts;
  MIR_label_t exit, reject;
} ssa_lower_t;

#define CTX s->c.ctx
#define FN s->c.jit_func
#define R(r) MIR_new_reg_op(CTX, (r))
#define U(v) MIR_new_uint_op(CTX, (uint64_t)(v))
#define I(v) MIR_new_int_op(CTX, (int64_t)(v))
#define L(l) MIR_new_label_op(CTX, (l))
#define E(code, ...) MIR_append_insn(CTX, FN, MIR_new_insn(CTX, (code), __VA_ARGS__))

static MIR_reg_t new_reg(ssa_lower_t *s, MIR_type_t type, const char *prefix, uint32_t id) {
  char name[64];
  snprintf(name, sizeof(name), "%s_%u", prefix, id);
  return MIR_new_func_reg(CTX, FN->u.func, type == MIR_T_P ? MIR_T_I64 : type, name);
}

static MIR_reg_t value_reg(ssa_lower_t *s, ssa_id_t id) {
  id = ssa_resolve(s->g, id);
  ssa_node_t *n = &s->g->nodes[id];
  if (n->rep == SSA_BOXED || n->rep == SSA_PTR) return s->values[id];
  MIR_reg_t dst = s->boxed[id];
  if (n->rep == SSA_BOOL) E(MIR_OR, R(dst), R(s->values[id]), U(js_false));
  else if (n->rep == SSA_F64) mir_d_to_i64(CTX, FN, dst, s->values[id], s->c.r_d_slot);
  else {
    E(MIR_I2D, R(s->number), R(s->values[id]));
    mir_d_to_i64_non_nan(CTX, FN, dst, s->number, s->c.r_d_slot);
  }
  return dst;
}

static void as_double(ssa_lower_t *s, ssa_id_t id, MIR_reg_t dst, MIR_label_t slow) {
  id = ssa_resolve(s->g, id);
  const ssa_node_t *n = &s->g->nodes[id];
  MIR_reg_t src = n->rep == SSA_BOOL ? value_reg(s, id) : s->values[id];
  if (n->rep == SSA_F64) E(MIR_DMOV, R(dst), R(src));
  else if (n->rep == SSA_I32 || n->rep == SSA_I53) E(MIR_I2D, R(dst), R(src));
  else {
    if (n->type != SSA_T_NUMBER) mir_emit_is_num_guard(CTX, FN, s->scratch, src, slow);
    mir_i64_to_d(CTX, FN, dst, src, s->c.r_d_slot);
  }
}

static MIR_type_t representation_type(const ssa_node_t *n) {
  return n->rep == SSA_F64 ? MIR_T_D : MIR_JSVAL;
}

static MIR_reg_t array_index(ssa_lower_t *s, ssa_id_t value, MIR_label_t slow, int site) {
  value = ssa_resolve(s->g, value);
  const ssa_node_t *n = &s->g->nodes[value];
  if (n->rep == SSA_I32 || n->rep == SSA_I53) {
    MIR_reg_t integer = s->values[value];
    if (!n->range_known || n->min < 0 || n->max >= UINT32_MAX)
      E(MIR_UBGE, L(slow), R(integer), U(UINT32_MAX));
    return integer;
  }
  bool number = n->rep == SSA_F64;
  return mir_emit_array_index_guard(CTX, FN, number ? 0 : value_reg(s, value),
      number ? s->values[value] : 0, number, s->c.r_d_slot, slow, site);
}

static bool edge_needs_guard(const ssa_graph_t *g, uint32_t from) {
  const ssa_block_t *b = &g->blocks[from];
  for (unsigned e = 0; e < b->successors.count; e++) {
    const ssa_block_t *to = &g->blocks[b->successors.data[e]];
    for (unsigned pi = 0; pi < to->predecessors.count; pi++) if (to->predecessors.data[pi] == from)
      for (unsigned i = 0; i < to->phis.count; i++) {
        const ssa_node_t *phi = &g->nodes[to->phis.data[i]];
        if (phi->replacement || phi->rep == SSA_BOXED) continue;
        ssa_id_t value = ssa_resolve(g, phi->inputs.data[pi]);
        if (g->nodes[value].rep == SSA_BOXED) return true;
      }
  }
  return false;
}

static MIR_op_t mem(ssa_lower_t *s, MIR_type_t type, MIR_reg_t base, size_t offset) {
  return MIR_new_mem_op(CTX, type, (MIR_disp_t)offset, base, 0, 1);
}

static void call(ssa_lower_t *s, MIR_item_t proto, MIR_item_t target,
                 MIR_reg_t result, unsigned count, const MIR_op_t *arguments) {
  MIR_op_t ops[12];
  unsigned at = 0;
  ops[at++] = MIR_new_ref_op(CTX, proto);
  ops[at++] = MIR_new_ref_op(CTX, target);
  if (result) ops[at++] = R(result);
  for (unsigned i = 0; i < count; i++) ops[at++] = arguments[i];
  MIR_append_insn(CTX, FN, MIR_new_insn_arr(CTX, MIR_CALL, at, ops));
}

static void record_element(ssa_lower_t *s, ssa_id_t id, bool write) {
  const ssa_node_t *n = &s->g->nodes[id];
  sv_func_t *f = s->g->frames[n->frame].func;
  sv_tfb_ensure(f);
  uint8_t *feedback = sv_func_type_feedback(f);
  if (!feedback || sv_tfb_specialization_ready(feedback[n->offset]) || (feedback[n->offset] & SV_TFB_SPEC_MISMATCH)) return;
  MIR_label_t done = MIR_new_label(CTX);
  MIR_reg_t slot = new_reg(s, MIR_T_I64, "ssa_feedback", id);
  E(MIR_MOV, R(slot), U((uintptr_t)(feedback + n->offset)));
  E(MIR_MOV, R(s->scratch), mem(s, MIR_T_U8, slot, 0));
  E(MIR_UBGE, L(done), R(s->scratch), U(SV_TFB_SPEC_MIN_SAMPLES << SV_TFB_SPEC_COUNT_SHIFT));
  MIR_op_t args[] = {U((uintptr_t)s->c.func), U((uintptr_t)f), U(n->offset),
      R(value_reg(s, n->inputs.data[0])), R(value_reg(s, n->inputs.data[1])),
      write ? R(value_reg(s, n->inputs.data[2])) : U(js_mkundef()), I(write)};
  call(s, s->record_proto, s->record_element, s->scratch, 7, args);
  E(MIR_BEQ, L(done), R(s->scratch), I(0));
  E(MIR_MOV, R(s->learning), I(1));
  E(MIR_JMP, L(s->bailouts[id]));
  MIR_append_insn(CTX, FN, done);
}

bool ssa_lowerable(const ssa_graph_t *g) {
  for (ssa_id_t id = 1; id < g->node_count; id++) {
    const ssa_node_t *n = &g->nodes[id];
    if (n->replacement) continue;
    switch (n->op) {
      case SSA_PHI: case SSA_PARAMETER: case SSA_THIS: case SSA_CLOSURE:
      case SSA_NEW_TARGET: case SSA_SUPER: case SSA_CONSTANT:
      case SSA_BRANCH: case SSA_JUMP: case SSA_RETURN: case SSA_GUARD:
      case SSA_ENTRY: case SSA_OSR_VALUE:
      case SSA_TARGET_TEST: case SSA_RESOLVE_THIS: case SSA_ROOT_CLOSURE:
      case SSA_ARRAY_GUARD: case SSA_ARRAY_LENGTH: case SSA_LOOP_RANGE: case SSA_DEOPT:
      case SSA_SHAPE_GUARD: case SSA_LOAD_FIELD:
      case SSA_CHECK_NUMBER:
      case SSA_ARRAY_TRY_GUARD: case SSA_ARRAY_TRY_LENGTH:
      case SSA_CALL: case OP_PUT_FIELD: case OP_PUT_ELEM: case OP_PUT_GLOBAL:
      case OP_GET_UPVAL: case OP_PUT_UPVAL:
      case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD:
      case OP_ADD_NUM: case OP_SUB_NUM: case OP_MUL_NUM: case OP_DIV_NUM:
      case OP_INC: case OP_DEC: case OP_NEG: case OP_UPLUS:
      case OP_LT: case OP_LE: case OP_GT: case OP_GE:
      case OP_EQ: case OP_NE: case OP_SEQ: case OP_SNE:
      case OP_NOT: case OP_TYPEOF: case OP_IS_UNDEF: case OP_IS_NULL:
      case OP_IS_NULLISH: case OP_IS_UNDEF_OR_NULL:
      case OP_BAND: case OP_BOR: case OP_BXOR: case OP_SHL: case OP_SHR: case OP_USHR:
      case OP_GET_FIELD: case OP_GET_FIELD_OPT: case OP_GET_ELEM: case OP_GET_ELEM_OPT:
      case OP_GET_LENGTH: case OP_GLOBAL: case OP_GET_GLOBAL: case OP_GET_GLOBAL_UNDEF:
        break;
      default: return false;
    }
    if (n->op == OP_GET_FIELD || n->op == OP_GET_FIELD_OPT || n->op == OP_PUT_FIELD ||
        n->op == OP_GET_GLOBAL || n->op == OP_GET_GLOBAL_UNDEF || n->op == OP_PUT_GLOBAL) {
      sv_func_t *f = g->frames[n->frame].func;
      uint32_t atom = sv_get_u32(f->code + n->offset + 1);
      if (atom >= (uint32_t)f->atom_count) return false;
      if ((n->op == OP_GET_GLOBAL || n->op == OP_GET_GLOBAL_UNDEF) &&
          f->atoms[atom].len == 5 && !memcmp(f->atoms[atom].str, "super", 5)) return false;
    }
    if (n->op == OP_GET_UPVAL || n->op == OP_PUT_UPVAL) {
      sv_func_t *f = g->frames[n->frame].func;
      uint16_t index = sv_get_u16(f->code + n->offset + 1);
      if (index >= f->upvalue_count || jit_upvalue_is_builder_target(f, index)) return false;
    }
  }
  return true;
}

static void box_bool(ssa_lower_t *s, MIR_reg_t reg) {
  E(MIR_OR, R(reg), R(reg), U(js_false));
}

static void check_error(ssa_lower_t *s, MIR_reg_t value) {
  MIR_label_t ok = MIR_new_label(CTX);
  E(MIR_URSH, R(s->scratch), R(value), I(NANBOX_TYPE_SHIFT));
  E(MIR_BNE, L(ok), R(s->scratch), U(JIT_ERR_TAG));
  E(MIR_MOV, R(s->result), R(value));
  E(MIR_JMP, L(s->exit));
  MIR_append_insn(CTX, FN, ok);
}

static void nullish(ssa_lower_t *s, MIR_reg_t value, MIR_reg_t result) {
  MIR_reg_t other = s->null_cmp;
  E(MIR_EQ, R(result), R(value), U(js_mkundef()));
  E(MIR_EQ, R(other), R(value), U(js_mknull()));
  E(MIR_OR, R(result), R(result), R(other));
}

static void edge(ssa_lower_t *s, uint32_t from, uint32_t edge_index) {
  ssa_block_t *b = &s->g->blocks[from];
  uint32_t to = b->successors.data[edge_index];
  ssa_block_t *target = &s->g->blocks[to];
  unsigned occurrence = 0, pred = 0;
  for (unsigned i = 0; i < edge_index; i++) occurrence += b->successors.data[i] == to;
  for (; pred < target->predecessors.count; pred++) if (target->predecessors.data[pred] == from) {
    if (!occurrence) break;
    occurrence--;
  }
  // Parallel phi copies: first read all old values, then replace destinations.
  for (unsigned i = 0; i < target->phis.count; i++) {
    ssa_id_t id = target->phis.data[i];
    if (s->g->nodes[id].replacement) continue;
    if (ssa_resolve(s->g, s->g->nodes[id].inputs.data[pred]) == id) continue;
    MIR_reg_t temp = s->copies[id];
    ssa_id_t input = ssa_resolve(s->g, s->g->nodes[id].inputs.data[pred]);
    ssa_node_t *phi = &s->g->nodes[id], *src = &s->g->nodes[input];
    ssa_id_t term = b->nodes.data[b->nodes.count - 1];
    if (phi->rep == SSA_BOOL) {
      if (src->rep == SSA_BOOL) E(MIR_MOV, R(temp), R(s->values[input]));
      else {
        MIR_reg_t boxed = value_reg(s, input);
        E(MIR_OR, R(s->scratch), R(boxed), I(1));
        E(MIR_BNE, L(s->bailouts[term]), R(s->scratch), U(js_true));
        E(MIR_AND, R(temp), R(boxed), I(1));
      }
    } else if (phi->rep == SSA_F64) as_double(s, input, temp, s->bailouts[term]);
    else if (phi->rep == SSA_I32 || phi->rep == SSA_I53) {
      if (src->rep == SSA_I32 || src->rep == SSA_I53) E(MIR_MOV, R(temp), R(s->values[input]));
      else {
        MIR_reg_t boxed = value_reg(s, input);
        // A word phi must not silently turn an OSR -0 into +0.
        E(MIR_BEQ, L(s->bailouts[term]), R(boxed), U(tov(-0.0)));
        MIR_reg_t integer = mir_emit_exact_integer_guard(CTX, FN, boxed, 0, false,
            s->c.r_d_slot, (double)phi->min, (double)phi->max,
            s->bailouts[term], s->conversion_site++);
        E(MIR_MOV, R(temp), R(integer));
      }
    } else E(MIR_MOV, R(temp), R(value_reg(s, input)));
  }
  for (unsigned i = 0; i < target->phis.count; i++) {
    ssa_id_t id = target->phis.data[i];
    if (s->g->nodes[id].replacement) continue;
    if (ssa_resolve(s->g, s->g->nodes[id].inputs.data[pred]) == id) continue;
    MIR_reg_t temp = s->copies[id];
    E(s->g->nodes[id].rep == SSA_F64 ? MIR_DMOV : MIR_MOV, R(s->values[id]), R(temp));
  }
  E(MIR_JMP, L(s->blocks[to]));
}

static MIR_op_t number_operand(ssa_lower_t *s, ssa_id_t id, MIR_reg_t temporary, MIR_label_t slow) {
  id = ssa_resolve(s->g, id);
  const ssa_node_t *n = &s->g->nodes[id];
  if (n->op == SSA_CONSTANT && vtype(n->imm.value) == kTypeNumber)
    return MIR_new_double_op(CTX, tod(n->imm.value));
  if (n->rep == SSA_F64) return R(s->values[id]);
  as_double(s, id, temporary, slow);
  return R(temporary);
}

static void numeric(ssa_lower_t *s, ssa_id_t id) {
  ssa_node_t *n = &s->g->nodes[id];
  MIR_reg_t dst = s->values[id];
  if (n->rep == SSA_I53 && (n->flags & SSA_F_NO_OVERFLOW)) {
    MIR_reg_t a = s->values[ssa_resolve(s->g, n->inputs.data[0])];
    MIR_op_t b = n->inputs.count > 1 ? R(s->values[ssa_resolve(s->g, n->inputs.data[1])]) : I(1);
    E(MIR_ADD, R(dst), R(a), b);
    return;
  }
  MIR_op_t left = number_operand(s, n->inputs.data[0], s->left, s->bailouts[id]);
  MIR_op_t right = n->inputs.count > 1
      ? number_operand(s, n->inputs.data[1], s->right, s->bailouts[id])
      : MIR_new_double_op(CTX, 1);
  MIR_insn_code_t code = MIR_DADD;
  switch (n->op) {
    case OP_SUB: case OP_SUB_NUM: case OP_DEC: code = MIR_DSUB; break;
    case OP_MUL: case OP_MUL_NUM: code = MIR_DMUL; break;
    case OP_DIV: case OP_DIV_NUM: code = MIR_DDIV; break;
    case OP_LT: code = MIR_DLT; break;
    case OP_LE: code = MIR_DLE; break;
    case OP_GT: code = MIR_DGT; break;
    case OP_GE: code = MIR_DGE; break;
    case OP_EQ: case OP_SEQ: code = MIR_DEQ; break;
    case OP_NE: case OP_SNE: code = MIR_DNE; break;
    case OP_NEG:
      E(MIR_DNEG, R(dst), left);
      return;
    case OP_UPLUS: E(MIR_DMOV, R(dst), left); return;
    case OP_MOD: {
      MIR_reg_t a = value_reg(s, n->inputs.data[0]), b = value_reg(s, n->inputs.data[1]);
      MIR_op_t args[] = {R(s->c.r_vm), R(s->c.r_js), R(a), R(b)};
      call(s, s->c.helper2_proto, s->c.imp_mod, dst, 4, args);
      return;
    }
    default: break;
  }
  bool comparison = code == MIR_DLT || code == MIR_DLE || code == MIR_DGT ||
                    code == MIR_DGE || code == MIR_DEQ || code == MIR_DNE;
  E(code, R(dst), left, right);
  if (comparison && n->rep != SSA_BOOL) box_bool(s, dst);
}

static void finish_load(ssa_lower_t *s, ssa_id_t id, MIR_reg_t value) {
  if (!(s->g->nodes[id].flags & SSA_F_CHECK_NUMBER)) return;
  mir_emit_is_num_guard(CTX, FN, s->scratch, value, s->bailouts[id]);
  mir_i64_to_d(CTX, FN, s->values[id], value, s->c.r_d_slot);
}

static void emit_node(ssa_lower_t *s, ssa_id_t id) {
  ssa_node_t *n = &s->g->nodes[id];
  sv_func_t *f = s->g->frames[n->frame].func;
  MIR_reg_t dst = (n->flags & SSA_F_CHECK_NUMBER) ? s->boxed[id] : s->values[id];
  if (n->op == OP_SEQ || n->op == OP_SNE) {
    for (unsigned i = 0; i < 2; i++) {
      const ssa_node_t *input = &s->g->nodes[ssa_resolve(s->g, n->inputs.data[i])];
      if (input->op != SSA_CONSTANT) continue;
      uint8_t type = vtype(input->imm.value);
      if (type != kTypeNull && type != kTypeUndefined && type != kTypeBool) continue;
      E(n->op == OP_SEQ ? MIR_EQ : MIR_NE, R(dst), R(value_reg(s, n->inputs.data[1 - i])), U(input->imm.value));
      return;
    }
  }
  if (n->op == OP_EQ || n->op == OP_NE) {
    for (unsigned i = 0; i < 2; i++) {
      const ssa_node_t *input = &s->g->nodes[ssa_resolve(s->g, n->inputs.data[i])];
      if (input->op != SSA_CONSTANT ||
          (input->imm.value != js_mkundef() && input->imm.value != js_mknull())) continue;
      nullish(s, value_reg(s, n->inputs.data[1 - i]), dst);
      if (n->op == OP_NE) E(MIR_XOR, R(dst), R(dst), I(1));
      if (n->rep != SSA_BOOL) box_bool(s, dst);
      return;
    }
  }
  // Numeric nodes use their chosen registers directly, without first boxing.
  bool number_op = (n->rep == SSA_F64 || n->rep == SSA_I53) && n->op != SSA_CONSTANT && n->op != SSA_PHI &&
      n->op != SSA_CHECK_NUMBER && !(n->flags & SSA_F_CHECK_NUMBER);
  bool numeric_compare = n->op == OP_LT || n->op == OP_LE || n->op == OP_GT ||
      n->op == OP_GE || n->op == OP_MOD;
  if ((n->op == OP_EQ || n->op == OP_NE || n->op == OP_SEQ || n->op == OP_SNE) &&
      s->g->nodes[ssa_resolve(s->g, n->inputs.data[0])].type == SSA_T_NUMBER &&
      s->g->nodes[ssa_resolve(s->g, n->inputs.data[1])].type == SSA_T_NUMBER) numeric_compare = true;
  if (number_op || numeric_compare) { numeric(s, id); return; }
  MIR_reg_t a = n->inputs.count ? value_reg(s, n->inputs.data[0]) : 0;
  bool element = n->op == OP_GET_ELEM || n->op == OP_GET_ELEM_OPT || n->op == OP_PUT_ELEM;
  MIR_reg_t b = n->inputs.count > 1 && !element ? value_reg(s, n->inputs.data[1]) : 0;
  switch (n->op) {
    case OP_EQ: case OP_NE: case OP_SEQ: case OP_SNE: {
      bool strict = n->op == OP_SEQ || n->op == OP_SNE;
      bool invert = n->op == OP_NE || n->op == OP_SNE;
      MIR_label_t tagged = MIR_new_label(CTX), slow = MIR_new_label(CTX), done = MIR_new_label(CTX);
      MIR_label_t unequal = strict ? MIR_new_label(CTX) : slow;
      MIR_reg_t tag = new_reg(s, MIR_T_I64, "ssa_equality_tag", id);
      E(MIR_UBGT, L(tagged), R(a), U(NANBOX_PREFIX));
      E(MIR_UBGT, L(unequal), R(b), U(NANBOX_PREFIX));
      mir_i64_to_d(CTX, FN, s->left, a, s->c.r_d_slot);
      mir_i64_to_d(CTX, FN, s->right, b, s->c.r_d_slot);
      E(invert ? MIR_DNE : MIR_DEQ, R(dst), R(s->left), R(s->right));
      E(MIR_JMP, L(done));
      MIR_append_insn(CTX, FN, tagged);
      E(MIR_UBLE, L(unequal), R(b), U(NANBOX_PREFIX));
      E(MIR_URSH, R(tag), R(a), I(NANBOX_TYPE_SHIFT));
      E(MIR_URSH, R(s->scratch), R(b), I(NANBOX_TYPE_SHIFT));
      E(MIR_BNE, L(unequal), R(tag), R(s->scratch));
      E(MIR_BEQ, L(slow), R(tag), U(JIT_STR_TAG));
      E(MIR_BEQ, L(slow), R(tag), U((NANBOX_PREFIX >> NANBOX_TYPE_SHIFT) | kTypeBigInt));
      E(invert ? MIR_NE : MIR_EQ, R(dst), R(a), R(b));
      E(MIR_JMP, L(done));
      MIR_append_insn(CTX, FN, slow);
      MIR_reg_t result = s->boxed[id];
      MIR_op_t args[] = {R(s->c.r_vm), R(s->c.r_js), R(a), R(b)};
      MIR_item_t helper = strict ? (invert ? s->c.imp_sne : s->c.imp_seq) : (invert ? s->not_equal : s->equal);
      call(s, s->c.helper2_proto, helper, result, 4, args);
      if (!strict) {
        E(MIR_BEQ, L(s->bailouts[id]), R(result), U(SV_JIT_BAILOUT));
        check_error(s, result);
      }
      E(MIR_AND, R(dst), R(result), I(1));
      if (strict) {
        E(MIR_JMP, L(done));
        MIR_append_insn(CTX, FN, unequal);
        E(MIR_MOV, R(dst), I(invert));
      }
      MIR_append_insn(CTX, FN, done);
      break;
    }
    case SSA_ARRAY_TRY_GUARD: case SSA_ARRAY_TRY_LENGTH: {
      MIR_label_t done = MIR_new_label(CTX);
      MIR_reg_t ptr = new_reg(s, MIR_T_I64, "ssa_array_fact", id);
      E(MIR_MOV, R(dst), I(0));
      if (n->op == SSA_ARRAY_TRY_LENGTH) {
        E(MIR_BEQ, L(done), R(s->values[ssa_resolve(s->g, n->inputs.data[1])]), I(0));
        mir_emit_decode_ref(CTX, FN, ptr, a);
        E(MIR_MOV, R(dst), mem(s, MIR_T_U32, ptr, offsetof(ant_object_t, u.array.len)));
      } else {
        E(MIR_URSH, R(s->scratch), R(a), I(NANBOX_TYPE_SHIFT));
        E(MIR_BNE, L(done), R(s->scratch), U(NANBOX_TARR_TAG));
        mir_emit_decode_ref(CTX, FN, ptr, a);
        unsigned mask = ANT_OBJECT_FLAG_EXOTIC | ANT_OBJECT_FLAG_FAST_ARRAY | ANT_OBJECT_FLAG_DENSE_LENGTH_FITS;
        if (n->imm.u64) mask |= ANT_OBJECT_FLAG_FROZEN | ANT_OBJECT_FLAG_COW_ELEMENTS;
        E(MIR_MOV, R(s->scratch), mem(s, MIR_T_U32, ptr, offsetof(ant_object_t, flags)));
        E(MIR_AND, R(s->scratch), R(s->scratch), U(mask));
        E(MIR_BNE, L(done), R(s->scratch), U(ANT_OBJECT_FLAG_FAST_ARRAY | ANT_OBJECT_FLAG_DENSE_LENGTH_FITS));
        E(MIR_MOV, R(dst), mem(s, MIR_T_P, ptr, offsetof(ant_object_t, u.array.data)));
      }
      MIR_append_insn(CTX, FN, done);
      break;
    }
    case SSA_CHECK_NUMBER: as_double(s, n->inputs.data[0], dst, s->bailouts[id]); break;
    case SSA_SHAPE_GUARD: {
      ant_shape_t *shape = (ant_shape_t *)(uintptr_t)n->imm.u64;
      const uint32_t *guard = ant_shape_jit_guard(shape);
      mir_emit_value_to_objptr_or_jmp(CTX, FN, a, dst, s->scratch, s->bailouts[id]);
      E(MIR_MOV, R(s->scratch), mem(s, MIR_T_P, dst, offsetof(ant_object_t, shape)));
      E(MIR_BNE, L(s->bailouts[id]), R(s->scratch), U((uintptr_t)shape));
      MIR_reg_t shape_reg = new_reg(s, MIR_T_I64, "ssa_shape", id);
      E(MIR_MOV, R(shape_reg), U((uintptr_t)shape));
      E(MIR_MOV, R(s->scratch), mem(s, MIR_T_U32, shape_reg, (size_t)((const char *)guard - (const char *)shape)));
      E(MIR_BNE, L(s->bailouts[id]), R(s->scratch), I(0));
      E(MIR_MOV, R(s->scratch), mem(s, MIR_T_U16, dst, offsetof(ant_object_t, flags)));
      E(MIR_AND, R(s->scratch), R(s->scratch), U(ANT_OBJECT_FLAG_EXOTIC));
      E(MIR_BNE, L(s->bailouts[id]), R(s->scratch), I(0));
      break;
    }
    case SSA_LOAD_FIELD: {
      ssa_id_t dependency = ssa_resolve(s->g, n->inputs.data[1]);
      ant_shape_t *shape = (ant_shape_t *)(uintptr_t)s->g->nodes[dependency].imm.u64;
      MIR_reg_t ptr = s->values[dependency];
      unsigned index = (unsigned)n->imm.u64;
      E(MIR_MOV, R(s->scratch), mem(s, MIR_T_U32, ptr, offsetof(ant_object_t, prop_count)));
      E(MIR_UBLE, L(s->bailouts[id]), R(s->scratch), U(index));
      unsigned inobj = ant_shape_get_inobj_limit(shape);
      if (index < inobj) E(MIR_MOV, R(dst), mem(s, MIR_JSVAL, ptr, offsetof(ant_object_t, inobj) + index * 8));
      else {
        E(MIR_MOV, R(s->scratch), mem(s, MIR_T_P, ptr, offsetof(ant_object_t, overflow_prop)));
        E(MIR_BEQ, L(s->bailouts[id]), R(s->scratch), I(0));
        E(MIR_MOV, R(dst), mem(s, MIR_JSVAL, s->scratch, (index - inobj) * 8));
      }
      break;
    }
    case SSA_DEOPT: E(MIR_JMP, L(s->bailouts[id])); break;
    case SSA_LOOP_RANGE: {
      E(MIR_BEQ, L(s->bailouts[id]), R(a), U(tov(-0.0)));
      mir_emit_exact_integer_guard(CTX, FN, a, 0, false, s->c.r_d_slot,
          0, UINT32_MAX, s->bailouts[id], s->conversion_site++);
      MIR_reg_t limit = mir_emit_exact_integer_guard(CTX, FN, b, 0, false, s->c.r_d_slot,
          0, UINT32_MAX, s->bailouts[id], s->conversion_site++);
      MIR_reg_t length = s->values[ssa_resolve(s->g, n->inputs.data[2])];
      E(n->imm.u64 ? MIR_UBGE : MIR_UBGT, L(s->bailouts[id]), R(limit), R(length));
      break;
    }
    case SSA_ARRAY_GUARD: case SSA_ARRAY_LENGTH: {
      MIR_reg_t ptr = new_reg(s, MIR_T_I64, "ssa_array_ptr", id);
      if (n->op == SSA_ARRAY_GUARD) {
        E(MIR_URSH, R(s->scratch), R(a), I(NANBOX_TYPE_SHIFT));
        E(MIR_BNE, L(s->bailouts[id]), R(s->scratch), U(NANBOX_TARR_TAG));
      }
      mir_emit_decode_ref(CTX, FN, ptr, a);
      if (n->op == SSA_ARRAY_LENGTH) {
        E(MIR_MOV, R(dst), mem(s, MIR_T_U32, ptr, offsetof(ant_object_t, u.array.len)));
        break;
      }
      E(MIR_MOV, R(s->scratch), mem(s, MIR_T_U32, ptr, offsetof(ant_object_t, flags)));
      unsigned mask = ANT_OBJECT_FLAG_EXOTIC | ANT_OBJECT_FLAG_FAST_ARRAY | ANT_OBJECT_FLAG_DENSE_LENGTH_FITS;
      if (n->imm.u64) mask |= ANT_OBJECT_FLAG_FROZEN | ANT_OBJECT_FLAG_COW_ELEMENTS;
      E(MIR_AND, R(s->scratch), R(s->scratch), U(mask));
      E(MIR_BNE, L(s->bailouts[id]), R(s->scratch), U(ANT_OBJECT_FLAG_FAST_ARRAY | ANT_OBJECT_FLAG_DENSE_LENGTH_FITS));
      E(MIR_MOV, R(dst), mem(s, MIR_T_P, ptr, offsetof(ant_object_t, u.array.data)));
      E(MIR_BEQ, L(s->bailouts[id]), R(dst), I(0));
      break;
    }
    case SSA_TARGET_TEST: {
      MIR_label_t done = MIR_new_label(CTX);
      MIR_reg_t ptr = new_reg(s, MIR_T_I64, "ssa_target", id);
      E(MIR_MOV, R(dst), n->rep == SSA_BOOL ? I(0) : U(js_false));
      E(MIR_URSH, R(s->scratch), R(a), I(NANBOX_TYPE_SHIFT));
      E(MIR_BNE, L(done), R(s->scratch), U(NANBOX_TFUNC_TAG));
      mir_emit_decode_ref(CTX, FN, ptr, a);
      E(MIR_MOV, R(s->scratch), mem(s, MIR_T_U32, ptr, offsetof(sv_closure_t, call_flags)));
      uint32_t forbidden = SV_CALL_HAS_BOUND_ARGS | SV_CALL_HAS_SUPER | SV_CALL_IS_DEFAULT_CTOR | SV_CALL_IS_UNCURRY;
      if (!n->imm.target->is_arrow) forbidden |= SV_CALL_HAS_BOUND_THIS;
      E(MIR_AND, R(s->scratch), R(s->scratch), U(forbidden));
      E(MIR_BNE, L(done), R(s->scratch), I(0));
      E(MIR_MOV, R(s->scratch), mem(s, MIR_T_P, ptr, offsetof(sv_closure_t, func)));
      E(MIR_BNE, L(done), R(s->scratch), U((uintptr_t)n->imm.target));
      E(MIR_MOV, R(dst), n->rep == SSA_BOOL ? I(1) : U(js_true));
      MIR_append_insn(CTX, FN, done);
      break;
    }
    case SSA_ROOT_CLOSURE:
      E(MIR_MOV, mem(s, MIR_JSVAL, s->contexts, (n->frame - 1) * 4 * 8), R(a));
      break;
    case SSA_RESOLVE_THIS: {
      MIR_reg_t ptr = new_reg(s, MIR_T_I64, "ssa_this_closure", id);
      MIR_label_t ordinary = MIR_new_label(CTX);
      E(MIR_MOV, R(dst), R(b));
      mir_emit_decode_ref(CTX, FN, ptr, a);
      if (!n->imm.target->is_arrow) {
        E(MIR_MOV, R(s->scratch), mem(s, MIR_T_U32, ptr, offsetof(sv_closure_t, call_flags)));
        E(MIR_AND, R(s->scratch), R(s->scratch), U(SV_CALL_HAS_BOUND_THIS));
        E(MIR_BEQ, L(ordinary), R(s->scratch), I(0));
      }
      E(MIR_MOV, R(dst), mem(s, MIR_JSVAL, ptr, offsetof(sv_closure_t, bound_this)));
      MIR_append_insn(CTX, FN, ordinary);
      if (!n->imm.target->is_arrow && !n->imm.target->is_strict) {
        MIR_op_t args[] = {R(s->c.r_js), R(dst)};
        call(s, s->c.normalize_this_proto, s->c.imp_normalize_this, dst, 2, args);
        check_error(s, dst);
      }
      E(MIR_MOV, mem(s, MIR_JSVAL, s->contexts, ((n->frame - 1) * 4 + 1) * 8), R(dst));
      break;
    }
    case SSA_CALL: {
      if (n->bytecode == OP_TAIL_CALL || n->bytecode == OP_TAIL_CALL_METHOD) {
        // An uninlined tail call resumes the interpreter's frame-reuse path.
        E(MIR_JMP, L(s->bailouts[id]));
        break;
      }
      unsigned argc = n->inputs.count - 2;
      for (unsigned i = 0; i < argc; i++)
        E(MIR_MOV, mem(s, MIR_JSVAL, s->args_buf, i * 8), R(value_reg(s, n->inputs.data[i + 2])));
      MIR_op_t record[] = {U((uintptr_t)f), U(n->offset), R(a)};
      call(s, s->c.call_target_proto, s->c.imp_record_call_target, s->scratch, 3, record);
      MIR_op_t args[] = {R(s->c.r_vm), R(s->c.r_js), R(a), R(b), R(s->args_buf), I(argc)};
      call(s, s->c.call_proto, s->c.imp_call, dst, 6, args);
      for (unsigned i = 0; i < argc; i++) E(MIR_MOV, mem(s, MIR_JSVAL, s->args_buf, i * 8), U(js_mkundef()));
      check_error(s, dst);
      break;
    }
    case OP_GET_UPVAL: case OP_PUT_UPVAL: {
      MIR_reg_t ptr = new_reg(s, MIR_T_I64, "ssa_uv_ptr", id);
      MIR_reg_t cell = new_reg(s, MIR_T_I64, "ssa_uv_cell", id);
      uint16_t index = sv_get_u16(f->code + n->offset + 1);
      mir_emit_decode_ref(CTX, FN, ptr, value_reg(s, s->g->frames[n->frame].closure));
      E(MIR_MOV, R(ptr), mem(s, MIR_T_P, ptr, offsetof(sv_closure_t, upvalues)));
      E(MIR_MOV, R(cell), mem(s, MIR_T_P, ptr, index * sizeof(sv_upvalue_t *)));
      E(MIR_MOV, R(ptr), mem(s, MIR_T_P, cell, offsetof(sv_upvalue_t, location)));
      if (n->op == OP_GET_UPVAL) E(MIR_MOV, R(dst), mem(s, MIR_JSVAL, ptr, 0));
      else {
        E(MIR_MOV, mem(s, MIR_JSVAL, ptr, 0), R(a));
        mir_emit_upval_write_barrier(CTX, FN, s->c.upval_barrier_proto,
            s->c.imp_upval_barrier, s->c.r_js, cell, a, -(int)id);
      }
      break;
    }
    case OP_PUT_FIELD: {
      sv_atom_t *atom = &f->atoms[sv_get_u32(f->code + n->offset + 1)];
      uint16_t ic_index = sv_get_u16(f->code + n->offset + 5);
      sv_ic_entry_t *ic = f->ic_slots && ic_index < f->ic_count ? &f->ic_slots[ic_index] : NULL;
      MIR_label_t slow = MIR_new_label(CTX), done = MIR_new_label(CTX);
      if (mir_emit_put_field_ic_fastpath(CTX, FN, s->c.js, f, -(int)id, ic_index, atom,
          s->c.r_js, a, b, slow, s->epoch, s->c.shape_transition_proto, s->c.imp_shape_transition,
          s->c.remember_obj_proto, s->c.imp_remember_obj)) E(MIR_JMP, L(done));
      MIR_append_insn(CTX, FN, slow);
      MIR_op_t args[] = {R(s->c.r_vm), R(s->c.r_js), R(a), R(b), U((uintptr_t)atom), U((uintptr_t)ic)};
      call(s, s->c.put_field_proto, s->put_field, dst, 6, args);
      E(MIR_BEQ, L(s->bailouts[id]), R(dst), U(SV_JIT_BAILOUT));
      check_error(s, dst);
      MIR_append_insn(CTX, FN, done);
      break;
    }
    case OP_PUT_ELEM: {
      MIR_reg_t val = value_reg(s, n->inputs.data[2]);
      MIR_reg_t old = new_reg(s, MIR_JSVAL, "ssa_element_old", id);
      bool hoisted = (n->flags & SSA_F_HOISTED) != 0;
      bool fact = (n->flags & SSA_F_ARRAY_FACT) != 0;
      MIR_label_t slow = hoisted ? s->bailouts[id] : MIR_new_label(CTX), done = MIR_new_label(CTX);
      mir_emit_is_num_guard(CTX, FN, s->scratch, val, slow);
      bool no_bounds = (n->flags & SSA_F_NO_BOUNDS) != 0;
      MIR_reg_t index = no_bounds ? s->values[ssa_resolve(s->g, n->inputs.data[1])] :
          array_index(s, n->inputs.data[1], slow, -(int)id);
      MIR_reg_t data;
      if (hoisted || fact) {
        data = s->values[ssa_resolve(s->g, n->inputs.data[3])];
        MIR_reg_t length = s->values[ssa_resolve(s->g, n->inputs.data[4])];
        if (fact) E(MIR_BEQ, L(slow), R(data), I(0));
        if (!no_bounds) E(MIR_UBGE, L(slow), R(index), R(length));
        E(MIR_MOV, R(old), MIR_new_mem_op(CTX, MIR_JSVAL, 0, data, index, 8));
      } else data = mir_emit_dense_element_guard(CTX, FN, a, index, old, JIT_ELEMENT_WRITE, slow, -(int)id);
      E(MIR_UBGE, L(slow), R(old), U(ANT_SENTINEL_TAG));
      record_element(s, id, true);
      E(MIR_MOV, MIR_new_mem_op(CTX, MIR_JSVAL, 0, data, index, 8), R(val));
      if (hoisted) break;
      E(MIR_JMP, L(done));
      MIR_append_insn(CTX, FN, slow);
      b = value_reg(s, n->inputs.data[1]);
      MIR_op_t args[] = {R(s->c.r_vm), R(s->c.r_js), R(a), R(b), R(val)};
      call(s, s->c.put_elem_proto, s->put_elem, dst, 5, args);
      E(MIR_BEQ, L(s->bailouts[id]), R(dst), U(SV_JIT_BAILOUT));
      check_error(s, dst);
      MIR_append_insn(CTX, FN, done);
      break;
    }
    case OP_PUT_GLOBAL: {
      sv_atom_t *atom = &f->atoms[sv_get_u32(f->code + n->offset + 1)];
      MIR_op_t args[] = {R(s->c.r_vm), R(s->c.r_js), R(a), U((uintptr_t)atom->str), I(atom->len), I(f->is_strict)};
      call(s, s->c.put_global_proto, s->put_global, dst, 6, args);
      E(MIR_BEQ, L(s->bailouts[id]), R(dst), U(SV_JIT_BAILOUT));
      check_error(s, dst);
      break;
    }
    case SSA_ENTRY: {
      MIR_label_t normal = MIR_new_label(CTX), bad = MIR_new_label(CTX);
      E(MIR_MOV, R(s->scratch), mem(s, MIR_T_U8, s->c.r_vm, offsetof(sv_vm_t, jit_osr.active)));
      E(MIR_BEQ, L(normal), R(s->scratch), I(0));
      E(MIR_MOV, R(s->scratch), mem(s, MIR_T_I32, s->c.r_vm, offsetof(sv_vm_t, jit_osr.n_locals)));
      E(MIR_BNE, L(bad), R(s->scratch), I(f->max_locals));
      ssa_block_t *entry = &s->g->blocks[n->block];
      for (unsigned e = 1; e < entry->successors.count; e++) {
        ssa_block_t *stub = &s->g->blocks[entry->successors.data[e]];
        MIR_label_t next = MIR_new_label(CTX);
        E(MIR_MOV, R(s->scratch), mem(s, MIR_T_I32, s->c.r_vm, offsetof(sv_vm_t, jit_osr.bc_offset)));
        E(MIR_BNE, L(next), R(s->scratch), I(stub->offset));
        E(MIR_MOV, R(s->scratch), mem(s, MIR_T_I32, s->c.r_vm, offsetof(sv_vm_t, jit_osr.vstack_sp)));
        E(MIR_BNE, L(bad), R(s->scratch), I(stub->stack_depth));
        if (f->max_locals) {
          E(MIR_MOV, R(s->scratch), mem(s, MIR_T_P, s->c.r_vm, offsetof(sv_vm_t, jit_osr.locals)));
          E(MIR_BEQ, L(bad), R(s->scratch), I(0));
        }
        if (stub->stack_depth) {
          E(MIR_MOV, R(s->scratch), mem(s, MIR_T_P, s->c.r_vm, offsetof(sv_vm_t, jit_osr.vstack)));
          E(MIR_BEQ, L(bad), R(s->scratch), I(0));
        }
        if (f->param_count) E(MIR_BEQ, L(bad), R(s->c.r_args), I(0));
        E(MIR_MOV, mem(s, MIR_T_U8, s->c.r_vm, offsetof(sv_vm_t, jit_osr.active)), I(0));
        E(MIR_JMP, L(s->blocks[entry->successors.data[e]]));
        MIR_append_insn(CTX, FN, next);
      }
      MIR_append_insn(CTX, FN, bad);
      E(MIR_MOV, mem(s, MIR_T_U8, s->c.r_vm, offsetof(sv_vm_t, jit_osr.active)), I(0));
      E(MIR_MOV, R(s->result), U(SV_JIT_RETRY_INTERP));
      E(MIR_JMP, L(s->exit));
      MIR_append_insn(CTX, FN, normal);
      edge(s, n->block, 0);
      break;
    }
    case SSA_OSR_VALUE: {
      unsigned slot = (unsigned)n->imm.u64;
      MIR_reg_t base = s->c.r_args;
      if (slot >= f->param_count) {
        slot -= f->param_count;
        bool operand = slot >= (unsigned)f->max_locals;
        if (operand) slot -= f->max_locals;
        E(MIR_MOV, R(s->scratch), mem(s, MIR_T_P, s->c.r_vm,
            operand ? offsetof(sv_vm_t, jit_osr.vstack) : offsetof(sv_vm_t, jit_osr.locals)));
        base = s->scratch;
      }
      E(MIR_MOV, R(dst), mem(s, MIR_JSVAL, base, slot * sizeof(ant_value_t)));
      break;
    }
    case SSA_CONSTANT:
      if (n->rep == SSA_BOOL) E(MIR_MOV, R(dst), I(vdata(n->imm.value) != 0));
      else if (n->rep == SSA_F64) E(MIR_DMOV, R(dst), MIR_new_double_op(CTX, tod(n->imm.value)));
      else if (n->rep == SSA_I32) E(MIR_MOV, R(dst), I((int64_t)tod(n->imm.value)));
      else if (n->bytecode == OP_CONST || n->bytecode == OP_CONST8) {
        unsigned index = n->bytecode == OP_CONST ? sv_get_u32(f->code + n->offset + 1) : f->code[n->offset + 1];
        E(MIR_MOV, R(s->scratch), U((uintptr_t)&f->constants[index]));
        E(MIR_MOV, R(dst), mem(s, MIR_JSVAL, s->scratch, 0));
      } else E(MIR_MOV, R(dst), U(n->imm.value));
      break;
    case SSA_PARAMETER: {
      MIR_label_t done = MIR_new_label(CTX);
      E(MIR_MOV, R(dst), U(js_mkundef()));
      E(MIR_BLE, L(done), R(s->c.r_argc), I(n->imm.u64));
      E(MIR_MOV, R(dst), mem(s, MIR_JSVAL, s->c.r_args, n->imm.u64 * sizeof(ant_value_t)));
      MIR_append_insn(CTX, FN, done);
      break;
    }
    case SSA_THIS: E(MIR_MOV, R(dst), R(s->c.r_this_curr)); break;
    case SSA_NEW_TARGET: E(MIR_MOV, R(dst), R(s->c.r_new_target)); break;
    case SSA_SUPER: E(MIR_MOV, R(dst), R(s->c.r_super_val)); break;
    case SSA_CLOSURE:
      mir_emit_cage_offset(CTX, FN, dst, s->c.r_closure);
      E(MIR_OR, R(dst), R(dst), U(mkval(kTypeFunction, 0)));
      break;
    case SSA_JUMP: edge(s, n->block, 0); break;
    case SSA_RETURN:
      E(MIR_MOV, R(s->result), R(a));
      E(MIR_JMP, L(s->exit));
      break;
    case SSA_BRANCH: {
      MIR_label_t taken = MIR_new_label(CTX);
      ssa_id_t condition = ssa_resolve(s->g, n->inputs.data[0]);
      if (s->g->nodes[condition].rep == SSA_BOOL) {
        if (n->bytecode == OP_JMP_NOT_NULLISH) E(MIR_JMP, L(taken));
        else {
          bool inverse = n->bytecode == OP_JMP_FALSE || n->bytecode == OP_JMP_FALSE8 || n->bytecode == OP_JMP_FALSE_PEEK;
          E(inverse ? MIR_BEQ : MIR_BNE, L(taken), R(s->values[condition]), I(0));
        }
      } else if (n->bytecode == OP_JMP_NOT_NULLISH) {
        nullish(s, a, s->scratch);
        E(MIR_BEQ, L(taken), R(s->scratch), I(0));
      } else {
        bool inverse = n->bytecode == OP_JMP_FALSE || n->bytecode == OP_JMP_FALSE8 || n->bytecode == OP_JMP_FALSE_PEEK;
        mir_emit_truthy_branch(CTX, FN, a, s->scratch, s->c.r_js,
            s->c.truthy_proto, s->c.imp_is_truthy, inverse, taken);
      }
      edge(s, n->block, 1);
      MIR_append_insn(CTX, FN, taken);
      edge(s, n->block, 0);
      break;
    }
    case SSA_GUARD: E(MIR_BEQ, L(s->bailouts[id]), R(a), U(n->imm.value)); break;
    case OP_NOT: {
      MIR_op_t args[] = {R(s->c.r_js), R(a)};
      call(s, s->c.truthy_proto, s->c.imp_is_truthy, dst, 2, args);
      E(MIR_EQ, R(dst), R(dst), I(0));
      if (n->rep != SSA_BOOL) box_bool(s, dst);
      break;
    }
    case OP_IS_UNDEF: case OP_IS_NULL:
      E(MIR_EQ, R(dst), R(a), U(n->op == OP_IS_NULL ? js_mknull() : js_mkundef()));
      if (n->rep != SSA_BOOL) box_bool(s, dst);
      break;
    case OP_IS_NULLISH: case OP_IS_UNDEF_OR_NULL:
      nullish(s, a, dst); if (n->rep != SSA_BOOL) box_bool(s, dst); break;
    case OP_TYPEOF: case OP_GET_LENGTH: {
      MIR_op_t args[] = {R(s->c.r_vm), R(s->c.r_js), R(a)};
      call(s, s->c.helper1_proto, n->op == OP_TYPEOF ? s->c.imp_typeof : s->c.imp_get_length_inline, dst, 3, args);
      E(MIR_BEQ, L(s->bailouts[id]), R(dst), U(SV_JIT_BAILOUT));
      check_error(s, dst);
      break;
    }
    case OP_BAND: case OP_BOR: case OP_BXOR: case OP_SHL: case OP_SHR: case OP_USHR: {
      ssa_id_t ai = ssa_resolve(s->g, n->inputs.data[0]), bi = ssa_resolve(s->g, n->inputs.data[1]);
      uint8_t ar = s->g->nodes[ai].rep, br = s->g->nodes[bi].rep;
      MIR_reg_t left = ar == SSA_I32 ? s->values[ai] : mir_emit_word32_guard(CTX, FN, a,
          s->values[ai], ar == SSA_F64, false, s->c.r_d_slot, s->bailouts[id], -(int)id * 2);
      MIR_reg_t right = br == SSA_I32 ? s->values[bi] : mir_emit_word32_guard(CTX, FN, b,
          s->values[bi], br == SSA_F64, false, s->c.r_d_slot, s->bailouts[id], -(int)id * 2 - 1);
      mir_emit_word32_binary(CTX, FN, (sv_op_t)n->op, left, right, dst, 0, false, -(int)id);
      break;
    }
    case OP_GLOBAL: E(MIR_MOV, R(dst), mem(s, MIR_JSVAL, s->c.r_js, offsetof(ant_t, global))); break;
    case OP_GET_GLOBAL: case OP_GET_GLOBAL_UNDEF: {
      sv_atom_t *atom = &f->atoms[sv_get_u32(f->code + n->offset + 1)];
      MIR_op_t args[] = {R(s->c.r_js), U((uintptr_t)atom->str), U((uintptr_t)f), I(n->offset)};
      call(s, s->c.gg_proto, s->global, dst, 4, args);
      E(MIR_BEQ, L(s->bailouts[id]), R(dst), U(SV_JIT_BAILOUT));
      check_error(s, dst);
      break;
    }
    case OP_GET_FIELD: case OP_GET_FIELD_OPT: case OP_GET_ELEM: case OP_GET_ELEM_OPT: {
      MIR_label_t done = MIR_new_label(CTX);
      if (n->op == OP_GET_FIELD_OPT || n->op == OP_GET_ELEM_OPT) {
        E(MIR_MOV, R(dst), U(js_mkundef()));
        nullish(s, a, s->scratch);
        E(MIR_BNE, L(done), R(s->scratch), I(0));
      }
      if (n->op == OP_GET_FIELD || n->op == OP_GET_FIELD_OPT) {
        sv_atom_t *atom = &f->atoms[sv_get_u32(f->code + n->offset + 1)];
        MIR_label_t slow = MIR_new_label(CTX);
        if (mir_emit_get_field_ic_fastpath(CTX, FN, s->c.js, f, -(int)id,
            sv_get_u16(f->code + n->offset + 5), atom, a, dst, slow, s->epoch)) E(MIR_JMP, L(done));
        MIR_append_insn(CTX, FN, slow);
        MIR_op_t args[] = {R(s->c.r_js), R(a), U((uintptr_t)atom->str), U(atom->len), U((uintptr_t)f), I(n->offset)};
        call(s, s->c.gf_proto, s->c.imp_get_field_inline, dst, 6, args);
      } else {
        bool hoisted = (n->flags & SSA_F_HOISTED) != 0;
        bool fact = (n->flags & SSA_F_ARRAY_FACT) != 0;
        MIR_label_t slow = hoisted ? s->bailouts[id] : MIR_new_label(CTX);
        bool no_bounds = (n->flags & SSA_F_NO_BOUNDS) != 0;
        MIR_reg_t index = no_bounds ? s->values[ssa_resolve(s->g, n->inputs.data[1])] :
            array_index(s, n->inputs.data[1], slow, -(int)id);
        if (hoisted || fact) {
          MIR_reg_t data = s->values[ssa_resolve(s->g, n->inputs.data[2])];
          MIR_reg_t length = s->values[ssa_resolve(s->g, n->inputs.data[3])];
          if (fact) E(MIR_BEQ, L(slow), R(data), I(0));
          if (!no_bounds) E(MIR_UBGE, L(slow), R(index), R(length));
          E(MIR_MOV, R(dst), MIR_new_mem_op(CTX, MIR_JSVAL, 0, data, index, 8));
          E(MIR_UBGE, L(slow), R(dst), U(ANT_SENTINEL_TAG));
          record_element(s, id, false);
          if (hoisted) { MIR_append_insn(CTX, FN, done); finish_load(s, id, dst); break; }
        } else {
          mir_emit_dense_element_guard(CTX, FN, a, index, dst, JIT_ELEMENT_READ, slow, -(int)id);
          record_element(s, id, false);
        }
        E(MIR_JMP, L(done));
        MIR_append_insn(CTX, FN, slow);
        b = value_reg(s, n->inputs.data[1]);
        MIR_op_t args[] = {R(s->c.r_vm), R(s->c.r_js), R(a), R(b)};
        call(s, s->c.helper2_proto, s->c.imp_get_elem_inline, dst, 4, args);
      }
      E(MIR_BEQ, L(s->bailouts[id]), R(dst), U(SV_JIT_BAILOUT));
      check_error(s, dst);
      MIR_append_insn(CTX, FN, done);
      finish_load(s, id, dst);
      break;
    }
    default: numeric(s, id); break;
  }
}

static bool prepare_recipes(ssa_lower_t *s) {
  for (ssa_id_t id = 1; id < s->g->node_count; id++) {
    ssa_node_t *n = &s->g->nodes[id];
    if (n->replacement || (n->flags & SSA_F_DEAD) || !s->g->blocks[n->block].reachable) continue;
    switch (n->op) {
      case SSA_PHI: case SSA_CONSTANT: case SSA_PARAMETER: case SSA_THIS:
      case SSA_CLOSURE: case SSA_NEW_TARGET: case SSA_SUPER: case SSA_RETURN:
        continue;
      case SSA_BRANCH: case SSA_JUMP: case SSA_ENTRY:
        if (edge_needs_guard(s->g, n->block)) break;
        continue;
      case OP_NOT: case OP_SEQ: case OP_SNE: case SSA_OSR_VALUE:
      case SSA_TARGET_TEST: case SSA_RESOLVE_THIS: case SSA_ROOT_CLOSURE:
      case OP_GET_UPVAL: case OP_PUT_UPVAL:
      case OP_GLOBAL: case OP_IS_UNDEF: case OP_IS_NULL: case OP_IS_NULLISH:
      case OP_IS_UNDEF_OR_NULL:
        continue;
      case SSA_CALL:
        if (n->bytecode == OP_TAIL_CALL || n->bytecode == OP_TAIL_CALL_METHOD) break;
        continue;
      case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD:
      case OP_ADD_NUM: case OP_SUB_NUM: case OP_MUL_NUM: case OP_DIV_NUM:
      case OP_INC: case OP_DEC: case OP_NEG: case OP_UPLUS:
      case OP_LT: case OP_LE: case OP_GT: case OP_GE: case OP_EQ: case OP_NE: {
        bool guarded = false, nullish_comparison = false;
        for (unsigned i = 0; i < n->inputs.count; i++) {
          const ssa_node_t *input = &s->g->nodes[ssa_resolve(s->g, n->inputs.data[i])];
          guarded |= input->type != SSA_T_NUMBER;
          nullish_comparison |= (n->op == OP_EQ || n->op == OP_NE) && input->op == SSA_CONSTANT &&
              (input->imm.value == js_mknull() || input->imm.value == js_mkundef());
        }
        if (guarded && !nullish_comparison) break;
        continue;
      }
      case SSA_ARRAY_LENGTH: case SSA_ARRAY_TRY_GUARD: case SSA_ARRAY_TRY_LENGTH: continue;
      default: break;
    }
    if (!n->snapshot) {
      s->g->failure = "guard without interpreter state";
      return false;
    }
    uint32_t chain[32], depth = 0, count = 0;
    for (uint32_t state = n->snapshot; state; state = s->g->snapshots[state].parent) {
      if (depth == 32) return false;
      chain[depth++] = state;
      ssa_snapshot_t *snapshot = &s->g->snapshots[state];
      count += 4 + snapshot->values.count;
      if (snapshot->frame != 1) count += s->g->frames[snapshot->frame].arguments.count;
    }
    if (count > 4096) return false;
    if (count > s->spill_count) s->spill_count = count;
    sv_deopt_recipe_t *recipe = code_arena_bump(sizeof(*recipe) + depth * sizeof(sv_deopt_frame_t));
    if (!recipe) return false;
    sv_deopt_frame_t *frame = (sv_deopt_frame_t *)(recipe + 1);
    unsigned at = 0;
    for (unsigned i = 0; i < depth; i++) {
      ssa_snapshot_t *state = &s->g->snapshots[chain[depth - i - 1]];
      ssa_frame_t *context = &s->g->frames[state->frame];
      frame[i] = (sv_deopt_frame_t){.func = context->func, .offset = state->offset,
          .call_offset = state->call_offset, .context_offset = at, .state_offset = at + 4,
          .params = state->params, .locals = state->locals, .stack = state->stack,
          .root_arguments = state->frame == 1, .return_child = state->return_child};
      at += 4 + state->values.count;
      if (state->frame != 1) {
        frame[i].arguments_offset = at;
        frame[i].argc = (uint16_t)context->arguments.count;
        at += context->arguments.count;
      }
      if (i + 1 < depth) frame[i].child = &frame[i + 1];
    }
    ssa_snapshot_t *owner_state = &s->g->snapshots[chain[depth - 1]];
    *recipe = (sv_deopt_recipe_t){.owner = s->c.func, .frame = frame,
        .value_count = count, .owner_offset = depth > 1 ? owner_state->call_offset : owner_state->offset};
    s->recipes[id] = recipe;
  }
  return true;
}

static void prologue(ssa_lower_t *s) {
  jit_compile_t *c = &s->c;
  char name[96];
  snprintf(name, sizeof(name), "ssa_%s_%p", c->func->debug && c->func->debug->name ? c->func->debug->name : "anon", (void *)c->func);
  c->mod = MIR_new_module(CTX, name);
  MIR_type_t result = MIR_JSVAL;
  jit_setup_prototypes(c, result);
  s->deopt_proto = MIR_new_proto(CTX, "ssa_deopt_proto", 1, &result, 6,
      MIR_T_P, "vm", MIR_T_P, "recipe", MIR_T_P, "values", MIR_T_P, "args", MIR_T_I32, "argc", MIR_T_I32, "learning");
  s->deopt = MIR_new_import(CTX, "jit_helper_ssa_deopt_owned");
  MIR_type_t pointer = MIR_T_I64;
  s->spill_proto = MIR_new_proto(CTX, "ssa_spill_proto", 1, &pointer, 2, MIR_T_P, "js", MIR_T_U32, "count");
  s->allocate_spill = MIR_new_import(CTX, "jit_helper_ssa_deopt_alloc");
  s->record_proto = MIR_new_proto(CTX, "ssa_record_proto", 1, &result, 7,
      MIR_T_P, "owner", MIR_T_P, "func", MIR_T_U32, "offset", MIR_JSVAL, "object",
      MIR_JSVAL, "key", MIR_JSVAL, "value", MIR_T_I32, "write");
  s->record_element = MIR_new_import(CTX, "jit_helper_ssa_record_element");
  s->roots_proto = MIR_new_proto(CTX, "ssa_roots_proto", 0, NULL, 4,
      MIR_T_P, "js", MIR_T_P, "scope", MIR_T_P, "values", MIR_T_U64, "count");
  s->roots = MIR_new_import(CTX, "gc_temp_root_scope_borrow");
  s->end_proto = MIR_new_proto(CTX, "ssa_end_proto", 0, NULL, 1, MIR_T_P, "scope");
  s->end = MIR_new_import(CTX, "gc_temp_root_scope_end");
  s->global = MIR_new_import(CTX, "jit_helper_ssa_get_global");
  s->equal = MIR_new_import(CTX, "jit_helper_ssa_eq");
  s->not_equal = MIR_new_import(CTX, "jit_helper_ssa_ne");
  s->put_field = MIR_new_import(CTX, "jit_helper_ssa_put_field");
  s->put_elem = MIR_new_import(CTX, "jit_helper_ssa_put_elem");
  s->put_global = MIR_new_import(CTX, "jit_helper_ssa_put_global");
  c->jit_func = MIR_new_func(CTX, name, 1, &result, 7,
      MIR_T_P, "vm", MIR_JSVAL, "this_val", MIR_JSVAL, "new_target", MIR_JSVAL, "super_val",
      MIR_T_P, "args", MIR_T_I32, "argc", MIR_T_P, "closure");
  c->r_vm = MIR_reg(CTX, "vm", FN->u.func);
  c->r_this_curr = MIR_reg(CTX, "this_val", FN->u.func);
  c->r_new_target = MIR_reg(CTX, "new_target", FN->u.func);
  c->r_super_val = MIR_reg(CTX, "super_val", FN->u.func);
  c->r_args = MIR_reg(CTX, "args", FN->u.func);
  c->r_argc = MIR_reg(CTX, "argc", FN->u.func);
  c->r_closure = MIR_reg(CTX, "closure", FN->u.func);
  c->r_cage_base = MIR_new_func_reg(CTX, FN->u.func, MIR_T_I64, "cage_base");
  E(MIR_MOV, R(c->r_cage_base), U((uintptr_t)ant_cage_base()));
  c->r_js = new_reg(s, MIR_T_P, "ssa_js", 0);
  E(MIR_MOV, R(c->r_js), mem(s, MIR_T_P, c->r_vm, offsetof(sv_vm_t, js)));
  s->scratch = new_reg(s, MIR_T_I64, "ssa_scratch", 0);
  s->learning = new_reg(s, MIR_T_I64, "ssa_learning", 0);
  E(MIR_MOV, R(s->learning), I(0));
  s->epoch = new_reg(s, MIR_T_I64, "ssa_epoch", 0);
  E(MIR_MOV, R(s->epoch), U((uintptr_t)&ant_ic_epoch_counter));
  s->args_buf = new_reg(s, MIR_T_P, "ssa_args", 0);
  s->null_cmp = new_reg(s, MIR_T_I64, "ssa_null_cmp", 0);
  s->left = new_reg(s, MIR_T_D, "ssa_left", 0);
  s->right = new_reg(s, MIR_T_D, "ssa_right", 0);
  s->number = new_reg(s, MIR_T_D, "ssa_number", 0);
  s->result = new_reg(s, MIR_JSVAL, "ssa_result", 0);
  s->spill = new_reg(s, MIR_T_P, "ssa_spill", 0);
  s->scope = new_reg(s, MIR_T_P, "ssa_roots", 0);
  s->contexts = new_reg(s, MIR_T_P, "ssa_context", 0);
  c->r_d_slot = new_reg(s, MIR_T_P, "ssa_bitcast", 0);
  s->exit = MIR_new_label(CTX);
  s->reject = MIR_new_label(CTX);
  E(MIR_ALLOCA, R(c->r_d_slot), I(8));
  E(MIR_MOV, R(s->scratch), mem(s, MIR_T_P, c->r_js, offsetof(ant_t, cstk.floor)));
  MIR_label_t stack_ok = MIR_new_label(CTX);
  E(MIR_UBGE, L(stack_ok), R(c->r_d_slot), R(s->scratch));
  MIR_op_t error_args[] = {R(c->r_vm), R(c->r_js)};
  call(s, c->stack_ovf_err_proto, c->imp_stack_ovf_err, s->result, 2, error_args);
  MIR_append_insn(CTX, FN, MIR_new_ret_insn(CTX, 1, R(s->result)));
  MIR_append_insn(CTX, FN, stack_ok);
  E(MIR_BEQ, L(s->reject), R(c->r_closure), I(0));
  E(MIR_MOV, R(s->scratch), mem(s, MIR_T_P, c->r_closure, offsetof(sv_closure_t, func)));
  E(MIR_BNE, L(s->reject), R(s->scratch), U((uintptr_t)c->func));
  E(MIR_ALLOCA, R(s->scope), U(sizeof(gc_temp_root_scope_t)));
  unsigned context_count = (s->g->frame_count - 1) * 4;
  unsigned root_count = context_count + s->argument_count;
  E(MIR_ALLOCA, R(s->contexts), U(root_count * sizeof(ant_value_t)));
  if (s->argument_count) E(MIR_ADD, R(s->args_buf), R(s->contexts), U(context_count * sizeof(ant_value_t)));
  else E(MIR_MOV, R(s->args_buf), I(0));
  for (unsigned i = 4; i < root_count; i++)
    E(MIR_MOV, mem(s, MIR_JSVAL, s->contexts, i * 8), U(js_mkundef()));
  mir_emit_cage_offset(CTX, FN, s->scratch, c->r_closure);
  E(MIR_OR, R(s->scratch), R(s->scratch), U(mkval(kTypeFunction, 0)));
  E(MIR_MOV, mem(s, MIR_JSVAL, s->contexts, 0), R(s->scratch));
  E(MIR_MOV, mem(s, MIR_JSVAL, s->contexts, 8), R(c->r_this_curr));
  E(MIR_MOV, mem(s, MIR_JSVAL, s->contexts, 16), R(c->r_new_target));
  E(MIR_MOV, mem(s, MIR_JSVAL, s->contexts, 24), R(c->r_super_val));
  MIR_op_t root_args[] = {R(c->r_js), R(s->scope), R(s->contexts), I(root_count)};
  call(s, s->roots_proto, s->roots, 0, 4, root_args);
  if (!c->func->is_arrow && !c->func->is_strict) {
    MIR_op_t args[] = {R(c->r_js), R(c->r_this_curr)};
    call(s, c->normalize_this_proto, c->imp_normalize_this, c->r_this_curr, 2, args);
    E(MIR_MOV, mem(s, MIR_JSVAL, s->contexts, 8), R(c->r_this_curr));
    E(MIR_URSH, R(s->scratch), R(c->r_this_curr), I(NANBOX_TYPE_SHIFT));
    MIR_label_t ok = MIR_new_label(CTX);
    E(MIR_BNE, L(ok), R(s->scratch), U(JIT_ERR_TAG));
    E(MIR_MOV, R(s->result), R(c->r_this_curr));
    E(MIR_JMP, L(s->exit));
    MIR_append_insn(CTX, FN, ok);
  }
}

sv_jit_func_t jit_ssa_compile(ant_t *js, sv_func_t *func) {
  ssa_graph_t graph;
  bool built = ssa_graph_build(&graph, func);
  if (!built || !ssa_lowerable(&graph) || !ssa_inline_calls(&graph) || !ssa_optimize(&graph)) {
    if (sv_jit_warn_unlikely && graph.failure) fprintf(stderr, "jit: SSA declined: %s\n", graph.failure);
    ssa_graph_destroy(&graph); return NULL;
  }
  ssa_lower_t lower = {.g = &graph, .c = {.js = js, .func = func, .jc = js->jit_ctx}, .conversion_site = 1000000};
  ssa_lower_t *s = &lower;
  s->values = calloc(graph.node_count, sizeof(*s->values));
  s->copies = calloc(graph.node_count, sizeof(*s->copies));
  s->boxed = calloc(graph.node_count, sizeof(*s->boxed));
  s->blocks = calloc(graph.block_count, sizeof(*s->blocks));
  s->bailouts = calloc(graph.node_count, sizeof(*s->bailouts));
  s->recipes = calloc(graph.node_count, sizeof(*s->recipes));
  sv_jit_func_t generated = NULL;
  for (ssa_id_t id = 1; id < graph.node_count; id++) {
    const ssa_node_t *n = &graph.nodes[id];
    if (n->replacement || !graph.blocks[n->block].reachable || n->op != SSA_CALL) continue;
    unsigned count = n->inputs.count - 2;
    if (count > s->argument_count) s->argument_count = count;
  }
  if (!s->values || !s->copies || !s->boxed || !s->blocks || !s->bailouts || !s->recipes || !s->c.jc || !prepare_recipes(s)) goto done;
  for (ssa_id_t id = 1; id < graph.node_count; id++) {
    if (graph.nodes[id].replacement || !graph.blocks[graph.nodes[id].block].reachable || graph.nodes[id].op != SSA_SHAPE_GUARD) continue;
    ant_shape_t *shape = (ant_shape_t *)(uintptr_t)graph.nodes[id].imm.u64;
    ant_shape_t **root = code_arena_bump(sizeof(*root));
    if (!root) goto done;
    *root = NULL;
    if (!sv_ic_shape_ref_register(js, root)) goto done;
    ant_shape_retain(shape);
    *root = shape;
  }
  s->c.ctx = s->c.jc->ctx_hot;
  jit_load_externals_once(s->c.jc);
  prologue(s);
  for (ssa_id_t id = 1; id < graph.node_count; id++) {
    if (graph.nodes[id].replacement || (graph.nodes[id].flags & SSA_F_DEAD) || !graph.blocks[graph.nodes[id].block].reachable) continue;
    s->values[id] = new_reg(s, representation_type(&graph.nodes[id]), "ssa_v", id);
    if (graph.nodes[id].rep != SSA_BOXED && graph.nodes[id].rep != SSA_PTR)
      s->boxed[id] = new_reg(s, MIR_JSVAL, "ssa_boxed", id);
    if (graph.nodes[id].op == SSA_PHI && !graph.nodes[id].replacement)
      s->copies[id] = new_reg(s, representation_type(&graph.nodes[id]), "ssa_phi_copy", id);
    if (s->recipes[id]) s->bailouts[id] = MIR_new_label(CTX);
  }
  for (uint32_t bi = 1; bi < graph.block_count; bi++) s->blocks[bi] = MIR_new_label(CTX);
  for (uint32_t bi = 1; bi < graph.block_count; bi++) {
    MIR_append_insn(CTX, FN, s->blocks[bi]);
    if (!graph.blocks[bi].reachable) continue;
    ssa_ids_t *nodes = &graph.blocks[bi].nodes;
    for (unsigned i = 0; i < nodes->count; i++)
      if (!graph.nodes[nodes->data[i]].replacement && !(graph.nodes[nodes->data[i]].flags & SSA_F_DEAD)) emit_node(s, nodes->data[i]);
  }
  for (ssa_id_t id = 1; id < graph.node_count; id++) if (s->recipes[id]) {
    MIR_append_insn(CTX, FN, s->bailouts[id]);
    MIR_op_t allocate_args[] = {R(s->c.r_js), U(s->recipes[id]->value_count)};
    call(s, s->spill_proto, s->allocate_spill, s->spill, 2, allocate_args);
    MIR_label_t allocated = MIR_new_label(CTX);
    E(MIR_BNE, L(allocated), R(s->spill), I(0));
    E(MIR_MOV, R(s->result), U(mkval(kTypeError, 0)));
    E(MIR_JMP, L(s->exit));
    MIR_append_insn(CTX, FN, allocated);
    uint32_t chain[32], depth = 0;
    for (uint32_t state = graph.nodes[id].snapshot; state; state = graph.snapshots[state].parent)
      chain[depth++] = state;
    unsigned at = 0;
    for (unsigned ci = depth; ci-- > 0;) {
      ssa_snapshot_t *state = &graph.snapshots[chain[ci]];
      ssa_frame_t *context = &graph.frames[state->frame];
      ssa_id_t ids[] = {context->closure, context->receiver, context->new_target, context->super};
      for (unsigned i = 0; i < 4; i++)
        E(MIR_MOV, mem(s, MIR_JSVAL, s->spill, at++ * 8), R(value_reg(s, ids[i])));
      for (unsigned i = 0; i < state->values.count; i++)
        E(MIR_MOV, mem(s, MIR_JSVAL, s->spill, at++ * 8), R(value_reg(s, state->values.data[i])));
      if (state->frame != 1) for (unsigned i = 0; i < context->arguments.count; i++)
        E(MIR_MOV, mem(s, MIR_JSVAL, s->spill, at++ * 8), R(value_reg(s, context->arguments.data[i])));
    }
    MIR_op_t args[] = {R(s->c.r_vm), U((uintptr_t)s->recipes[id]), R(s->spill), R(s->c.r_args), R(s->c.r_argc), R(s->learning)};
    call(s, s->deopt_proto, s->deopt, s->result, 6, args);
    E(MIR_JMP, L(s->exit));
  }
  MIR_append_insn(CTX, FN, s->exit);
  MIR_op_t end_args[] = {R(s->scope)};
  call(s, s->end_proto, s->end, 0, 1, end_args);
  MIR_append_insn(CTX, FN, MIR_new_ret_insn(CTX, 1, R(s->result)));
  MIR_append_insn(CTX, FN, s->reject);
  E(MIR_MOV, mem(s, MIR_T_U8, s->c.r_vm, offsetof(sv_vm_t, jit_osr.active)), I(0));
  MIR_append_insn(CTX, FN, MIR_new_ret_insn(CTX, 1, U(SV_JIT_BAILOUT)));
  MIR_finish_func(CTX);
  MIR_finish_module(CTX);
  if (sv_jit_warn_unlikely) {
    unsigned instructions = 0;
    for (MIR_insn_t i = DLIST_HEAD(MIR_insn_t, FN->u.func->insns); i; i = DLIST_NEXT(MIR_insn_t, i)) instructions++;
    fprintf(stderr, "jit: SSA graph func=%s nodes=%u frames=%u inline=%u loads-removed=%u guards-removed=%u hoisted=%u MIR=%u\n",
        func->debug && func->debug->name ? func->debug->name : "<anonymous>", graph.node_count - 1,
        graph.frame_count - 1, graph.inlined_calls, graph.eliminated_loads, graph.eliminated_guards, graph.hoisted_guards, instructions);
  }
  if (sv_dump_jit_unlikely) { ssa_graph_dump(&graph, stderr); MIR_output_module(CTX, stderr, s->c.mod); }
  MIR_load_module(CTX, s->c.mod);
  MIR_link(CTX, MIR_set_gen_interface, NULL);
  generated = MIR_gen(CTX, FN);
  MIR_insn_t insn;
  while ((insn = DLIST_HEAD(MIR_insn_t, FN->u.func->insns)) != NULL) MIR_remove_insn(CTX, FN, insn);
  jit_release_gen_scratch(s->c.jc, CTX);
done:
  free(s->values); free(s->copies); free(s->boxed); free(s->blocks); free(s->bailouts); free(s->recipes);
  ssa_graph_destroy(&graph);
  return generated;
}
