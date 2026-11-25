// Join operators: nested loop (with inner rescan), hash, sort-merge and
// index nested-loop.
#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "execution/exec_state.h"

namespace minidb {

class Tuple;

struct JoinState : PlanState {
  std::vector<KeyRef> keys;
  std::vector<ResidualPredicate> residual;
  std::vector<std::pair<uint32_t, uint32_t>> left_sel, right_sel;

  void InitJoin(const PlanNode& node);
  bool NullKey(const TupleSlot& slot, bool left_side) const;
  size_t HashKey(const TupleSlot& slot, bool left_side) const;
  int CompareKey(const TupleSlot& a, const TupleSlot& b, bool a_left,
                 bool b_left) const;
  bool ResidualMatch(const TupleSlot& left, const TupleSlot& right) const;
  void FormJoinedSlot(const TupleSlot& left, const TupleSlot& right,
                      TupleSlot* out) const;
};

struct NestLoopState : JoinState {
  TupleSlot outer;
  bool have_outer = false;
  bool inner_done = false;

  Status Begin() override;
  Status Next(TupleSlot* out, bool* eof) override;
  Status ReScan() override;
  Status End() override;
};

struct HashJoinState : JoinState {
  std::vector<TupleSlot> build_rows;
  std::unordered_map<size_t, std::vector<uint32_t>> buckets;
  bool built = false;
  TupleSlot outer;
  const std::vector<uint32_t>* bucket = nullptr;
  size_t bucket_pos = 0;
  size_t build_bytes = 0;
  bool finished = false;

  Status Begin() override;
  Status Next(TupleSlot* out, bool* eof) override;
  Status ReScan() override;
  Status End() override;

 private:
  Status Build();
};

struct MergeJoinState : JoinState {
  std::vector<TupleSlot> left_rows, right_rows;
  std::vector<uint32_t> left_order, right_order;
  bool sorted = false;
  size_t li = 0, ri = 0;   // merge positions
  size_t le = 0, re = 0;   // current equal-group ends
  size_t gx = 0, gy = 0;   // cross-product cursor inside the group
  bool in_group = false;
  size_t sort_bytes = 0;
  bool finished = false;

  Status Begin() override;
  Status Next(TupleSlot* out, bool* eof) override;
  Status ReScan() override;
  Status End() override;

 private:
  Status Sort();
  Status Drain(PlanState* child, bool left_side, std::vector<TupleSlot>* rows,
               std::vector<uint32_t>* order);
};

struct IndexJoinState : JoinState {
  TableInfo* inner = nullptr;
  BPlusTree* index = nullptr;
  size_t indexed_column = 0;
  CompareOp op = CompareOp::kEq;
  int probe_cell = -1;  // left cell holding the probe key; -1 => CROSS
  std::vector<BoundPredicate> filters;   // inner table-local predicates
  std::vector<int> filter_columns;
  std::vector<size_t> inner_decode_local;
  std::vector<int> inner_value_of_flat;  // flat id -> decoded value index
  std::vector<int> inner_cell_of_flat;   // flat id -> inner cell index
  std::vector<TupleSlot> null_rows;      // CROSS: rows with a NULL index key
  bool nulls_built = false;
  size_t null_bytes = 0;
  TupleSlot outer;
  std::vector<TupleSlot> pending;  // matches for the current outer row
  size_t pending_pos = 0;

  Status Begin() override;
  Status Next(TupleSlot* out, bool* eof) override;
  Status ReScan() override;
  Status End() override;

 private:
  Status MaterializeInner(RID rid, const Tuple* known, TupleSlot* out);
  bool MatchInner(const TupleSlot& row, RID rid) const;
  Status Probe(const TupleSlot& left, const Value& key, CompareOp probe_op);
  Status ProbeCross(const TupleSlot& left);
  Status BuildNullRows();
};

}  // namespace minidb
