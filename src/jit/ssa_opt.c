#include "ssa.h"
#include "silver/feedback.h"
#include "shapes.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>

static bool arithmetic(unsigned op) {
  switch (op) {
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV:
    case OP_ADD_NUM: case OP_SUB_NUM: case OP_MUL_NUM: case OP_DIV_NUM:
    case OP_INC: case OP_DEC: case OP_NEG: case OP_UPLUS: return true;
    default: return false;
  }
}

static bool word(unsigned op) {
  return op == OP_BAND || op == OP_BOR || op == OP_BXOR ||
         op == OP_SHL || op == OP_SHR || op == OP_USHR;
}

static bool boolean_result(unsigned op) {
  switch (op) {
    case OP_LT: case OP_LE: case OP_GT: case OP_GE: case OP_EQ: case OP_NE:
    case OP_SEQ: case OP_SNE: case OP_NOT: case OP_IS_UNDEF: case OP_IS_NULL:
    case OP_IS_NULLISH: case OP_IS_UNDEF_OR_NULL: case SSA_TARGET_TEST: return true;
    default: return false;
  }
}

static bool constant_truth(ant_value_t value, bool *truth) {
  switch (vtype(value)) {
    case kTypeNumber: *truth = tod(value) != 0 && !isnan(tod(value)); return true;
    case kTypeBool: *truth = vdata(value) != 0; return true;
    case kTypeNull: case kTypeUndefined: *truth = false; return true;
    default: return false;
  }
}

static bool simplify(ssa_graph_t *g) {
  bool changed;
  do {
    changed = false;
    for (ssa_id_t id = 1; id < g->node_count; id++) {
      ssa_node_t *n = &g->nodes[id];
      if (n->replacement || !g->blocks[n->block].reachable || !n->inputs.count) continue;
      const ssa_node_t *a = &g->nodes[ssa_resolve(g, n->inputs.data[0])];
      if (a->op != SSA_CONSTANT) continue;
      if (n->op == SSA_BRANCH) {
        bool truth;
        if (n->bytecode == OP_JMP_NOT_NULLISH) truth = a->imm.value != js_mkundef() && a->imm.value != js_mknull();
        else if (!constant_truth(a->imm.value, &truth)) continue;
        bool inverse = n->bytecode == OP_JMP_FALSE || n->bytecode == OP_JMP_FALSE8 || n->bytecode == OP_JMP_FALSE_PEEK;
        unsigned taken = truth != inverse ? 0 : 1, removed = 1 - taken;
        ssa_block_t *block = &g->blocks[n->block];
        uint32_t target = block->successors.data[removed];
        unsigned occurrence = removed && block->successors.data[0] == target ? 1 : 0;
        ssa_block_t *to = &g->blocks[target];
        unsigned pi = 0;
        for (; pi < to->predecessors.count; pi++) if (to->predecessors.data[pi] == n->block) {
          if (!occurrence) break;
          occurrence--;
        }
        if (pi == to->predecessors.count) return false;
        memmove(to->predecessors.data + pi, to->predecessors.data + pi + 1,
            (to->predecessors.count - pi - 1) * sizeof(ssa_id_t));
        to->predecessors.count--;
        for (unsigned i = 0; i < to->phis.count; i++) {
          ssa_ids_t *inputs = &g->nodes[to->phis.data[i]].inputs;
          memmove(inputs->data + pi, inputs->data + pi + 1, (inputs->count - pi - 1) * sizeof(ssa_id_t));
          inputs->count--;
        }
        block->successors.data[0] = block->successors.data[taken];
        block->successors.count = 1;
        n->op = SSA_JUMP; n->inputs.count = 0;
        changed = true;
        continue;
      }
      if (vtype(a->imm.value) != kTypeNumber) continue;
      double left = tod(a->imm.value), right = 1, result = 0;
      if (n->inputs.count > 1) {
        const ssa_node_t *b = &g->nodes[ssa_resolve(g, n->inputs.data[1])];
        if (b->op != SSA_CONSTANT || vtype(b->imm.value) != kTypeNumber) continue;
        right = tod(b->imm.value);
      }
      bool boolean = false;
      switch (n->op) {
        case OP_ADD: case OP_ADD_NUM: case OP_INC: result = left + right; break;
        case OP_SUB: case OP_SUB_NUM: case OP_DEC: result = left - right; break;
        case OP_MUL: case OP_MUL_NUM: result = left * right; break;
        case OP_DIV: case OP_DIV_NUM: result = left / right; break;
        case OP_MOD: result = fmod(left, right); break;
        case OP_NEG: result = -left; break;
        case OP_UPLUS: result = left; break;
        case OP_LT: result = left < right; boolean = true; break;
        case OP_LE: result = left <= right; boolean = true; break;
        case OP_GT: result = left > right; boolean = true; break;
        case OP_GE: result = left >= right; boolean = true; break;
        case OP_EQ: case OP_SEQ: result = left == right; boolean = true; break;
        case OP_NE: case OP_SNE: result = left != right; boolean = true; break;
        default: continue;
      }
      n->op = SSA_CONSTANT; n->bytecode = OP_INVALID; n->inputs.count = 0;
      n->imm.value = boolean ? js_bool(result != 0) : tov(result);
      n->type = boolean ? SSA_T_BOOL : SSA_T_NUMBER;
      n->effects = SSA_EFFECT_NONE;
      changed = true;
    }
    if (changed) {
      for (uint32_t i = 1; i < g->block_count; i++) g->blocks[i].reachable = false;
      ssa_ids_t queue = {0};
      if (!ssa_ids_push(&queue, g->entry)) return false;
      g->blocks[g->entry].reachable = true;
      for (unsigned i = 0; i < queue.count; i++) {
        ssa_ids_t *edges = &g->blocks[queue.data[i]].successors;
        for (unsigned j = 0; j < edges->count; j++) if (!g->blocks[edges->data[j]].reachable) {
          g->blocks[edges->data[j]].reachable = true;
          if (!ssa_ids_push(&queue, edges->data[j])) { ssa_ids_free(&queue); return false; }
        }
      }
      ssa_ids_free(&queue);
      ssa_simplify_phis(g);
    }
  } while (changed);
  return ssa_compute_dominators(g);
}

// Successful arithmetic is numeric: non-numbers leave through the pre-effect
// deopt guard. Phi candidates are solved together across cycles. OSR sources
// may enter an unboxed phi only through a checked edge conversion; they never
// acquire the phi's type merely from the normal-entry initializer.
static bool representations(ssa_graph_t *g) {
  uint8_t *number = calloc(g->node_count, 1), *integer = calloc(g->node_count, 1), *boolean = calloc(g->node_count, 1);
  uint8_t *numeric_use = calloc(g->node_count, 1);
  if (!number || !integer || !boolean || !numeric_use) {
    free(number); free(integer); free(boolean); free(numeric_use); return false;
  }
  // A boxed entry value must not force a numeric recurrence back into boxed
  // registers on every backedge. Propagate numeric demand through phi chains;
  // uncertain incoming values then use the existing checked edge conversion.
  // Keep the source value's type unchanged: the check establishes the phi's
  // type, and a failure resumes at that edge before executing any new effect.
  for (ssa_id_t id = 1; id < g->node_count; id++) {
    const ssa_node_t *n = &g->nodes[id];
    if (n->replacement || !g->blocks[n->block].reachable) continue;
    if (!arithmetic(n->op) && !word(n->op) && n->op != OP_MOD &&
        n->op != OP_LT && n->op != OP_LE && n->op != OP_GT && n->op != OP_GE) continue;
    for (unsigned i = 0; i < n->inputs.count; i++) numeric_use[ssa_resolve(g, n->inputs.data[i])] = 1;
  }
  bool demand_changed;
  do {
    demand_changed = false;
    for (ssa_id_t id = 1; id < g->node_count; id++) {
      const ssa_node_t *n = &g->nodes[id];
      if (n->replacement || n->op != SSA_PHI || !numeric_use[id]) continue;
      for (unsigned i = 0; i < n->inputs.count; i++) {
        ssa_id_t input = ssa_resolve(g, n->inputs.data[i]);
        if (!numeric_use[input]) { numeric_use[input] = 1; demand_changed = true; }
      }
    }
  } while (demand_changed);
  for (ssa_id_t id = 1; id < g->node_count; id++) {
    ssa_node_t *n = &g->nodes[id];
    if (n->replacement) continue;
    number[id] = n->op == SSA_PHI || arithmetic(n->op) || word(n->op) || n->op == OP_MOD ||
        n->op == SSA_ARRAY_LENGTH || n->op == SSA_ARRAY_TRY_LENGTH || n->op == SSA_CHECK_NUMBER ||
        (n->flags & SSA_F_CHECK_NUMBER);
    integer[id] = n->op == SSA_PHI || word(n->op) || n->op == SSA_ARRAY_LENGTH || n->op == SSA_ARRAY_TRY_LENGTH;
    boolean[id] = n->op == SSA_PHI || boolean_result(n->op) ||
        (n->op == SSA_CONSTANT && vtype(n->imm.value) == kTypeBool);
    if (n->op == SSA_CONSTANT && vtype(n->imm.value) == kTypeNumber) {
      double d = tod(n->imm.value);
      number[id] = true;
      integer[id] = isfinite(d) && !(d == 0 && signbit(d)) &&
          d >= INT32_MIN && d <= UINT32_MAX && d == (double)(int64_t)d;
      if (integer[id]) { n->range_known = true; n->min = n->max = (int64_t)d; }
    }
    if (word(n->op)) {
      n->range_known = true;
      n->min = n->op == OP_USHR ? 0 : INT32_MIN;
      n->max = n->op == OP_USHR ? UINT32_MAX : INT32_MAX;
      if (n->op == OP_BAND) for (unsigned i = 0; i < n->inputs.count; i++) {
        const ssa_node_t *input = &g->nodes[ssa_resolve(g, n->inputs.data[i])];
        if (input->op == SSA_CONSTANT && vtype(input->imm.value) == kTypeNumber &&
            tod(input->imm.value) >= 0 && tod(input->imm.value) <= INT32_MAX) {
          n->min = 0; n->max = (int64_t)tod(input->imm.value);
        }
      }
    }
  }
  bool changed;
  do {
    changed = false;
    for (ssa_id_t id = 1; id < g->node_count; id++) {
      ssa_node_t *n = &g->nodes[id];
      if (n->replacement || n->op != SSA_PHI) continue;
      for (unsigned i = 0; i < n->inputs.count; i++) {
        ssa_id_t input = ssa_resolve(g, n->inputs.data[i]);
        if (!input || g->nodes[input].op == SSA_OSR_VALUE) continue;
        bool checked_input = numeric_use[id] && (g->nodes[input].type & SSA_T_NUMBER);
        if (number[id] && !number[input] && !checked_input) { number[id] = 0; changed = true; }
        if (integer[id] && !integer[input]) { integer[id] = 0; changed = true; }
        if (boolean[id] && !boolean[input]) { boolean[id] = 0; changed = true; }
      }
    }
  } while (changed);
  for (ssa_id_t id = 1; id < g->node_count; id++) {
    ssa_node_t *n = &g->nodes[id];
    if (n->replacement) continue;
    if (!number[id]) {
      if (boolean[id]) { n->rep = SSA_BOOL; n->type = SSA_T_BOOL; }
      continue;
    }
    n->type = SSA_T_NUMBER;
    if (n->flags & SSA_F_NO_OVERFLOW) { n->rep = SSA_I53; continue; }
    n->rep = integer[id] ? SSA_I32 : SSA_F64;
    if (n->op == OP_MOD) n->rep = SSA_BOXED;
    if (n->op == SSA_PHI && integer[id]) {
      n->range_known = true; n->min = INT32_MIN; n->max = UINT32_MAX;
    }
  }
  free(number); free(integer); free(boolean); free(numeric_use);
  return true;
}

enum { MEMORY_PROPERTY = 1, MEMORY_CELL = 2, MEMORY_GLOBAL = 4 };

static bool precedes(const ssa_graph_t *g, ssa_id_t definition, ssa_id_t use) {
  uint32_t db = g->nodes[definition].block, ub = g->nodes[use].block;
  if (!ssa_dominates(g, db, ub)) return false;
  if (db != ub || g->nodes[definition].op == SSA_PHI) return true;
  const ssa_ids_t *list = &g->blocks[ub].nodes;
  for (unsigned i = 0; i < list->count && list->data[i] != use; i++)
    if (list->data[i] == definition) return true;
  return false;
}

static bool simplify_values(ssa_graph_t *g) {
  ssa_id_t global = 0;
  for (ssa_id_t id = 1; id < g->node_count; id++) {
    ssa_node_t *n = &g->nodes[id];
    if (n->replacement || !g->blocks[n->block].reachable) continue;
    if (n->op == SSA_RESOLVE_THIS && !n->imm.target->is_arrow) {
      ssa_id_t receiver = ssa_resolve(g, n->inputs.data[1]);
      const ssa_node_t *input = &g->nodes[receiver];
      if (!n->imm.target->is_strict && input->op == SSA_CONSTANT &&
          (input->imm.value == js_mkundef() || input->imm.value == js_mknull())) {
        if (!global) {
          global = ssa_node_add(g, g->entry, OP_GLOBAL, NULL, 0);
          if (!global) return false;
          g->nodes[global].type = SSA_T_OBJECT;
          ssa_ids_t *entry = &g->blocks[g->entry].nodes;
          ssa_id_t term = entry->data[entry->count - 2];
          entry->data[entry->count - 2] = global;
          entry->data[entry->count - 1] = term;
        }
        g->nodes[id].replacement = global;
        continue;
      }
      unsigned type = g->nodes[receiver].type;
      bool object = type && !(type & ~(SSA_T_OBJECT | SSA_T_FUNCTION));
      for (ssa_id_t guard = 1; !object && guard < g->node_count; guard++)
        if (!g->nodes[guard].replacement && g->nodes[guard].op == SSA_SHAPE_GUARD &&
            ssa_resolve(g, g->nodes[guard].inputs.data[0]) == receiver && precedes(g, guard, id)) object = true;
      // Target tests exclude bound-this wrappers on ordinary functions.
      if (n->imm.target->is_strict || object) { n->replacement = receiver; continue; }
    }
    if (!arithmetic(n->op) && !word(n->op)) continue;
    for (ssa_id_t other = 1; other < id; other++) {
      const ssa_node_t *p = &g->nodes[other];
      if (p->replacement || p->op != n->op || p->inputs.count != n->inputs.count || !precedes(g, other, id)) continue;
      bool same = true;
      for (unsigned i = 0; i < n->inputs.count; i++)
        same &= ssa_resolve(g, p->inputs.data[i]) == ssa_resolve(g, n->inputs.data[i]);
      if (same) { n->replacement = other; break; }
    }
  }
  return true;
}
static unsigned load_kind(unsigned op) {
  switch (op) {
    case OP_GET_FIELD: case OP_GET_FIELD_OPT: case OP_GET_ELEM:
    case OP_GET_ELEM_OPT: case OP_GET_LENGTH: return MEMORY_PROPERTY;
    case SSA_SHAPE_GUARD: case SSA_LOAD_FIELD: return MEMORY_PROPERTY;
    case SSA_ARRAY_TRY_GUARD: case SSA_ARRAY_TRY_LENGTH: return MEMORY_PROPERTY;
    case OP_GET_UPVAL: return MEMORY_CELL;
    case OP_GET_GLOBAL: case OP_GET_GLOBAL_UNDEF: return MEMORY_GLOBAL;
    default: return 0;
  }
}
static unsigned invalidates(unsigned op) {
  switch (op) {
    case SSA_CALL: return MEMORY_PROPERTY | MEMORY_CELL | MEMORY_GLOBAL;
    case OP_PUT_FIELD: case OP_PUT_ELEM: return MEMORY_PROPERTY | MEMORY_GLOBAL;
    case OP_PUT_UPVAL: return MEMORY_CELL;
    case OP_PUT_GLOBAL: return MEMORY_PROPERTY | MEMORY_GLOBAL;
    default: return 0;
  }
}

static bool same_load(const ssa_graph_t *g, const ssa_node_t *a, const ssa_node_t *b) {
  if (a->op != b->op || a->inputs.count != b->inputs.count) return false;
  for (unsigned i = 0; i < a->inputs.count; i++)
    if (ssa_resolve(g, a->inputs.data[i]) != ssa_resolve(g, b->inputs.data[i])) return false;
  sv_func_t *af = g->frames[a->frame].func, *bf = g->frames[b->frame].func;
  if (a->op == SSA_SHAPE_GUARD || a->op == SSA_LOAD_FIELD || a->op == SSA_ARRAY_TRY_GUARD)
    return a->imm.u64 == b->imm.u64;
  if (a->op == OP_GET_UPVAL)
    return ssa_resolve(g, g->frames[a->frame].closure) == ssa_resolve(g, g->frames[b->frame].closure) &&
        sv_get_u16(af->code + a->offset + 1) == sv_get_u16(bf->code + b->offset + 1);
  if (a->op == OP_GET_FIELD || a->op == OP_GET_FIELD_OPT ||
      a->op == OP_GET_GLOBAL || a->op == OP_GET_GLOBAL_UNDEF)
    return af->atoms[sv_get_u32(af->code + a->offset + 1)].str ==
           bf->atoms[sv_get_u32(bf->code + b->offset + 1)].str;
  return true;
}

// An own-data IC supplies a compile-time descriptor, never a mutable slot
// number read later from the IC. The lowerer retains this exact shape and
// checks its invalidation word. That guard is an explicit SSA value shared by
// field loads until a possibly aliasing effect invalidates it.
static bool specialize_shapes(ssa_graph_t *g) {
  unsigned limit = g->node_count;
  for (ssa_id_t id = 1; id < limit; id++) {
    ssa_node_t source = g->nodes[id];
    if (source.replacement || !g->blocks[source.block].reachable || source.op != OP_GET_FIELD) continue;
    sv_func_t *f = g->frames[source.frame].func;
    unsigned slot = sv_get_u16(f->code + source.offset + 5);
    if (!f->ic_slots || slot >= f->ic_count) continue;
    sv_ic_entry_t *ic = &f->ic_slots[slot];
    if (ic->epoch != ant_ic_epoch_counter || ic->get_kind != SV_GF_IC_OWN ||
        !ic->cached_shape || !sv_gf_ic_active(ic->cached_aux)) continue;
    sv_atom_t *atom = &f->atoms[sv_get_u32(f->code + source.offset + 1)];
    const uint32_t *guard = ant_shape_jit_guard(ic->cached_shape);
    const ant_shape_prop_t *property = ant_shape_prop_at(ic->cached_shape, ic->cached_index);
    if (!guard || *guard || !atom->len || (atom->str[0] >= '0' && atom->str[0] <= '9') ||
        !property || property->type != ANT_SHAPE_KEY_STRING || property->key.interned != atom->str ||
        property->has_getter || property->has_setter) continue;
    ssa_id_t dependency = ssa_node_add(g, source.block, SSA_SHAPE_GUARD, source.inputs.data, 1);
    if (!dependency) return false;
    g->nodes[dependency].snapshot = source.snapshot;
    g->nodes[dependency].offset = source.offset;
    g->nodes[dependency].frame = source.frame;
    g->nodes[dependency].imm.u64 = (uintptr_t)ic->cached_shape;
    ssa_ids_t *list = &g->blocks[source.block].nodes;
    unsigned at = 0;
    while (list->data[at] != id) at++;
    memmove(list->data + at + 1, list->data + at, (list->count - at - 1) * sizeof(*list->data));
    list->data[at] = dependency;
    g->nodes[id].op = SSA_LOAD_FIELD;
    g->nodes[id].imm.u64 = ic->cached_index;
    if (!ssa_ids_push(&g->nodes[id].inputs, dependency)) return false;
  }
  return true;
}

static bool array_facts(ssa_graph_t *g) {
  unsigned limit = g->node_count;
  for (ssa_id_t id = 1; id < limit; id++) {
    ssa_node_t source = g->nodes[id];
    if (source.replacement || !g->blocks[source.block].reachable || (source.flags & SSA_F_HOISTED) ||
        (source.op != OP_GET_ELEM && source.op != OP_PUT_ELEM)) continue;
    ssa_id_t data = ssa_node_add(g, source.block, SSA_ARRAY_TRY_GUARD, source.inputs.data, 1);
    if (!data) return false;
    g->nodes[data].imm.u64 = source.op == OP_PUT_ELEM;
    ssa_id_t length_inputs[] = {source.inputs.data[0], data};
    ssa_id_t length = ssa_node_add(g, source.block, SSA_ARRAY_TRY_LENGTH, length_inputs, 2);
    if (!length) return false;
    ssa_ids_t *list = &g->blocks[source.block].nodes;
    unsigned at = 0;
    while (list->data[at] != id) at++;
    memmove(list->data + at + 2, list->data + at, (list->count - at - 2) * sizeof(ssa_id_t));
    list->data[at] = data; list->data[at + 1] = length;
    if (!ssa_ids_push(&g->nodes[id].inputs, data) || !ssa_ids_push(&g->nodes[id].inputs, length)) return false;
    g->nodes[id].flags |= SSA_F_ARRAY_FACT;
  }
  return true;
}

static void transfer(const ssa_graph_t *g, uint32_t block, uint64_t *set,
                     const ssa_id_t *loads, const uint32_t *indices, unsigned count) {
  const ssa_ids_t *nodes = &g->blocks[block].nodes;
  for (unsigned j = 0; j < nodes->count; j++) {
    ssa_id_t id = nodes->data[j];
    if (g->nodes[id].replacement) continue;
    unsigned kill = invalidates(g->nodes[id].op);
    if (kill) for (unsigned i = 0; i < count; i++)
      if (load_kind(g->nodes[loads[i]].op) & kill) set[i / 64] &= ~(UINT64_C(1) << (i % 64));
    if (indices[id]) {
      unsigned i = indices[id] - 1;
      set[i / 64] |= UINT64_C(1) << (i % 64);
    }
  }
}

// Must-availability is an intersection at joins. Unknown calls kill every
// memory domain; stores kill possibly aliased domains. This works across
// imported call bodies and loop backedges without pretending they are pure.
static bool eliminate_loads(ssa_graph_t *g) {
  ssa_id_t *loads = malloc(g->node_count * sizeof(*loads));
  uint32_t *indices = calloc(g->node_count, sizeof(*indices));
  if (!loads || !indices) { free(loads); free(indices); return false; }
  unsigned count = 0;
  for (ssa_id_t id = 1; id < g->node_count; id++)
    if (!g->nodes[id].replacement && g->blocks[g->nodes[id].block].reachable && load_kind(g->nodes[id].op)) {
      loads[count] = id; indices[id] = ++count;
    }
  if (!count) { free(loads); free(indices); return true; }
  unsigned words = (count + 63) / 64;
  uint64_t *out = malloc((size_t)g->block_count * words * sizeof(*out));
  uint64_t *state = malloc(words * sizeof(*state));
  if (!out || !state) { free(loads); free(indices); free(out); free(state); return false; }
  memset(out, 255, (size_t)g->block_count * words * sizeof(*out));
  bool changed;
  do {
    changed = false;
    for (uint32_t bi = 1; bi < g->block_count; bi++) {
      const ssa_block_t *b = &g->blocks[bi];
      if (!b->reachable) continue;
      memset(state, bi == g->entry ? 0 : 255, words * sizeof(*state));
      for (unsigned j = 0; j < b->predecessors.count; j++) {
        uint32_t pred = b->predecessors.data[j];
        if (!g->blocks[pred].reachable) continue;
        for (unsigned w = 0; w < words; w++) state[w] &= out[(size_t)pred * words + w];
      }
      transfer(g, bi, state, loads, indices, count);
      uint64_t *row = out + (size_t)bi * words;
      if (memcmp(row, state, words * sizeof(*state))) {
        memcpy(row, state, words * sizeof(*state)); changed = true;
      }
    }
  } while (changed);
  for (uint32_t bi = 1; bi < g->block_count; bi++) {
    const ssa_block_t *b = &g->blocks[bi];
    if (!b->reachable) continue;
    memset(state, bi == g->entry ? 0 : 255, words * sizeof(*state));
    for (unsigned j = 0; j < b->predecessors.count; j++) {
      uint32_t pred = b->predecessors.data[j];
      if (!g->blocks[pred].reachable) continue;
      for (unsigned w = 0; w < words; w++) state[w] &= out[(size_t)pred * words + w];
    }
    for (unsigned j = 0; j < b->nodes.count; j++) {
      ssa_id_t id = b->nodes.data[j];
      ssa_node_t *n = &g->nodes[id];
      if (n->replacement) continue;
      unsigned kill = invalidates(n->op);
      if (kill) for (unsigned i = 0; i < count; i++)
        if (load_kind(g->nodes[loads[i]].op) & kill) state[i / 64] &= ~(UINT64_C(1) << (i % 64));
      if (!indices[id]) continue;
      for (unsigned i = 0; i < count; i++) {
        ssa_id_t previous = ssa_resolve(g, loads[i]);
        if (previous == id || !(state[i / 64] & (UINT64_C(1) << (i % 64)))) continue;
        const ssa_node_t *p = &g->nodes[previous];
        if (!ssa_dominates(g, p->block, bi) || !same_load(g, p, n)) continue;
        if (p->block == bi) {
          bool earlier = false;
          for (unsigned k = 0; k < j; k++) earlier |= b->nodes.data[k] == previous;
          if (!earlier) continue;
        }
        n->replacement = previous; g->eliminated_loads++; break;
      }
      unsigned i = indices[id] - 1;
      state[i / 64] |= UINT64_C(1) << (i % 64);
    }
  }
  free(loads); free(indices); free(out); free(state);
  return true;
}

bool ssa_optimize(ssa_graph_t *g) {
  for (ssa_id_t id = 1; id < g->node_count; id++) {
    ssa_node_t *n = &g->nodes[id];
    if (n->replacement || n->op != OP_GET_ELEM) continue;
    uint8_t *feedback = sv_func_type_feedback(g->frames[n->frame].func);
    if (feedback && sv_tfb_specialization_ready(feedback[n->offset])) {
      n->flags |= SSA_F_CHECK_NUMBER;
      n->type = SSA_T_NUMBER;
    }
  }
  if (!simplify(g) || !specialize_shapes(g) || !simplify_values(g) || !eliminate_loads(g) ||
      !ssa_optimize_loops(g) || !array_facts(g) || !eliminate_loads(g) || !representations(g)) {
    g->failure = "optimizer allocation"; return false;
  }
  for (ssa_id_t id = 1; id < g->node_count; id++) {
    ssa_node_t *n = &g->nodes[id];
    if (n->replacement || n->op != SSA_GUARD) continue;
    const ssa_node_t *input = &g->nodes[ssa_resolve(g, n->inputs.data[0])];
    if ((input->op == SSA_CONSTANT && input->imm.value != n->imm.value) ||
        (n->imm.value == SV_TDZ && !(input->type & SSA_T_OTHER))) {
      n->flags |= SSA_F_DEAD;
      g->eliminated_guards++;
    }
  }
  return ssa_graph_verify(g, NULL);
}
