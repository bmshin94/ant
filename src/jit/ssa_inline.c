#include "ssa.h"
#include "silver/feedback.h"
#include <stdlib.h>
#include <string.h>

static constexpr unsigned SSA_INLINE_COST_LIMIT = 8000;

// Deopt reconstruction, not bytecode length alone, dominates MIR compilation
// for deep imports. Charge each guarded operation for its entire frame chain.
// This permits many tiny callees without expanding a large arithmetic caller
// into tens of thousands of cold spill instructions.
static unsigned state_words(const ssa_graph_t *g, uint32_t state) {
  unsigned words = 0;
  for (; state; state = g->snapshots[state].parent)
    words += 4 + g->snapshots[state].values.count;
  return words;
}

static unsigned import_cost(const ssa_graph_t *g, unsigned parent_words) {
  unsigned cost = 0;
  for (ssa_id_t id = 1; id < g->node_count; id++) {
    const ssa_node_t *n = &g->nodes[id];
    if (n->replacement || !g->blocks[n->block].reachable) continue;
    cost += 8;
    if (n->snapshot && (n->op < OP__COUNT || n->op == SSA_GUARD))
      cost += 2 * (parent_words + state_words(g, n->snapshot));
  }
  return cost;
}

// Import whole CFGs, never bytecode snippets. The caller continuation describes
// an already-started call; a child deopt resumes that continuation with its
// result. The generic arm remains available for unobserved/bound targets.
static bool recursive(const ssa_graph_t *g, uint32_t frame, sv_func_t *target) {
  unsigned depth = 0;
  for (; frame; frame = g->frames[frame].parent) {
    if (g->frames[frame].func == target || ++depth >= 5) return true;
  }
  return false;
}

static ssa_id_t undefined_value(ssa_graph_t *g, uint32_t block) {
  ssa_id_t id = ssa_node_add(g, block, SSA_CONSTANT, NULL, 0);
  if (id) { g->nodes[id].imm.value = js_mkundef(); g->nodes[id].type = SSA_T_UNDEFINED; }
  return id;
}

static bool import_target(ssa_graph_t *g, ssa_graph_t *child, const ssa_node_t *call,
                          uint32_t dispatch, uint32_t continuation, uint32_t parent_state,
                          ssa_id_t result_phi, ssa_id_t undef, uint32_t *entry_out) {
  uint32_t frame = ssa_frame_add(g, child->frames[1].func, call->frame);
  if (!frame) return false;
  uint32_t *blocks = calloc(child->block_count, sizeof(*blocks));
  ssa_id_t *values = calloc(child->node_count, sizeof(*values));
  uint32_t *states = calloc(child->snapshot_count, sizeof(*states));
  if (!blocks || !values || !states) goto failed;
  for (uint32_t bi = 1; bi < child->block_count; bi++) {
    blocks[bi] = ssa_block_add(g, child->blocks[bi].offset, frame);
    if (!blocks[bi]) goto failed;
    g->blocks[blocks[bi]].reachable = child->blocks[bi].reachable;
    g->blocks[blocks[bi]].stack_depth = child->blocks[bi].stack_depth;
  }
  uint32_t entry = blocks[child->entry];
  *entry_out = entry;
  g->frames[frame].entry = entry;
  g->frames[frame].continuation = parent_state;
  g->frames[frame].closure = call->inputs.data[0];
  g->frames[frame].new_target = undef;
  g->frames[frame].super = undef;
  for (unsigned i = 2; i < call->inputs.count; i++)
    if (!ssa_ids_push(&g->frames[frame].arguments, call->inputs.data[i])) goto failed;
  ssa_id_t root = ssa_node_add(g, entry, SSA_ROOT_CLOSURE, call->inputs.data, 1);
  if (!root) goto failed;
  g->nodes[root].flags |= SSA_F_ROOT;
  ssa_id_t receiver_inputs[] = {call->inputs.data[0], call->inputs.data[1]};
  ssa_id_t receiver = ssa_node_add(g, entry, SSA_RESOLVE_THIS, receiver_inputs, 2);
  if (!receiver) goto failed;
  g->nodes[receiver].imm.target = child->frames[1].func;
  if (!child->frames[1].func->is_strict && !child->frames[1].func->is_arrow)
    g->nodes[receiver].type = SSA_T_OBJECT | SSA_T_FUNCTION;
  g->frames[frame].receiver = receiver;
  for (uint32_t bi = 1; bi < child->block_count; bi++) {
    const ssa_block_t *source = &child->blocks[bi];
    for (unsigned section = 0; section < 2; section++) {
      const ssa_ids_t *list = section ? &source->nodes : &source->phis;
      for (unsigned i = 0; i < list->count; i++) {
        ssa_id_t old = list->data[i];
        values[old] = ssa_node_add(g, blocks[bi], child->nodes[old].op, NULL, 0);
        if (!values[old]) goto failed;
      }
    }
    for (unsigned i = 0; i < source->successors.count; i++)
      if (!ssa_edge_add(g, blocks[bi], blocks[source->successors.data[i]])) goto failed;
  }
  // The order of predecessor lists must match source phi inputs. Creating
  // edges in block order need not preserve it after graph transformations.
  for (uint32_t bi = 1; bi < child->block_count; bi++) {
    ssa_ids_t *preds = &g->blocks[blocks[bi]].predecessors;
    for (unsigned i = 0; i < child->blocks[bi].predecessors.count; i++)
      preds->data[i] = blocks[child->blocks[bi].predecessors.data[i]];
  }
  for (ssa_id_t old = 1; old < child->node_count; old++) {
    const ssa_node_t *src = &child->nodes[old];
    ssa_node_t *dst = &g->nodes[values[old]];
    dst->bytecode = src->bytecode; dst->offset = src->offset;
    dst->type = src->type; dst->effects = src->effects; dst->imm = src->imm;
    dst->replacement = src->replacement ? values[src->replacement] : 0;
    for (unsigned j = 0; j < src->inputs.count; j++)
      if (!ssa_ids_push(&dst->inputs, values[src->inputs.data[j]])) goto failed;
    switch (src->op) {
      case SSA_PARAMETER:
        dst->replacement = src->imm.u64 + 2 < call->inputs.count ? call->inputs.data[src->imm.u64 + 2] : undef;
        break;
      case SSA_THIS: dst->replacement = receiver; break;
      case SSA_CLOSURE: dst->replacement = call->inputs.data[0]; break;
      case SSA_NEW_TARGET: case SSA_SUPER: dst->replacement = undef; break;
      default: break;
    }
  }
  for (uint32_t old = 1; old < child->snapshot_count; old++) {
    const ssa_snapshot_t *src = &child->snapshots[old];
    ssa_id_t mapped[512];
    if (src->values.count > 512) goto failed;
    for (unsigned i = 0; i < src->values.count; i++) mapped[i] = values[src->values.data[i]];
    states[old] = ssa_snapshot_add(g, frame, src->offset, mapped, src->values.count, src->stack);
    if (!states[old]) goto failed;
    g->snapshots[states[old]].parent = parent_state;
  }
  for (ssa_id_t old = 1; old < child->node_count; old++) {
    ssa_node_t *n = &g->nodes[values[old]];
    n->snapshot = states[child->nodes[old].snapshot];
    if (n->op != SSA_RETURN || !g->blocks[n->block].reachable) continue;
    ssa_id_t result = n->inputs.data[0];
    n->op = SSA_JUMP;
    n->inputs.count = 0;
    if (!ssa_edge_add(g, n->block, continuation) ||
        !ssa_ids_push(&g->nodes[result_phi].inputs, result)) goto failed;
  }
  if (!ssa_edge_add(g, dispatch, entry)) goto failed;
  free(blocks); free(values); free(states);
  return true;
failed:
  free(blocks); free(values); free(states);
  g->failure = "inline import allocation or budget";
  return false;
}

static bool inline_call(ssa_graph_t *g, ssa_id_t id) {
  ssa_node_t call = g->nodes[id];
  sv_func_t *targets[SV_CALL_FB_MAX_TARGETS];
  int count = sv_tfb_get_call_targets(g->frames[call.frame].func, (int)call.offset, targets, SV_CALL_FB_MAX_TARGETS);
  ssa_graph_t children[SV_CALL_FB_MAX_TARGETS] = {0};
  unsigned accepted = 0, budget = g->node_count;
  unsigned cost = g->inline_cost;
  for (int i = 0; i < count; i++) {
    if (recursive(g, call.frame, targets[i]) || targets[i]->code_len > 1024 ||
        g->frame_count + accepted >= g->max_frames) continue;
    ssa_graph_t candidate;
    if (!ssa_graph_build_mode(&candidate, targets[i], false) || !ssa_lowerable(&candidate) ||
        budget + candidate.node_count + 20 >= g->max_nodes) {
      ssa_graph_destroy(&candidate); continue;
    }
    unsigned added_cost = import_cost(&candidate, state_words(g, call.snapshot));
    if (cost + added_cost > SSA_INLINE_COST_LIMIT) { ssa_graph_destroy(&candidate); continue; }
    cost += added_cost;
    budget += candidate.node_count + 20;
    children[accepted++] = candidate;
  }
  if (!accepted) return true;
  if (count > 1 && accepted < (unsigned)count) {
    for (unsigned i = 0; i < accepted; i++) ssa_graph_destroy(&children[i]);
    return true;
  }
  g->inline_cost = cost;
  uint32_t block = call.block;
  unsigned position = 0;
  while (g->blocks[block].nodes.data[position] != id) position++;
  uint32_t cont = ssa_block_add(g, call.offset, call.frame);
  uint32_t fallback = ssa_block_add(g, call.offset, call.frame);
  if (!cont || !fallback) goto fail;
  g->blocks[cont].reachable = g->blocks[fallback].reachable = true;
  ssa_block_t *b = &g->blocks[block];
  for (unsigned i = position + 1; i < b->nodes.count; i++) {
    ssa_id_t moved = b->nodes.data[i];
    if (!ssa_ids_push(&g->blocks[cont].nodes, moved)) goto fail;
    g->nodes[moved].block = cont;
  }
  b->nodes.count = position;
  g->blocks[cont].successors = b->successors;
  b->successors = (ssa_ids_t){0};
  for (unsigned e = 0; e < g->blocks[cont].successors.count; e++) {
    ssa_ids_t *preds = &g->blocks[g->blocks[cont].successors.data[e]].predecessors;
    for (unsigned p = 0; p < preds->count; p++) if (preds->data[p] == block) preds->data[p] = cont;
  }
  ssa_id_t undef = undefined_value(g, block);
  ssa_id_t result = ssa_node_add(g, cont, SSA_PHI, NULL, 0);
  if (!undef || !result) goto fail;
  g->nodes[id].replacement = result;
  const ssa_snapshot_t before = g->snapshots[call.snapshot];
  bool tail = call.bytecode == OP_TAIL_CALL || call.bytecode == OP_TAIL_CALL_METHOD;
  unsigned pop = call.inputs.count - ((call.bytecode == OP_CALL || call.bytecode == OP_TAIL_CALL) ? 1 : 0);
  unsigned stack = before.stack - pop + (tail ? 0 : 1);
  ssa_id_t resume[512];
  unsigned total = before.params + before.locals + stack;
  if (total > 512) goto fail;
  memcpy(resume, before.values.data, (total - (tail ? 0 : 1)) * sizeof(*resume));
  if (!tail) resume[total - 1] = undef;
  uint32_t parent = ssa_snapshot_add(g, call.frame,
      tail ? call.offset : call.offset + sv_op_size[call.bytecode], resume, total, (int)stack);
  if (!parent) goto fail;
  g->snapshots[parent].parent = before.parent;
  g->snapshots[parent].call_offset = call.offset;
  g->snapshots[parent].return_child = tail;
  uint32_t dispatch = block;
  for (unsigned i = 0; i < accepted; i++) {
    uint32_t next = i + 1 == accepted ? fallback : ssa_block_add(g, call.offset, call.frame);
    if (!next) goto fail;
    g->blocks[next].reachable = true;
    ssa_id_t test = ssa_node_add(g, dispatch, SSA_TARGET_TEST, call.inputs.data, 1);
    if (!test) goto fail;
    g->nodes[test].imm.target = children[i].frames[1].func;
    g->nodes[test].type = SSA_T_BOOL;
    ssa_id_t branch = ssa_node_add(g, dispatch, SSA_BRANCH, &test, 1);
    if (!branch) goto fail;
    g->nodes[branch].bytecode = OP_JMP_TRUE;
    uint32_t child_entry;
    if (!import_target(g, &children[i], &call, dispatch, cont, parent, result, undef, &child_entry) ||
        !ssa_edge_add(g, dispatch, next)) goto fail;
    dispatch = next;
    g->inlined_calls++;
  }
  ssa_id_t slow = ssa_node_add(g, fallback, SSA_DEOPT, NULL, 0);
  if (!slow) goto fail;
  g->nodes[slow].snapshot = call.snapshot;
  g->nodes[slow].offset = call.offset;
  g->nodes[slow].bytecode = call.bytecode;
  for (unsigned i = 0; i < accepted; i++) ssa_graph_destroy(&children[i]);
  return true;
fail:
  for (unsigned i = 0; i < accepted; i++) ssa_graph_destroy(&children[i]);
  g->failure = "inline transform allocation or budget";
  return false;
}

bool ssa_inline_calls(ssa_graph_t *g) {
  g->inline_cost = import_cost(g, 0);
  for (ssa_id_t id = 1; id < g->node_count; id++) {
    if (g->nodes[id].op != SSA_CALL || g->nodes[id].replacement) continue;
    if (!inline_call(g, id)) return false;
  }
  ssa_simplify_phis(g);
  return ssa_compute_dominators(g) && ssa_graph_verify(g, NULL);
}
