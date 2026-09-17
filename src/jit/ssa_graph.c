#include "ssa.h"
#include "silver/feedback.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>

static bool grow(void **ptr, uint32_t *capacity, uint32_t need, size_t size) {
  if (need <= *capacity) return true;
  uint32_t next = *capacity ? *capacity : 16;
  while (next < need) {
    if (next > UINT32_MAX / 2) return false;
    next *= 2;
  }
  if (next > SIZE_MAX / size) return false;
  void *p = realloc(*ptr, next * size);
  if (!p) return false;
  memset((char *)p + *capacity * size, 0, (next - *capacity) * size);
  *ptr = p;
  *capacity = next;
  return true;
}

bool ssa_ids_push(ssa_ids_t *ids, ssa_id_t id) {
  if (!grow((void **)&ids->data, &ids->capacity, ids->count + 1, sizeof(*ids->data))) return false;
  ids->data[ids->count++] = id;
  return true;
}

void ssa_ids_free(ssa_ids_t *ids) {
  free(ids->data);
  *ids = (ssa_ids_t){0};
}

static bool fail(ssa_graph_t *g, const char *reason) {
  if (!g->failure) g->failure = reason;
  return false;
}

ssa_id_t ssa_node_add(ssa_graph_t *g, uint32_t block, uint16_t op,
                    const ssa_id_t *inputs, uint32_t count) {
  if (g->failure) return 0;
  if (g->node_count >= g->max_nodes) { fail(g, "node budget"); return 0; }
  if (!grow((void **)&g->nodes, &g->node_capacity, g->node_count + 1, sizeof(*g->nodes))) {
    fail(g, "out of memory"); return 0;
  }
  ssa_id_t id = g->node_count++;
  ssa_node_t *n = &g->nodes[id];
  n->op = op;
  n->bytecode = OP_INVALID;
  n->block = block;
  n->frame = g->blocks[block].frame;
  n->type = SSA_T_ANY;
  if (op == SSA_ARRAY_GUARD || op == SSA_SHAPE_GUARD || op == SSA_ARRAY_TRY_GUARD) { n->type = SSA_T_RAW; n->rep = SSA_PTR; }
  if (op == SSA_CHECK_NUMBER) { n->type = SSA_T_NUMBER; n->rep = SSA_F64; }
  if (op == SSA_ARRAY_LENGTH || op == SSA_ARRAY_TRY_LENGTH) { n->type = SSA_T_NUMBER; n->rep = SSA_I32; }
  for (uint32_t i = 0; i < count; i++) if (!ssa_ids_push(&n->inputs, inputs[i])) {
    fail(g, "out of memory"); return 0;
  }
  if (!ssa_ids_push(op == SSA_PHI ? &g->blocks[block].phis : &g->blocks[block].nodes, id)) {
    fail(g, "out of memory"); return 0;
  }
  return id;
}

ssa_id_t ssa_resolve(const ssa_graph_t *g, ssa_id_t id) {
  uint32_t steps = 0;
  while (id && id < g->node_count && g->nodes[id].replacement) {
    if (++steps >= g->node_count) return 0;
    id = g->nodes[id].replacement;
  }
  return id < g->node_count ? id : 0;
}

uint32_t ssa_snapshot_add(ssa_graph_t *g, uint32_t frame, uint32_t offset,
                         const ssa_id_t *values, uint32_t count, int stack) {
  if (g->failure || !grow((void **)&g->snapshots, &g->snapshot_capacity,
                         g->snapshot_count + 1, sizeof(*g->snapshots))) {
    fail(g, "out of memory"); return 0;
  }
  uint32_t id = g->snapshot_count++;
  ssa_snapshot_t *s = &g->snapshots[id];
  sv_func_t *f = g->frames[frame].func;
  s->frame = frame;
  s->offset = offset;
  s->params = f->param_count;
  s->locals = (uint16_t)f->max_locals;
  s->stack = (uint16_t)stack;
  for (uint32_t i = 0; i < count; i++) if (!ssa_ids_push(&s->values, values[i])) {
    fail(g, "out of memory"); return 0;
  }
  return id;
}

uint32_t ssa_block_add(ssa_graph_t *g, uint32_t offset, uint32_t frame) {
  if (g->block_count >= 2048) { fail(g, "block budget"); return 0; }
  if (!grow((void **)&g->blocks, &g->block_capacity,
            g->block_count + 1, sizeof(*g->blocks))) {
    fail(g, "out of memory"); return 0;
  }
  uint32_t id = g->block_count++;
  g->blocks[id] = (ssa_block_t){.offset = offset, .frame = frame, .stack_depth = -1};
  return id;
}

bool ssa_edge_add(ssa_graph_t *g, uint32_t from, uint32_t to) {
  if (!from || !to || from >= g->block_count || to >= g->block_count) return fail(g, "invalid edge");
  return (ssa_ids_push(&g->blocks[from].successors, to) &&
          ssa_ids_push(&g->blocks[to].predecessors, from)) || fail(g, "out of memory");
}

uint32_t ssa_frame_add(ssa_graph_t *g, sv_func_t *func, uint32_t parent) {
  if (g->frame_count >= g->max_frames || !grow((void **)&g->frames,
      &g->frame_capacity, g->frame_count + 1, sizeof(*g->frames))) {
    fail(g, "frame budget or out of memory"); return 0;
  }
  uint32_t id = g->frame_count++;
  g->frames[id].func = func;
  g->frames[id].parent = parent;
  return id;
}

static bool unconditional(unsigned op) { return op == OP_JMP || op == OP_JMP8; }
static bool terminal(unsigned op) {
  return op == OP_RETURN || op == OP_RETURN_UNDEF || op == OP_TAIL_CALL || op == OP_TAIL_CALL_METHOD;
}
static bool branching(unsigned op) {
  return (sv_op_flags[op] & (SV_OPF_JIT_BRANCH32 | SV_OPF_JIT_BRANCH8)) != 0;
}
static int64_t branch_target(sv_func_t *f, int pc) {
  unsigned op = f->code[pc];
  return (int64_t)pc + sv_op_size[op] +
      ((sv_op_flags[op] & SV_OPF_JIT_BRANCH32) ? sv_get_i32(f->code + pc + 1) : sv_get_i8(f->code + pc + 1));
}

// Unsupported operations reject this tier before any native code is emitted.
// In particular, a closure over this frame needs live-slot materialization;
// it cannot be treated as an ordinary opaque call over SSA-only locals.
static bool supported(unsigned op) {
  switch (op) {
    case OP_CONST: case OP_CONST8: case OP_CONST_I8:
    case OP_UNDEF: case OP_NULL: case OP_TRUE: case OP_FALSE: case OP_THIS:
    case OP_GET_ARG: case OP_PUT_ARG: case OP_SET_ARG:
    case OP_GET_LOCAL: case OP_GET_LOCAL8: case OP_PUT_LOCAL: case OP_PUT_LOCAL8:
    case OP_SET_LOCAL: case OP_SET_LOCAL8: case OP_SET_LOCAL_UNDEF:
    case OP_GET_LOCAL_CHK: case OP_PUT_LOCAL_CHK:
    case OP_GET_UPVAL: case OP_PUT_UPVAL: case OP_SET_UPVAL:
    case OP_GET_GLOBAL: case OP_GET_GLOBAL_UNDEF: case OP_PUT_GLOBAL: case OP_GLOBAL:
    case OP_GET_FIELD: case OP_GET_FIELD2: case OP_GET_FIELD_OPT: case OP_PUT_FIELD:
    case OP_GET_LENGTH: case OP_GET_ELEM: case OP_GET_ELEM2: case OP_GET_ELEM_OPT: case OP_PUT_ELEM:
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD:
    case OP_ADD_NUM: case OP_SUB_NUM: case OP_MUL_NUM: case OP_DIV_NUM:
    case OP_NEG: case OP_UPLUS: case OP_INC: case OP_DEC: case OP_POST_INC: case OP_POST_DEC:
    case OP_INC_LOCAL: case OP_DEC_LOCAL: case OP_ADD_LOCAL:
    case OP_EQ: case OP_NE: case OP_SEQ: case OP_SNE: case OP_LT: case OP_LE: case OP_GT: case OP_GE:
    case OP_BAND: case OP_BOR: case OP_BXOR: case OP_BNOT: case OP_SHL: case OP_SHR: case OP_USHR:
    case OP_NOT: case OP_TYPEOF: case OP_VOID: case OP_IS_PRIMITIVE_TYPE:
    case OP_IS_NULLISH: case OP_IS_UNDEF_OR_NULL: case OP_IS_UNDEF: case OP_IS_NULL:
    case OP_CALL: case OP_CALL_METHOD: case OP_TAIL_CALL: case OP_TAIL_CALL_METHOD:
    case OP_RETURN: case OP_RETURN_UNDEF:
    case OP_JMP: case OP_JMP8: case OP_JMP_TRUE: case OP_JMP_FALSE:
    case OP_JMP_TRUE8: case OP_JMP_FALSE8: case OP_JMP_TRUE_PEEK: case OP_JMP_FALSE_PEEK:
    case OP_JMP_NOT_NULLISH:
    case OP_POP: case OP_DUP: case OP_DUP2: case OP_SWAP: case OP_SWAP_UNDER:
    case OP_ROT3L: case OP_ROT3R: case OP_ROT4_UNDER: case OP_NIP: case OP_NIP2:
    case OP_INSERT2: case OP_INSERT3:
    case OP_NOP: case OP_LABEL: case OP_LINE_NUM: case OP_COL_NUM:
      return true;
    default: return false;
  }
}

static uint16_t constant_type(ant_value_t value) {
  switch (vtype(value)) {
    case kTypeNumber: return SSA_T_NUMBER;
    case kTypeBool: return SSA_T_BOOL;
    case kTypeUndefined: return SSA_T_UNDEFINED;
    case kTypeNull: return SSA_T_NULL;
    case kTypeString: return SSA_T_STRING;
    case kTypeFunction: case kTypeBuiltin: return SSA_T_FUNCTION;
    default: return is_object_type(value) ? SSA_T_OBJECT : SSA_T_OTHER;
  }
}

static ssa_id_t constant(ssa_graph_t *g, uint32_t block, ant_value_t value) {
  ssa_id_t id = ssa_node_add(g, block, SSA_CONSTANT, NULL, 0);
  if (id) { g->nodes[id].imm.value = value; g->nodes[id].type = constant_type(value); }
  return id;
}

static bool analyze_cfg(ssa_graph_t *g, sv_func_t *f, uint32_t *at) {
  uint8_t *boundary = calloc((size_t)f->code_len + 1, 1);
  if (!boundary) return fail(g, "out of memory");
  boundary[0] = 2;
  for (int pc = 0; pc < f->code_len;) {
    unsigned op = f->code[pc];
    int size = op < OP__COUNT ? sv_op_size[op] : 0;
    if (!size || pc + size > f->code_len || !supported(op)) {
      free(boundary); return fail(g, "unsupported or truncated bytecode");
    }
    boundary[pc] |= 1;
    if (branching(op)) {
      int64_t target = branch_target(f, pc);
      if (target < 0 || target >= f->code_len) {
        free(boundary); return fail(g, "branch outside function");
      }
      boundary[target] |= 2;
    }
    if (branching(op) || terminal(op)) boundary[pc + size] |= 2;
    pc += size;
  }
  for (int pc = 0; pc < f->code_len; pc++) {
    if ((boundary[pc] & 2) && !(boundary[pc] & 1)) {
      free(boundary); return fail(g, "branch into instruction");
    }
    if (boundary[pc] & 2) {
      at[pc] = ssa_block_add(g, pc, 1);
      if (!at[pc]) { free(boundary); return false; }
    }
  }
  free(boundary);
  if (!ssa_edge_add(g, g->entry, at[0])) return false;
  g->blocks[g->entry].stack_depth = 0;
  g->blocks[g->entry].reachable = true;
  for (uint32_t bi = 2; bi < g->block_count; bi++) {
    ssa_block_t *b = &g->blocks[bi];
    b->end = bi + 1 < g->block_count ? g->blocks[bi + 1].offset : (uint32_t)f->code_len;
    int pc = b->offset, last = pc;
    while (pc < (int)b->end) { last = pc; pc += sv_op_size[f->code[pc]]; }
    unsigned op = f->code[last];
    if (branching(op) && !ssa_edge_add(g, bi, at[branch_target(f, last)])) return false;
    if (!unconditional(op) && !terminal(op)) {
      if (b->end >= (uint32_t)f->code_len) return fail(g, "unterminated function");
      if (!ssa_edge_add(g, bi, at[b->end])) return false;
    }
  }
  ssa_ids_t queue = {0};
  if (!ssa_ids_push(&queue, at[0])) return fail(g, "out of memory");
  g->blocks[at[0]].stack_depth = 0;
  for (uint32_t qi = 0; qi < queue.count; qi++) {
    uint32_t bi = queue.data[qi];
    ssa_block_t *b = &g->blocks[bi];
    b->reachable = true;
    int depth = b->stack_depth;
    for (int pc = b->offset; pc < (int)b->end; pc += sv_op_size[f->code[pc]]) {
      int pops, pushes;
      if (!sv_op_stack_effect(f, f->code + pc, &pops, &pushes) || depth < pops ||
          depth - pops + pushes > f->max_stack) {
        ssa_ids_free(&queue); return fail(g, "invalid operand stack");
      }
      depth += pushes - pops;
    }
    for (uint32_t i = 0; i < b->successors.count; i++) {
      ssa_block_t *s = &g->blocks[b->successors.data[i]];
      if (s->stack_depth >= 0 && s->stack_depth != depth) {
        ssa_ids_free(&queue); return fail(g, "inconsistent stack merge");
      }
      if (s->stack_depth < 0) {
        s->stack_depth = depth;
        if (!ssa_ids_push(&queue, b->successors.data[i])) {
          ssa_ids_free(&queue); return fail(g, "out of memory");
        }
      }
    }
  }
  ssa_ids_free(&queue);
  return true;
}

static bool local_index(ssa_graph_t *g, sv_func_t *f, unsigned op, const uint8_t *ip, int *index) {
  bool short_op = op == OP_GET_LOCAL8 || op == OP_SET_LOCAL8 || op == OP_PUT_LOCAL8 ||
                  op == OP_INC_LOCAL || op == OP_DEC_LOCAL || op == OP_ADD_LOCAL;
  int local = short_op ? sv_get_u8(ip + 1) : sv_get_u16(ip + 1);
  if (local >= f->max_locals) return fail(g, "invalid local index");
  *index = f->param_count + local;
  return true;
}

static bool fill_edges(ssa_graph_t *g, uint32_t bi, const ssa_id_t *state, uint32_t count) {
  ssa_block_t *b = &g->blocks[bi];
  for (uint32_t e = 0; e < b->successors.count; e++) {
    ssa_block_t *to = &g->blocks[b->successors.data[e]];
    if (to->osr_stub) continue;
    uint32_t pi = 0, nth = 0;
    for (uint32_t j = 0; j < e; j++) nth += b->successors.data[j] == b->successors.data[e];
    for (; pi < to->predecessors.count; pi++) if (to->predecessors.data[pi] == bi) {
      if (!nth) break;
      nth--;
    }
    if (count != to->phis.count || pi == to->predecessors.count) return fail(g, "invalid phi edge");
    for (uint32_t i = 0; i < count; i++) g->nodes[to->phis.data[i]].inputs.data[pi] = state[i];
  }
  return true;
}

static bool build_block(ssa_graph_t *g, uint32_t bi, ssa_id_t *state, ssa_id_t undef) {
  ssa_block_t *b = &g->blocks[bi];
  sv_func_t *f = g->frames[1].func;
  int base = f->param_count + f->max_locals, sp = b->stack_depth;
  memcpy(state, b->phis.data, b->phis.count * sizeof(*state));
  for (uint32_t pc = b->offset; pc < b->end;) {
    const uint8_t *ip = f->code + pc;
    unsigned op = *ip;
    int pops, pushes, idx;
    if (!sv_op_stack_effect(f, ip, &pops, &pushes)) return fail(g, "unknown stack effect");
    uint32_t snap = ssa_snapshot_add(g, 1, pc, state, base + sp, sp);
    ssa_id_t *stack = state + base, id = 0, a = 0, c = 0;
    if (!snap) return false;
    switch (op) {
      case OP_NOP: case OP_LABEL: case OP_LINE_NUM: case OP_COL_NUM: break;
      case OP_UNDEF: stack[sp++] = undef; break;
      case OP_NULL: stack[sp++] = constant(g, bi, js_mknull()); break;
      case OP_TRUE: case OP_FALSE: stack[sp++] = constant(g, bi, op == OP_TRUE ? js_true : js_false); break;
      case OP_THIS: stack[sp++] = g->frames[1].receiver; break;
      case OP_CONST_I8: stack[sp++] = constant(g, bi, tov(sv_get_i8(ip + 1))); break;
      case OP_CONST: case OP_CONST8: {
        uint32_t k = op == OP_CONST8 ? sv_get_u8(ip + 1) : sv_get_u32(ip + 1);
        if (k >= (uint32_t)f->const_count) return fail(g, "invalid constant index");
        id = constant(g, bi, f->constants[k]);
        stack[sp++] = id;
        break;
      }
      case OP_GET_ARG: case OP_PUT_ARG: case OP_SET_ARG:
        idx = sv_get_u16(ip + 1);
        if (idx >= f->param_count) return fail(g, "invalid parameter index");
        if (op == OP_GET_ARG) stack[sp++] = state[idx];
        else { state[idx] = stack[sp - 1]; if (op == OP_PUT_ARG) sp--; }
        break;
      case OP_GET_LOCAL: case OP_GET_LOCAL8: case OP_PUT_LOCAL: case OP_PUT_LOCAL8:
      case OP_SET_LOCAL: case OP_SET_LOCAL8: case OP_SET_LOCAL_UNDEF:
      case OP_GET_LOCAL_CHK: case OP_PUT_LOCAL_CHK:
        if (!local_index(g, f, op, ip, &idx)) return false;
        if (op == OP_GET_LOCAL_CHK || op == OP_PUT_LOCAL_CHK) {
          id = ssa_node_add(g, bi, SSA_GUARD, &state[idx], 1);
          if (id) g->nodes[id].imm.value = SV_TDZ;
        }
        if (op == OP_GET_LOCAL || op == OP_GET_LOCAL8 || op == OP_GET_LOCAL_CHK) stack[sp++] = state[idx];
        else if (op == OP_SET_LOCAL_UNDEF) state[idx] = constant(g, bi, SV_TDZ);
        else { state[idx] = stack[sp - 1]; if (op != OP_SET_LOCAL && op != OP_SET_LOCAL8) sp--; }
        break;
      case OP_INC_LOCAL: case OP_DEC_LOCAL: case OP_ADD_LOCAL: {
        if (!local_index(g, f, op, ip, &idx)) return false;
        ssa_id_t in[2] = {state[idx], op == OP_ADD_LOCAL ? stack[--sp] : constant(g, bi, tov(1))};
        id = ssa_node_add(g, bi, op == OP_DEC_LOCAL ? OP_SUB : OP_ADD, in, 2);
        state[idx] = id;
        break;
      }
      case OP_POP: sp--; break;
      case OP_DUP: a = stack[sp - 1]; stack[sp++] = a; break;
      case OP_DUP2: a = stack[sp - 2]; c = stack[sp - 1]; stack[sp++] = a; stack[sp++] = c; break;
      case OP_NIP: stack[sp - 2] = stack[sp - 1]; sp--; break;
      case OP_NIP2: stack[sp - 3] = stack[sp - 1]; sp -= 2; break;
      case OP_SWAP: a = stack[sp - 1]; stack[sp - 1] = stack[sp - 2]; stack[sp - 2] = a; break;
      case OP_SWAP_UNDER: a = stack[sp - 3]; stack[sp - 3] = stack[sp - 2]; stack[sp - 2] = a; break;
      case OP_ROT3L: a = stack[sp - 3]; stack[sp - 3] = stack[sp - 2]; stack[sp - 2] = stack[sp - 1]; stack[sp - 1] = a; break;
      case OP_ROT3R: a = stack[sp - 1]; stack[sp - 1] = stack[sp - 2]; stack[sp - 2] = stack[sp - 3]; stack[sp - 3] = a; break;
      case OP_ROT4_UNDER: a = stack[sp - 2]; stack[sp - 2] = stack[sp - 3]; stack[sp - 3] = stack[sp - 4]; stack[sp - 4] = a; break;
      case OP_INSERT2: a = stack[sp - 1]; stack[sp] = a; stack[sp - 1] = stack[sp - 2]; stack[sp - 2] = a; sp++; break;
      case OP_INSERT3: a = stack[sp - 1]; stack[sp] = a; stack[sp - 1] = stack[sp - 2]; stack[sp - 2] = stack[sp - 3]; stack[sp - 3] = a; sp++; break;
      case OP_RETURN: case OP_RETURN_UNDEF:
        a = op == OP_RETURN ? stack[--sp] : undef;
        id = ssa_node_add(g, bi, SSA_RETURN, &a, 1);
        break;
      case OP_JMP: case OP_JMP8:
        id = ssa_node_add(g, bi, SSA_JUMP, NULL, 0);
        break;
      case OP_JMP_TRUE: case OP_JMP_FALSE: case OP_JMP_TRUE8: case OP_JMP_FALSE8:
      case OP_JMP_TRUE_PEEK: case OP_JMP_FALSE_PEEK: case OP_JMP_NOT_NULLISH:
        a = stack[sp - 1];
        if (op != OP_JMP_TRUE_PEEK && op != OP_JMP_FALSE_PEEK && op != OP_JMP_NOT_NULLISH) sp--;
        id = ssa_node_add(g, bi, SSA_BRANCH, &a, 1);
        break;
      case OP_CALL: case OP_CALL_METHOD: case OP_TAIL_CALL: case OP_TAIL_CALL_METHOD: {
        bool method = op == OP_CALL_METHOD || op == OP_TAIL_CALL_METHOD;
        uint32_t argc = sv_get_u16(ip + 1);
        if (argc > 64) return fail(g, "call argument budget");
        ssa_id_t in[66];
        in[0] = stack[sp - (int)argc - 1];
        in[1] = method ? stack[sp - (int)argc - 2] : undef;
        memcpy(in + 2, stack + sp - argc, argc * sizeof(*in));
        id = ssa_node_add(g, bi, SSA_CALL, in, argc + 2);
        sp -= pops;
        if (pushes) stack[sp++] = id;
        else {
          if (id) {
            g->nodes[id].snapshot = snap;
            g->nodes[id].offset = pc;
            g->nodes[id].bytecode = (uint16_t)op;
            g->nodes[id].effects = SSA_EFFECT_WORLD;
          }
          a = id;
          id = ssa_node_add(g, bi, SSA_RETURN, &a, 1);
        }
        break;
      }
      case OP_GET_FIELD2:
        a = stack[sp - 1];
        id = ssa_node_add(g, bi, OP_GET_FIELD, &a, 1);
        stack[sp++] = id;
        break;
      case OP_GET_ELEM2: {
        ssa_id_t in[2] = {stack[sp - 2], stack[sp - 1]};
        id = ssa_node_add(g, bi, OP_GET_ELEM, in, 2);
        stack[sp - 1] = id;
        break;
      }
      case OP_POST_INC: case OP_POST_DEC:
        a = stack[sp - 1];
        id = ssa_node_add(g, bi, op == OP_POST_INC ? OP_INC : OP_DEC, &a, 1);
        // The number guard in lowering must precede both old and new results.
        stack[sp++] = id;
        break;
      case OP_SET_UPVAL:
        a = stack[sp - 1];
        id = ssa_node_add(g, bi, OP_PUT_UPVAL, &a, 1);
        break;
      case OP_VOID: stack[sp - 1] = undef; break;
      default:
        id = ssa_node_add(g, bi, (uint16_t)op, stack + sp - pops, (uint32_t)pops);
        sp -= pops;
        if (pushes) {
          if (pushes != 1) return fail(g, "unmodelled multi-result operation");
          stack[sp++] = id;
        }
        break;
    }
    if (g->failure) return false;
    if (id) {
      ssa_node_t *n = &g->nodes[id];
      n->bytecode = (uint16_t)op;
      n->offset = pc;
      n->snapshot = snap;
      switch (n->op) {
        case SSA_CALL: case OP_GET_GLOBAL: case OP_GET_GLOBAL_UNDEF:
        case OP_PUT_GLOBAL: case OP_PUT_FIELD: case OP_PUT_ELEM:
          n->effects = SSA_EFFECT_WORLD;
          break;
        case OP_PUT_UPVAL: n->effects = SSA_EFFECT_CELL; break;
        default: break;
      }
    }
    pc += sv_op_size[op];
  }
  if (b->successors.count) {
    // End-of-block falls through unless the last emitted node is a terminator.
    ssa_node_t *last = b->nodes.count ? &g->nodes[b->nodes.data[b->nodes.count - 1]] : NULL;
    if (!last || (last->op != SSA_JUMP && last->op != SSA_BRANCH && last->op != SSA_RETURN)) {
      ssa_id_t jump = ssa_node_add(g, bi, SSA_JUMP, NULL, 0);
      uint32_t snapshot = ssa_snapshot_add(g, 1, b->end, state, base + sp, sp);
      if (jump) {
        g->nodes[jump].snapshot = snapshot;
        g->nodes[jump].offset = b->end;
      }
    }
  }
  return !g->failure && fill_edges(g, bi, state, base + sp);
}

bool ssa_graph_build_mode(ssa_graph_t *g, sv_func_t *f, bool osr) {
  *g = (ssa_graph_t){.node_count = 1, .block_count = 1, .snapshot_count = 1,
      .frame_count = 2, .max_nodes = 16000, .max_frames = 32};
  if (!f || !f->code || f->code_len <= 0 || f->code_len > 16384 || f->max_locals < 0 ||
      f->max_stack < 0 || f->param_count + f->max_locals + f->max_stack > 512 ||
      f->is_async || f->has_await || f->is_generator || f->has_dynamic_eval || f->is_derived_ctor)
    return fail(g, "unsupported frame");
  if (!grow((void **)&g->frames, &g->frame_capacity, 2, sizeof(*g->frames))) return fail(g, "out of memory");
  g->frames[1].func = f;
  g->entry = ssa_block_add(g, UINT32_MAX, 1);
  g->frames[1].entry = g->entry;
  uint32_t *at = calloc((size_t)f->code_len, sizeof(*at));
  if (!at || !g->entry) { free(at); return fail(g, "out of memory"); }
  bool ok = analyze_cfg(g, f, at);
  free(at);
  if (!ok) return false;
  uint32_t bytecode_blocks = g->block_count;
  if (osr) {
    // Model every OSR entry as a real CFG predecessor before phi simplification.
    // A value available on normal entry is not necessarily available at OSR.
    for (uint32_t bi = 2; bi < bytecode_blocks; bi++) {
      if (!g->blocks[bi].reachable) continue;
      for (unsigned e = 0; e < g->blocks[bi].successors.count; e++) {
        uint32_t target = g->blocks[bi].successors.data[e];
        if (target > bi || g->blocks[target].osr_entry) continue;
        g->blocks[target].osr_entry = true;
        uint32_t stub = ssa_block_add(g, g->blocks[target].offset, 1);
        if (!stub) return false;
        g->blocks[stub].osr_stub = g->blocks[stub].reachable = true;
        g->blocks[stub].stack_depth = g->blocks[target].stack_depth;
        if (!ssa_edge_add(g, g->entry, stub) || !ssa_edge_add(g, stub, target)) return false;
      }
    }
  }
  int base = f->param_count + f->max_locals;
  ssa_id_t *state = calloc((size_t)(base + f->max_stack + 4), sizeof(*state));
  if (!state) return fail(g, "out of memory");
  ssa_id_t undef = constant(g, g->entry, js_mkundef());
  g->frames[1].receiver = ssa_node_add(g, g->entry, SSA_THIS, NULL, 0);
  g->frames[1].closure = ssa_node_add(g, g->entry, SSA_CLOSURE, NULL, 0);
  if (g->frames[1].receiver && !f->is_strict && !f->is_arrow)
    g->nodes[g->frames[1].receiver].type = SSA_T_OBJECT | SSA_T_FUNCTION;
  if (g->frames[1].closure) g->nodes[g->frames[1].closure].type = SSA_T_FUNCTION;
  g->frames[1].new_target = ssa_node_add(g, g->entry, SSA_NEW_TARGET, NULL, 0);
  g->frames[1].super = ssa_node_add(g, g->entry, SSA_SUPER, NULL, 0);
  for (int i = 0; i < f->param_count; i++) {
    state[i] = ssa_node_add(g, g->entry, SSA_PARAMETER, NULL, 0);
    if (state[i]) g->nodes[state[i]].imm.u64 = i;
    if (!ssa_ids_push(&g->frames[1].arguments, state[i])) fail(g, "out of memory");
  }
  for (int i = f->param_count; i < base; i++) state[i] = undef;
  ssa_id_t entry_jump = ssa_node_add(g, g->entry, osr ? SSA_ENTRY : SSA_JUMP, NULL, 0);
  uint32_t entry_snapshot = ssa_snapshot_add(g, 1, 0, state, (uint32_t)base, 0);
  if (entry_jump) g->nodes[entry_jump].snapshot = entry_snapshot;

  // Allocate merge nodes before evaluating any block, including backedges.
  // Inputs are filled by predecessor edges; subsequent simplification removes
  // identity phis. No type or representation is guessed from forward order.
  for (uint32_t bi = 2; bi < g->block_count && !g->failure; bi++) {
    ssa_block_t *b = &g->blocks[bi];
    if (!b->reachable || b->osr_stub) continue;
    ssa_id_t *empty = calloc(b->predecessors.count, sizeof(*empty));
    if (!empty) { fail(g, "out of memory"); break; }
    for (int slot = 0; slot < base + b->stack_depth; slot++) {
      ssa_id_t phi = ssa_node_add(g, bi, SSA_PHI, empty, b->predecessors.count);
      if (phi) g->nodes[phi].imm.u64 = slot;
    }
    free(empty);
  }
  if (!g->failure) ok = fill_edges(g, g->entry, state, base);
  for (uint32_t bi = 2; bi < bytecode_blocks && ok && !g->failure; bi++)
    if (g->blocks[bi].reachable) ok = build_block(g, bi, state, undef);
  for (uint32_t bi = bytecode_blocks; bi < g->block_count && ok && !g->failure; bi++) {
    int count = base + g->blocks[bi].stack_depth;
    for (int slot = 0; slot < count; slot++) {
      state[slot] = ssa_node_add(g, bi, SSA_OSR_VALUE, NULL, 0);
      if (state[slot]) g->nodes[state[slot]].imm.u64 = slot;
    }
    ssa_id_t jump = ssa_node_add(g, bi, SSA_JUMP, NULL, 0);
    uint32_t snap = ssa_snapshot_add(g, 1, g->blocks[bi].offset, state, count, g->blocks[bi].stack_depth);
    if (jump) g->nodes[jump].snapshot = snap;
    ok = fill_edges(g, bi, state, count);
  }
  free(state);
  if (!ok || g->failure) return false;
  ssa_simplify_phis(g);
  if (!ssa_compute_dominators(g)) return false;
  return ssa_graph_verify(g, NULL);
}

bool ssa_graph_build(ssa_graph_t *g, sv_func_t *f) {
  return ssa_graph_build_mode(g, f, true);
}

bool ssa_dominates(const ssa_graph_t *g, uint32_t definition, uint32_t use) {
  for (uint32_t steps = 0; use && use < g->block_count && steps < g->block_count; steps++) {
    if (use == definition) return true;
    uint32_t next = g->blocks[use].idom;
    if (next == use) break;
    use = next;
  }
  return false;
}

bool ssa_compute_dominators(ssa_graph_t *g) {
  uint32_t words = (g->block_count + 63) / 64;
  uint64_t *sets = calloc((size_t)g->block_count * words, sizeof(*sets));
  uint64_t *next = malloc(words * sizeof(*next));
  if (!sets || !next) { free(sets); free(next); return fail(g, "out of memory"); }
  for (uint32_t bi = 1; bi < g->block_count; bi++) if (g->blocks[bi].reachable) {
    for (uint32_t d = 1; d < g->block_count; d++)
      if (g->blocks[d].reachable && (bi != g->entry || d == bi))
        sets[(size_t)bi * words + d / 64] |= UINT64_C(1) << (d % 64);
  }
  bool changed;
  do {
    changed = false;
    for (uint32_t bi = 1; bi < g->block_count; bi++) {
      ssa_block_t *b = &g->blocks[bi];
      if (!b->reachable || bi == g->entry) continue;
      memset(next, 255, words * sizeof(*next));
      bool any = false;
      for (uint32_t j = 0; j < b->predecessors.count; j++) {
        uint32_t pred = b->predecessors.data[j];
        if (!pred || pred >= g->block_count) { free(sets); free(next); return fail(g, "invalid predecessor"); }
        if (!g->blocks[pred].reachable) continue;
        for (uint32_t w = 0; w < words; w++) next[w] &= sets[(size_t)pred * words + w];
        any = true;
      }
      if (!any) { free(sets); free(next); return fail(g, "disconnected reachable block"); }
      next[bi / 64] |= UINT64_C(1) << (bi % 64);
      uint64_t *row = sets + (size_t)bi * words;
      if (memcmp(row, next, words * sizeof(*next))) {
        memcpy(row, next, words * sizeof(*next));
        changed = true;
      }
    }
  } while (changed);
  for (uint32_t bi = 1; bi < g->block_count; bi++) {
    ssa_block_t *b = &g->blocks[bi];
    b->idom = 0;
    if (!b->reachable) continue;
    if (bi == g->entry) { b->idom = bi; continue; }
    uint32_t best_depth = 0;
    for (uint32_t d = 1; d < g->block_count; d++) {
      if (d == bi || !(sets[(size_t)bi * words + d / 64] & (UINT64_C(1) << (d % 64)))) continue;
      uint32_t depth = 0;
      for (uint32_t w = 0; w < words; w++) depth += (uint32_t)__builtin_popcountll(sets[(size_t)d * words + w]);
      if (depth > best_depth) { best_depth = depth; b->idom = d; }
    }
    if (!b->idom) { free(sets); free(next); return fail(g, "missing dominator"); }
  }
  free(sets);
  free(next);
  return true;
}

void ssa_simplify_phis(ssa_graph_t *g) {
  bool changed;
  do {
    changed = false;
    for (ssa_id_t id = 1; id < g->node_count; id++) {
      ssa_node_t *n = &g->nodes[id];
      if (n->op != SSA_PHI || n->replacement) continue;
      ssa_id_t other = 0;
      bool same = true;
      for (uint32_t j = 0; j < n->inputs.count; j++) {
        if (!g->blocks[g->blocks[n->block].predecessors.data[j]].reachable) continue;
        ssa_id_t input = ssa_resolve(g, n->inputs.data[j]);
        if (!input || input == id) continue;
        if (other && input != other) { same = false; break; }
        other = input;
      }
      if (same && other) { n->replacement = other; changed = true; }
    }
  } while (changed);
}

bool ssa_graph_verify(const ssa_graph_t *g, FILE *out) {
#define CHECK(condition, message) do { if (!(condition)) { if (out) fprintf(out, "SSA: %s\n", message); return false; } } while (0)
  CHECK(!g->failure && g->entry && g->entry < g->block_count, "invalid graph");
  for (uint32_t bi = 1; bi < g->block_count; bi++) {
    const ssa_block_t *b = &g->blocks[bi];
    if (!b->reachable) continue;
    CHECK(b->frame && b->frame < g->frame_count, "invalid block frame");
    CHECK(b->nodes.count, "empty reachable block");
    const ssa_node_t *last = &g->nodes[b->nodes.data[b->nodes.count - 1]];
    CHECK((last->op == SSA_DEOPT && b->successors.count == 0) ||
          (last->op == SSA_ENTRY && bi == g->entry && b->successors.count >= 1) ||
          (last->op == SSA_RETURN && b->successors.count == 0) ||
          (last->op == SSA_JUMP && b->successors.count == 1) ||
          (last->op == SSA_BRANCH && b->successors.count == 2), "invalid terminator");
    for (uint32_t i = 0; i < b->phis.count; i++) {
      const ssa_node_t *n = &g->nodes[b->phis.data[i]];
      CHECK(n->op == SSA_PHI && n->inputs.count == b->predecessors.count, "phi arity");
      for (uint32_t p = 0; p < b->predecessors.count; p++)
        if (g->blocks[b->predecessors.data[p]].reachable) CHECK(ssa_resolve(g, n->inputs.data[p]), "missing phi input");
    }
  }
  for (ssa_id_t id = 1; id < g->node_count; id++) {
    const ssa_node_t *n = &g->nodes[id];
    CHECK(ssa_resolve(g, id), "cyclic replacement");
    CHECK(n->block && n->block < g->block_count, "invalid node block");
    if (!g->blocks[n->block].reachable) continue;
    for (uint32_t j = 0; j < n->inputs.count; j++) {
      if (n->op == SSA_PHI && !n->inputs.data[j]) continue; // Unreachable predecessor.
      ssa_id_t input = ssa_resolve(g, n->inputs.data[j]);
      CHECK(input, "invalid input");
      uint32_t use = n->op == SSA_PHI ? g->blocks[n->block].predecessors.data[j] : n->block;
      if (!g->blocks[use].reachable) continue;
      uint32_t definition = g->nodes[input].block;
      CHECK(ssa_dominates(g, definition, use), "input does not dominate use");
      if (definition == use && n->op != SSA_PHI && g->nodes[input].op != SSA_PHI) {
        const ssa_ids_t *list = &g->blocks[use].nodes;
        bool earlier = false;
        for (uint32_t k = 0; k < list->count && list->data[k] != id; k++)
          if (list->data[k] == input) earlier = true;
        CHECK(earlier, "input defined after use");
      }
    }
    CHECK(n->snapshot < g->snapshot_count, "invalid snapshot");
    unsigned depth = 0;
    for (uint32_t state = n->snapshot; state; state = g->snapshots[state].parent) {
      CHECK(state < g->snapshot_count && ++depth <= g->frame_count, "invalid snapshot chain");
      const ssa_snapshot_t *s = &g->snapshots[state];
      CHECK(s->frame && s->frame < g->frame_count, "snapshot frame");
      if (s->parent) {
        CHECK(s->parent < g->snapshot_count, "invalid parent snapshot");
        CHECK(g->frames[s->frame].parent == g->snapshots[s->parent].frame, "snapshot parent frame");
      }
      for (uint32_t j = 0; j < s->values.count; j++) {
        ssa_id_t value = ssa_resolve(g, s->values.data[j]);
        CHECK(value && ssa_dominates(g, g->nodes[value].block, n->block), "snapshot value does not dominate");
        CHECK(g->nodes[value].type != SSA_T_RAW, "native pointer in interpreter state");
        if (g->nodes[value].block == n->block && g->nodes[value].op != SSA_PHI) {
          const ssa_ids_t *list = &g->blocks[n->block].nodes;
          bool earlier = false;
          for (uint32_t k = 0; k < list->count && list->data[k] != id; k++)
            if (list->data[k] == value) earlier = true;
          CHECK(earlier, "snapshot value defined after guard");
        }
      }
      const ssa_frame_t *frame = &g->frames[s->frame];
      ssa_id_t contexts[] = {frame->closure, frame->receiver, frame->new_target, frame->super};
      for (unsigned j = 0; j < 4 + (s->frame == 1 ? 0 : frame->arguments.count); j++) {
        ssa_id_t value = ssa_resolve(g, j < 4 ? contexts[j] : frame->arguments.data[j - 4]);
        CHECK(value && ssa_dominates(g, g->nodes[value].block, n->block), "frame context does not dominate");
        if (g->nodes[value].block == n->block && g->nodes[value].op != SSA_PHI) {
          const ssa_ids_t *list = &g->blocks[n->block].nodes;
          bool earlier = false;
          for (uint32_t k = 0; k < list->count && list->data[k] != id; k++)
            if (list->data[k] == value) earlier = true;
          CHECK(earlier, "frame context defined after guard");
        }
      }
    }
  }
  for (uint32_t i = 1; i < g->snapshot_count; i++) {
    const ssa_snapshot_t *s = &g->snapshots[i];
    CHECK(s->values.count == (uint32_t)s->params + s->locals + s->stack, "snapshot size");
    CHECK(s->frame && s->frame < g->frame_count, "snapshot frame");
    CHECK(s->offset < (uint32_t)g->frames[s->frame].func->code_len, "snapshot offset");
    for (uint32_t j = 0; j < s->values.count; j++) CHECK(ssa_resolve(g, s->values.data[j]), "snapshot value");
  }
  return true;
#undef CHECK
}

void ssa_graph_destroy(ssa_graph_t *g) {
  for (uint32_t i = 1; i < g->node_count; i++) ssa_ids_free(&g->nodes[i].inputs);
  for (uint32_t i = 1; i < g->block_count; i++) {
    ssa_ids_free(&g->blocks[i].predecessors); ssa_ids_free(&g->blocks[i].successors);
    ssa_ids_free(&g->blocks[i].nodes); ssa_ids_free(&g->blocks[i].phis);
  }
  for (uint32_t i = 1; i < g->snapshot_count; i++) ssa_ids_free(&g->snapshots[i].values);
  if (g->frames) for (uint32_t i = 1; i < g->frame_count; i++) ssa_ids_free(&g->frames[i].arguments);
  free(g->nodes); free(g->blocks); free(g->snapshots); free(g->frames);
  *g = (ssa_graph_t){0};
}

void ssa_graph_dump(const ssa_graph_t *g, FILE *out) {
  static const char *const names[] = {
    "PHI", "PARAMETER", "THIS", "CLOSURE", "NEW_TARGET", "SUPER", "CONSTANT", "BRANCH", "JUMP", "RETURN",
    "TARGET_TEST", "RESOLVE_THIS", "CALL", "GUARD", "ENTRY", "OSR_VALUE", "ROOT_CLOSURE",
    "ARRAY_GUARD", "ARRAY_LENGTH", "LOOP_RANGE", "DEOPT", "SHAPE_GUARD", "LOAD_FIELD", "CHECK_NUMBER",
    "ARRAY_TRY_GUARD", "ARRAY_TRY_LENGTH"
  };
  fprintf(out, "SSA nodes=%u blocks=%u frames=%u snapshots=%u\n",
          g->node_count - 1, g->block_count - 1, g->frame_count - 1, g->snapshot_count - 1);
  for (uint32_t bi = 1; bi < g->block_count; bi++) {
    const ssa_block_t *b = &g->blocks[bi];
    if (!b->reachable) continue;
    fprintf(out, "B%u frame=%u bc=%u idom=%u preds", bi, b->frame, b->offset, b->idom);
    for (uint32_t j = 0; j < b->predecessors.count; j++) fprintf(out, " B%u", b->predecessors.data[j]);
    fprintf(out, "\n");
    for (int section = 0; section < 2; section++) {
      const ssa_ids_t *list = section ? &b->nodes : &b->phis;
      for (uint32_t j = 0; j < list->count; j++) {
        ssa_id_t id = list->data[j];
        const ssa_node_t *n = &g->nodes[id];
        const char *name = n->op < OP__COUNT ? sv_op_names[n->op] :
            n->op - OP__COUNT < sizeof(names) / sizeof(*names) ? names[n->op - OP__COUNT] : "?";
        fprintf(out, "  v%u op=%s bc=%u rep=%u state=%u effects=%u flags=%u", id, name, n->offset, n->rep, n->snapshot, n->effects, n->flags);
        for (uint32_t k = 0; k < n->inputs.count; k++) fprintf(out, " v%u", n->inputs.data[k]);
        if (n->replacement) fprintf(out, " -> v%u", ssa_resolve(g, id));
        fprintf(out, "\n");
      }
    }
  }
}
