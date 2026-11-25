// Scan operators: heap scan, index scan and direct RID lookup.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "execution/exec_state.h"
#include "index/bplus_tree.h"
#include "table/table_heap.h"

namespace minidb {

// Shared scan-side state: which columns to decode and how to test filters.
struct ScanState : PlanState {
  std::vector<BoundPredicate> filters;
  std::vector<int> filter_columns;
  std::vector<size_t> decode_local;  // unique non-rid schema ordinals
  std::vector<int> value_of_flat;    // flat id -> decoded value index

  void InitScan(const PlanNode& node);
  bool Match(const std::vector<Value>& values, RID rid) const;
  void FillSlot(RID rid, const std::vector<Value>& values, TupleSlot* out) const;
  Status DecodeRow(const TableInfo& table, RID rid, const Tuple* known,
                   std::vector<Value>* values) const;
};

struct SeqScanState : ScanState {
  TableInfo* table = nullptr;
  TableHeap::Iterator iterator;
  RID rid;
  int64_t remaining = -1;  // scan_limit pushdown

  Status Begin() override;
  Status Next(TupleSlot* out, bool* eof) override;
  Status ReScan() override;
  Status End() override;
};

struct RidLookupState : ScanState {
  TableInfo* table = nullptr;
  bool emitted = false;

  Status Begin() override;
  Status Next(TupleSlot* out, bool* eof) override;
  Status ReScan() override;
  Status End() override;
};

struct IndexScanState : ScanState {
  enum class Phase { kEntries, kNullHeap, kDone };
  enum class EmitResult { kEmitted, kSkipped };

  TableInfo* table = nullptr;
  BPlusTree* index = nullptr;
  CompareOp op = CompareOp::kEq;
  Value bound;
  bool full_index = false;
  std::vector<RID> rids;  // collected at Begin (B+ Scan is callback-based)
  size_t entry_pos = 0;
  Phase phase = Phase::kEntries;
  TableHeap::Iterator null_iter;
  size_t null_column = 0;
  int64_t remaining = -1;
  size_t entry_bytes = 0;

  Status Begin() override;
  Status Next(TupleSlot* out, bool* eof) override;
  Status ReScan() override;
  Status End() override;

 private:
  Status Emit(RID rid, const Tuple* known, TupleSlot* out,
              EmitResult* result);
};

}  // namespace minidb
