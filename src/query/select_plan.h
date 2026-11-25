// Static plan produced by SelectBinder and consumed by the executor.
// Plan nodes are immutable during execution; per-query cursors live in
// PlanState objects (see execution/exec_state.h).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "db/database.h"

namespace minidb {

struct RelationBinding {
  TableInfo* table = nullptr;
  std::string alias;
};

// One column of the flattened FROM space. `local` is the schema ordinal; a
// synthetic rid pseudo column uses local == schema.num_columns().
struct BoundColumn {
  size_t relation = 0;
  size_t local = 0;
  Column column;
  bool rid = false;
};

struct BoundPredicate {
  int left = -1;  // flat column id, -1 for a literal
  Value left_literal;
  int right = -1;
  Value right_literal;
  CompareOp op = CompareOp::kEq;
  size_t first = 0;  // smallest relation ordinal referenced
  size_t last = 0;   // largest relation ordinal referenced
  bool IsJoin() const { return first != last; }
};

// Equi-join key: cell positions in the left and right child layouts.
struct KeyRef {
  uint32_t left_cell = 0;
  uint32_t right_cell = 0;
};

// A residual join condition with operands resolved to child cell positions.
struct EvalOperand {
  int cell = -1;  // -1 => literal
  bool left_side = true;  // cell belongs to the left child slot
  Value literal;
};
struct ResidualPredicate {
  EvalOperand left, right;
  CompareOp op = CompareOp::kEq;
};

enum class NodeTag {
  kSeqScan,
  kIndexScan,
  kRidLookup,
  kNestLoopJoin,
  kHashJoin,
  kMergeJoin,
  kIndexJoin,
  kProjection,
};

struct PlanNode {
  NodeTag tag = NodeTag::kSeqScan;
  std::vector<int> cells;  // flat ids materialized in this node's output tuple
  std::string plan_line;

  // ---- scan ----
  size_t relation = 0;                  // FROM ordinal for scan / inner relation
  std::vector<BoundPredicate> filters;  // table-local predicates
  std::vector<int> filter_columns;      // flat ids referenced by filters
  BPlusTree* index = nullptr;
  CompareOp op = CompareOp::kEq;
  Value bound;
  bool full_index = false;
  RID rid;
  bool has_rid = false;

  // ---- join ----
  std::unique_ptr<PlanNode> left;
  std::unique_ptr<PlanNode> right;  // null for IndexJoin
  std::vector<KeyRef> keys;
  std::vector<ResidualPredicate> residual;
  // (source cell, destination cell) pairs used to assemble the joined tuple.
  std::vector<std::pair<uint32_t, uint32_t>> left_sel, right_sel;
  size_t inner_relation = 0;
  size_t indexed_column = 0;
  std::vector<int> inner_cells;  // IndexJoin: materialized inner columns
  int probe_cell = -1;           // IndexJoin: left cell holding the probe key
  std::string algorithm;

  // ---- projection ----
  struct OutputColumn {
    uint32_t relation = 0;
    uint32_t local = 0;
    bool rid = false;
  };
  std::vector<OutputColumn> output_columns;
};

struct SelectPlan {
  std::vector<RelationBinding> relations;
  std::vector<BoundColumn> columns;
  std::vector<size_t> projection;  // flat ids, user order and duplicates kept
  std::vector<QueryColumn> output;
  std::unique_ptr<PlanNode> root;
  std::vector<std::string> plan_lines;
  bool explain = false;
  bool detailed = false;
  int64_t limit = -1;
  int64_t scan_limit = -1;  // pushed down only for single-relation queries
  std::string single_index;
};

}  // namespace minidb
