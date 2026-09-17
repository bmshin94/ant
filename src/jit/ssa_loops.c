#include "ssa.h"
#include "silver/feedback.h"
#include <stdlib.h>
#include <string.h>

static bool member(const uint8_t *loop, uint32_t block) { return loop[block] != 0; }

static bool array_feedback(const ssa_graph_t *g, const ssa_node_t *n) {
  sv_func_t *f = g->frames[n->frame].func;
  uint8_t *feedback = sv_func_type_feedback(f);
  return feedback && sv_tfb_specialization_ready(feedback[n->offset]);
}

static bool numeric_operands(const ssa_node_t *n) {
  switch (n->op) {
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD:
    case OP_ADD_NUM: case OP_SUB_NUM: case OP_MUL_NUM: case OP_DIV_NUM:
    case OP_LT: case OP_LE: case OP_GT: case OP_GE:
    case OP_INC: case OP_DEC: case OP_NEG: case OP_UPLUS:
    case OP_BAND: case OP_BOR: case OP_BXOR: case OP_SHL: case OP_SHR: case OP_USHR:
      return true;
    default: return false;
  }
}

static bool invariant(const ssa_graph_t *g, const uint8_t *loop, uint32_t header, ssa_id_t value) {
  value = ssa_resolve(g, value);
  const ssa_node_t *n = &g->nodes[value];
  if (!member(loop, n->block)) return true;
  if (n->op == SSA_CONSTANT) return true;
  if (n->op == OP_GET_UPVAL) {
    ssa_id_t closure = ssa_resolve(g, g->frames[n->frame].closure);
    return !member(loop, g->nodes[closure].block);
  }
  if (n->op != SSA_PHI || n->block != header) return false;
  const ssa_block_t *b = &g->blocks[header];
  for (unsigned i = 0; i < b->predecessors.count; i++)
    if (member(loop, b->predecessors.data[i]) && ssa_resolve(g, n->inputs.data[i]) != value) return false;
  return true;
}

static ssa_id_t incoming(const ssa_graph_t *g, uint32_t header, unsigned edge, ssa_id_t value) {
  value = ssa_resolve(g, value);
  const ssa_node_t *n = &g->nodes[value];
  return n->op == SSA_PHI && n->block == header ? ssa_resolve(g, n->inputs.data[edge]) : value;
}

// Each outside edge gets its own preheader and original interpreter state.
// This includes OSR: its imports run before these guards. A failed guard at a
// zero-trip loop resumes at the incoming edge instead of executing the body.
static uint32_t preheader(ssa_graph_t *g, uint32_t header, unsigned edge) {
  uint32_t pred = g->blocks[header].predecessors.data[edge];
  ssa_id_t term = g->blocks[pred].nodes.data[g->blocks[pred].nodes.count - 1];
  if (!g->nodes[term].snapshot) return 0;
  uint32_t bridge = ssa_block_add(g, g->nodes[term].offset, g->blocks[pred].frame);
  if (!bridge) return 0;
  g->blocks[bridge].reachable = true;
  unsigned occurrence = 0;
  for (unsigned i = 0; i < edge; i++) occurrence += g->blocks[header].predecessors.data[i] == pred;
  bool found = false;
  for (unsigned e = 0; e < g->blocks[pred].successors.count; e++) {
    if (g->blocks[pred].successors.data[e] != header) continue;
    if (occurrence) { occurrence--; continue; }
    g->blocks[pred].successors.data[e] = bridge;
    found = true;
    break;
  }
  if (!found || !ssa_ids_push(&g->blocks[bridge].predecessors, pred) ||
      !ssa_ids_push(&g->blocks[bridge].successors, header)) return 0;
  g->blocks[header].predecessors.data[edge] = bridge;
  ssa_id_t jump = ssa_node_add(g, bridge, SSA_JUMP, NULL, 0);
  if (!jump) return 0;
  g->nodes[jump].snapshot = g->nodes[term].snapshot;
  g->nodes[jump].offset = g->nodes[term].offset;
  return bridge;
}

static ssa_id_t before_jump(ssa_graph_t *g, uint32_t block, unsigned op,
                            const ssa_id_t *inputs, unsigned count) {
  ssa_id_t id = ssa_node_add(g, block, (uint16_t)op, inputs, count);
  if (!id) return 0;
  ssa_ids_t *list = &g->blocks[block].nodes;
  ssa_id_t term = list->data[list->count - 2];
  list->data[list->count - 2] = id;
  list->data[list->count - 1] = term;
  g->nodes[id].snapshot = g->nodes[term].snapshot;
  return id;
}

static ssa_id_t merge_hoisted(ssa_graph_t *g, uint32_t header, const uint8_t *loop,
                             const uint32_t *entries, unsigned op, ssa_id_t value, bool write) {
  unsigned count = g->blocks[header].predecessors.count;
  ssa_id_t *inputs = calloc(count, sizeof(*inputs));
  if (!inputs) return 0;
  ssa_id_t phi = ssa_node_add(g, header, SSA_PHI, inputs, count);
  free(inputs);
  if (!phi) return 0;
  g->nodes[phi].flags |= SSA_F_HOISTED;
  if (op == SSA_ARRAY_GUARD) { g->nodes[phi].type = SSA_T_RAW; g->nodes[phi].rep = SSA_PTR; }
  if (op == SSA_ARRAY_LENGTH) { g->nodes[phi].type = SSA_T_NUMBER; g->nodes[phi].rep = SSA_I32; }
  if (op == SSA_CHECK_NUMBER) { g->nodes[phi].type = SSA_T_NUMBER; g->nodes[phi].rep = SSA_F64; }
  for (unsigned i = 0; i < count; i++) {
    uint32_t pred = g->blocks[header].predecessors.data[i];
    if (member(loop, pred)) { g->nodes[phi].inputs.data[i] = phi; continue; }
    ssa_id_t input = incoming(g, header, i, value);
    ssa_id_t guard = before_jump(g, entries[i], op, &input, 1);
    if (!guard) return 0;
    g->nodes[guard].imm.u64 = write;
    g->nodes[guard].flags |= SSA_F_HOISTED;
    g->nodes[phi].inputs.data[i] = guard;
    g->hoisted_guards++;
  }
  return phi;
}

static bool hoist_capture(ssa_graph_t *g, uint32_t header, const uint8_t *loop,
                           const uint32_t *entries, ssa_id_t id) {
  ssa_node_t source = g->nodes[id];
  unsigned count = g->blocks[header].predecessors.count;
  ssa_id_t *empty = calloc(count, sizeof(*empty));
  if (!empty) return false;
  ssa_id_t phi = ssa_node_add(g, header, SSA_PHI, empty, count);
  free(empty);
  if (!phi) return false;
  for (unsigned i = 0; i < count; i++) {
    if (member(loop, g->blocks[header].predecessors.data[i])) {
      g->nodes[phi].inputs.data[i] = phi;
      continue;
    }
    ssa_id_t load = before_jump(g, entries[i], source.op, NULL, 0);
    if (!load) return false;
    g->nodes[load].frame = source.frame;
    g->nodes[load].offset = source.offset;
    g->nodes[load].bytecode = source.bytecode;
    g->nodes[load].imm = source.imm;
    g->nodes[load].type = source.type;
    g->nodes[load].flags |= SSA_F_HOISTED;
    g->nodes[phi].inputs.data[i] = load;
  }
  g->nodes[id].replacement = phi;
  return true;
}

static bool hoist_number(ssa_graph_t *g, uint32_t header, const uint8_t *loop,
                         const uint32_t *entries, ssa_id_t value, unsigned limit) {
  value = ssa_resolve(g, value);
  if (g->nodes[value].type != SSA_T_ANY || !invariant(g, loop, header, value)) return true;
  ssa_id_t checked = merge_hoisted(g, header, loop, entries, SSA_CHECK_NUMBER, value, false);
  if (!checked) return false;
  for (ssa_id_t id = 1; id < limit; id++) {
    ssa_node_t *n = &g->nodes[id];
    if (n->replacement || !member(loop, n->block) || n->op == SSA_PHI) continue;
    unsigned inputs = n->inputs.count;
    if (n->op == SSA_LOAD_FIELD || n->op == SSA_ARRAY_LENGTH) inputs = 1;
    if (n->op == SSA_LOOP_RANGE) inputs = 2;
    if ((n->flags & SSA_F_HOISTED) && n->op == OP_GET_ELEM) inputs = 2;
    if ((n->flags & SSA_F_HOISTED) && n->op == OP_PUT_ELEM) inputs = 3;
    for (unsigned i = 0; i < inputs; i++)
      if (ssa_resolve(g, n->inputs.data[i]) == value) n->inputs.data[i] = checked;
    if (n->snapshot) {
      ssa_ids_t *state = &g->snapshots[n->snapshot].values;
      for (unsigned i = 0; i < state->count; i++)
        if (ssa_resolve(g, state->data[i]) == value) state->data[i] = checked;
    }
  }
  return true;
}

static bool prove_bounds(ssa_graph_t *g, uint32_t header, const uint8_t *loop,
                         const uint32_t *entries, ssa_id_t object, ssa_id_t length,
                         unsigned node_limit) {
  const ssa_ids_t *list = &g->blocks[header].nodes;
  const ssa_node_t *branch = &g->nodes[list->data[list->count - 1]];
  if (branch->op != SSA_BRANCH) return true;
  bool inverse = branch->bytecode == OP_JMP_FALSE || branch->bytecode == OP_JMP_FALSE8;
  if (!inverse && branch->bytecode != OP_JMP_TRUE && branch->bytecode != OP_JMP_TRUE8) return true;
  uint32_t body = g->blocks[header].successors.data[inverse ? 1 : 0];
  uint32_t exit = g->blocks[header].successors.data[inverse ? 0 : 1];
  if (!member(loop, body) || member(loop, exit)) return true;
  const ssa_node_t *comparison = &g->nodes[ssa_resolve(g, branch->inputs.data[0])];
  if (comparison->op != OP_LT && comparison->op != OP_LE) return true;
  bool inclusive = comparison->op == OP_LE;
  ssa_id_t index = ssa_resolve(g, comparison->inputs.data[0]);
  ssa_id_t limit = ssa_resolve(g, comparison->inputs.data[1]);
  const ssa_node_t *phi = &g->nodes[index];
  if (phi->op != SSA_PHI || phi->block != header || !invariant(g, loop, header, limit)) return true;
  unsigned count = g->blocks[header].predecessors.count;
  for (unsigned p = 0; p < count; p++) {
    uint32_t pred = g->blocks[header].predecessors.data[p];
    if (!member(loop, pred)) continue;
    const ssa_node_t *update = &g->nodes[ssa_resolve(g, phi->inputs.data[p])];
    if (!ssa_dominates(g, body, update->block)) return true;
    if (update->op == OP_INC && ssa_resolve(g, update->inputs.data[0]) == index) continue;
    if (update->op != OP_ADD && update->op != OP_ADD_NUM) return true;
    ssa_id_t a = ssa_resolve(g, update->inputs.data[0]), b = ssa_resolve(g, update->inputs.data[1]);
    if (b == index) { ssa_id_t temp = a; a = b; b = temp; }
    if (a != index || g->nodes[b].op != SSA_CONSTANT || g->nodes[b].imm.value != tov(1)) return true;
  }
  bool useful = false;
  for (ssa_id_t id = 1; id < node_limit; id++) {
    const ssa_node_t *n = &g->nodes[id];
    if (!n->replacement && member(loop, n->block) && ssa_dominates(g, body, n->block) &&
        (n->op == OP_GET_ELEM || n->op == OP_PUT_ELEM) &&
        ssa_resolve(g, n->inputs.data[0]) == object && ssa_resolve(g, n->inputs.data[1]) == index) useful = true;
  }
  if (!useful) return true;
  for (unsigned p = 0; p < count; p++) {
    if (member(loop, g->blocks[header].predecessors.data[p])) continue;
    ssa_id_t inputs[] = {incoming(g, header, p, index), incoming(g, header, p, limit), incoming(g, header, p, length)};
    ssa_id_t guard = before_jump(g, entries[p], SSA_LOOP_RANGE, inputs, 3);
    if (!guard) return false;
    g->nodes[guard].imm.u64 = inclusive;
    g->hoisted_guards++;
  }
  g->nodes[index].flags |= SSA_F_NO_OVERFLOW;
  g->nodes[index].range_known = true;
  g->nodes[index].min = 0; g->nodes[index].max = UINT32_MAX;
  for (unsigned p = 0; p < count; p++) if (member(loop, g->blocks[header].predecessors.data[p])) {
    ssa_id_t update = ssa_resolve(g, g->nodes[index].inputs.data[p]);
    g->nodes[update].flags |= SSA_F_NO_OVERFLOW;
    g->nodes[update].range_known = true;
    g->nodes[update].min = 1; g->nodes[update].max = UINT32_MAX;
  }
  for (ssa_id_t id = 1; id < node_limit; id++) {
    ssa_node_t *n = &g->nodes[id];
    if (!n->replacement && member(loop, n->block) && ssa_dominates(g, body, n->block) &&
        (n->op == OP_GET_ELEM || n->op == OP_PUT_ELEM) &&
        ssa_resolve(g, n->inputs.data[0]) == object && ssa_resolve(g, n->inputs.data[1]) == index)
      n->flags |= SSA_F_NO_BOUNDS;
  }
  return true;
}

static bool optimize_loop(ssa_graph_t *g, uint32_t header, uint8_t *loop, uint32_t original_blocks) {
  bool writes_elements = false;
  for (uint32_t bi = 1; bi < original_blocks; bi++) if (member(loop, bi)) {
    const ssa_ids_t *nodes = &g->blocks[bi].nodes;
    for (unsigned j = 0; j < nodes->count; j++) {
      const ssa_node_t *n = &g->nodes[nodes->data[j]];
      if (n->replacement) continue;
      if (n->op == OP_PUT_ELEM) { writes_elements = true; continue; }
      if (n->effects != SSA_EFFECT_NONE) return true;
    }
  }
  unsigned pred_count = g->blocks[header].predecessors.count;
  uint32_t *entries = calloc(pred_count, sizeof(*entries));
  if (!entries) return false;
  // Validate entry snapshots before changing any edge.
  for (unsigned i = 0; i < pred_count; i++) {
    uint32_t pred = g->blocks[header].predecessors.data[i];
    if (member(loop, pred)) continue;
    const ssa_ids_t *nodes = &g->blocks[pred].nodes;
    if (!g->nodes[nodes->data[nodes->count - 1]].snapshot) { free(entries); return true; }
  }
  unsigned node_limit = g->node_count;
  // Only create preheaders when there is an invariant receiver to specialize.
  bool candidate = false;
  for (ssa_id_t id = 1; id < node_limit; id++) {
    const ssa_node_t *n = &g->nodes[id];
    if (n->replacement || !member(loop, n->block)) continue;
    if ((n->op == OP_GET_ELEM || n->op == OP_PUT_ELEM) &&
        n->inputs.count <= 3 && array_feedback(g, n) && invariant(g, loop, header, n->inputs.data[0])) candidate = true;
    if (numeric_operands(n)) for (unsigned i = 0; i < n->inputs.count; i++) {
      ssa_id_t input = ssa_resolve(g, n->inputs.data[i]);
      if (g->nodes[input].type == SSA_T_ANY && invariant(g, loop, header, input)) candidate = true;
    }
  }
  if (!candidate) { free(entries); return true; }
  // A store through an unguarded alias might grow an array. Keep this loop on
  // per-access guards unless every element store has an invariant receiver.
  if (writes_elements) for (ssa_id_t id = 1; id < node_limit; id++) {
    const ssa_node_t *n = &g->nodes[id];
    if (!n->replacement && member(loop, n->block) && n->op == OP_PUT_ELEM &&
        !invariant(g, loop, header, n->inputs.data[0])) { free(entries); return true; }
  }
  for (unsigned i = 0; i < pred_count; i++) {
    uint32_t pred = g->blocks[header].predecessors.data[i];
    if (!member(loop, pred) && !(entries[i] = preheader(g, header, i))) { free(entries); return false; }
  }
  // Captured cell values, rather than just cell pointers, may move when no
  // retained operation writes a cell or invokes user code. OSR reads them anew.
  for (ssa_id_t id = 1; id < node_limit; id++) {
    const ssa_node_t *n = &g->nodes[id];
    if (!n->replacement && member(loop, n->block) && n->op == OP_GET_UPVAL &&
        invariant(g, loop, header, id) && !hoist_capture(g, header, loop, entries, id)) {
      free(entries); return false;
    }
  }
  for (ssa_id_t id = 1; id < node_limit; id++) {
    const ssa_node_t *n = &g->nodes[id];
    if (n->replacement || !member(loop, n->block) || !numeric_operands(n)) continue;
    unsigned count = n->inputs.count;
    for (unsigned i = 0; i < count; i++) {
      ssa_id_t input = g->nodes[id].inputs.data[i];
      if (!hoist_number(g, header, loop, entries, input, node_limit)) { free(entries); return false; }
    }
  }
  for (ssa_id_t id = 1; id < node_limit; id++) {
    ssa_node_t *n = &g->nodes[id];
    if (n->replacement || !member(loop, n->block) ||
        (n->op != OP_GET_ELEM && n->op != OP_PUT_ELEM) || (n->flags & SSA_F_HOISTED)) continue;
    ssa_id_t object = ssa_resolve(g, n->inputs.data[0]);
    if (!array_feedback(g, n) || !invariant(g, loop, header, object)) continue;
    ssa_id_t data = merge_hoisted(g, header, loop, entries, SSA_ARRAY_GUARD, object, writes_elements);
    if (!data) { free(entries); return false; }
    // The length is read at the same guarded entry; no operation retained in
    // this loop can replace backing storage or change the array length.
    ssa_id_t length = merge_hoisted(g, header, loop, entries, SSA_ARRAY_LENGTH, object, false);
    if (!length) { free(entries); return false; }
    for (unsigned edge = 0; edge < pred_count; edge++) if (entries[edge]) {
      ssa_id_t load = g->nodes[length].inputs.data[edge];
      if (!ssa_ids_push(&g->nodes[load].inputs, g->nodes[data].inputs.data[edge])) { free(entries); return false; }
    }
    for (ssa_id_t use = 1; use < node_limit; use++) {
      ssa_node_t *load = &g->nodes[use];
      if (!load->replacement && member(loop, load->block) && load->op == OP_GET_LENGTH &&
          ssa_resolve(g, load->inputs.data[0]) == object) load->replacement = length;
    }
    for (ssa_id_t use = id; use < node_limit; use++) {
      ssa_node_t *access = &g->nodes[use];
      if (access->replacement || !member(loop, access->block) ||
          (access->op != OP_GET_ELEM && access->op != OP_PUT_ELEM) || (access->flags & SSA_F_HOISTED) ||
          ssa_resolve(g, access->inputs.data[0]) != object) continue;
      if (!ssa_ids_push(&access->inputs, data) || !ssa_ids_push(&access->inputs, length)) { free(entries); return false; }
      access->flags |= SSA_F_HOISTED;
    }
    if (!prove_bounds(g, header, loop, entries, object, length, node_limit)) { free(entries); return false; }
  }
  free(entries);
  return true;
}

bool ssa_optimize_loops(ssa_graph_t *g) {
  uint32_t original_blocks = g->block_count;
  uint8_t *loop = calloc(2048, 1);
  ssa_ids_t pending = {0};
  if (!loop) return false;
  // Innermost/later bytecode headers first. Outer loops with already-hoisted
  // accesses can still reuse the result; they do not create duplicate guards.
  for (uint32_t header = original_blocks; --header > 0;) {
    memset(loop, 0, 2048);
    pending.count = 0;
    loop[header] = 1;
    for (unsigned p = 0; p < g->blocks[header].predecessors.count; p++) {
      uint32_t pred = g->blocks[header].predecessors.data[p];
      if (ssa_dominates(g, header, pred) && pred != header) {
        loop[pred] = 1;
        if (!ssa_ids_push(&pending, pred)) goto fail;
      }
    }
    if (!pending.count) continue;
    for (unsigned i = 0; i < pending.count; i++) {
      uint32_t bi = pending.data[i];
      for (unsigned p = 0; p < g->blocks[bi].predecessors.count; p++) {
        uint32_t pred = g->blocks[bi].predecessors.data[p];
        if (loop[pred]) continue;
        loop[pred] = 1;
        if (!ssa_ids_push(&pending, pred)) goto fail;
      }
    }
    if (!optimize_loop(g, header, loop, original_blocks) || !ssa_compute_dominators(g)) goto fail;
  }
  free(loop); ssa_ids_free(&pending);
  return ssa_graph_verify(g, NULL);
fail:
  g->failure = "loop optimization allocation or budget";
  free(loop); ssa_ids_free(&pending);
  return false;
}
