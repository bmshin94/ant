#ifndef ANT_JIT_SSA_H
#define ANT_JIT_SSA_H

#include "silver/engine.h"
#include "silver/opcode.h"
#include <stdint.h>
#include <stdio.h>

// Zero is not a value. IDs survive vector relocation and graph rewriting.
typedef uint32_t ssa_id_t;
enum {
  SSA_PHI = OP__COUNT,
  SSA_PARAMETER, SSA_THIS, SSA_CLOSURE, SSA_NEW_TARGET, SSA_SUPER,
  SSA_CONSTANT, SSA_BRANCH, SSA_JUMP, SSA_RETURN,
  SSA_TARGET_TEST, SSA_RESOLVE_THIS, SSA_CALL, SSA_GUARD,
  SSA_ENTRY, SSA_OSR_VALUE,
  SSA_ROOT_CLOSURE,
  SSA_ARRAY_GUARD, SSA_ARRAY_LENGTH,
  SSA_LOOP_RANGE,
  SSA_DEOPT,
  SSA_SHAPE_GUARD, SSA_LOAD_FIELD,
  SSA_CHECK_NUMBER,
  SSA_ARRAY_TRY_GUARD, SSA_ARRAY_TRY_LENGTH,
  SSA_OP_COUNT
};
typedef enum { SSA_BOXED, SSA_BOOL, SSA_I32, SSA_I53, SSA_F64, SSA_PTR } ssa_rep_t;
enum {
  SSA_T_NUMBER = 1, SSA_T_BOOL = 2, SSA_T_UNDEFINED = 4,
  SSA_T_NULL = 8, SSA_T_STRING = 16, SSA_T_OBJECT = 32,
  SSA_T_FUNCTION = 64, SSA_T_OTHER = 128, SSA_T_ANY = 255,
  SSA_T_RAW = 256 // Internal metadata pointers are never JavaScript values.
};
enum {
  SSA_EFFECT_NONE = 0,
  SSA_EFFECT_FIELD = 1, SSA_EFFECT_ELEMENT = 2, SSA_EFFECT_CELL = 4,
  SSA_EFFECT_WORLD = 255
};
enum {
  SSA_F_DEAD = 1, SSA_F_CHECK_NUMBER = 2, SSA_F_HOISTED = 4,
  SSA_F_NO_BOUNDS = 8, SSA_F_NO_OVERFLOW = 16, SSA_F_ROOT = 32,
  SSA_F_ARRAY_FACT = 64
};

typedef struct {
  ssa_id_t *data;
  uint32_t count, capacity;
} ssa_ids_t;

typedef struct {
  uint16_t op, bytecode;
  uint16_t type, flags;
  uint8_t rep, effects;
  uint32_t block, frame, offset;
  uint32_t snapshot;
  ssa_id_t replacement;
  ssa_ids_t inputs;
  union { ant_value_t value; uint64_t u64; sv_func_t *target; } imm;
  int64_t min, max;
  bool range_known;
} ssa_node_t;

typedef struct {
  uint32_t frame, offset;
  uint32_t parent; // Caller continuation; zero for the outermost frame.
  uint32_t call_offset;
  bool return_child;
  uint16_t params, locals, stack;
  ssa_ids_t values; // Current params, locals, then operand stack.
} ssa_snapshot_t;

typedef struct {
  sv_func_t *func;
  uint32_t parent, continuation;
  ssa_id_t closure, receiver, new_target, super;
  ssa_ids_t arguments; // Original arguments, separate from mutable parameters.
  uint32_t entry;
} ssa_frame_t;

typedef struct {
  uint32_t frame, offset, end;
  int32_t stack_depth;
  ssa_ids_t predecessors, successors, nodes, phis;
  uint32_t idom, loop_header, loop_depth;
  bool reachable, osr_entry, osr_stub;
} ssa_block_t;

typedef struct {
  ssa_node_t *nodes;
  ssa_block_t *blocks;
  ssa_snapshot_t *snapshots;
  ssa_frame_t *frames;
  uint32_t node_count, node_capacity, block_count, block_capacity;
  uint32_t snapshot_count, snapshot_capacity, frame_count, frame_capacity;
  uint32_t entry, max_nodes, max_frames;
  uint32_t eliminated_loads, eliminated_guards, inlined_calls, hoisted_guards;
  uint32_t inline_cost;
  const char *failure;
} ssa_graph_t;

bool ssa_ids_push(ssa_ids_t *ids, ssa_id_t id);
void ssa_ids_free(ssa_ids_t *ids);
ssa_id_t ssa_node_add(ssa_graph_t *g, uint32_t block, uint16_t op,
                    const ssa_id_t *inputs, uint32_t count);
ssa_id_t ssa_resolve(const ssa_graph_t *g, ssa_id_t id);
uint32_t ssa_snapshot_add(ssa_graph_t *g, uint32_t frame, uint32_t offset,
                         const ssa_id_t *values, uint32_t count, int stack);
bool ssa_graph_build(ssa_graph_t *g, sv_func_t *func);
bool ssa_graph_build_mode(ssa_graph_t *g, sv_func_t *func, bool osr);
uint32_t ssa_block_add(ssa_graph_t *g, uint32_t offset, uint32_t frame);
bool ssa_edge_add(ssa_graph_t *g, uint32_t from, uint32_t to);
uint32_t ssa_frame_add(ssa_graph_t *g, sv_func_t *func, uint32_t parent);
bool ssa_inline_calls(ssa_graph_t *g);
bool ssa_lowerable(const ssa_graph_t *g);
bool ssa_optimize(ssa_graph_t *g);
bool ssa_optimize_loops(ssa_graph_t *g);
bool ssa_graph_verify(const ssa_graph_t *g, FILE *diagnostic);
bool ssa_compute_dominators(ssa_graph_t *g);
bool ssa_dominates(const ssa_graph_t *g, uint32_t definition, uint32_t use);
void ssa_graph_destroy(ssa_graph_t *g);
void ssa_graph_dump(const ssa_graph_t *g, FILE *out);
void ssa_simplify_phis(ssa_graph_t *g);

#endif
