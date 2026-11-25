// Per-query executor state (PG EState/PlanState analogue).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "common/status.h"
#include "execution/exec_tuple.h"
#include "query/select_plan.h"
#include "txn/transaction.h"

namespace minidb {

class QueryBudget {
 public:
  explicit QueryBudget(size_t limit) : limit_(limit) {}

  Status Reserve(size_t bytes) {
    if (bytes > limit_ || used_ > limit_ - bytes)
      return Status(StatusCode::kNoSpace,
                    "SPJ query memory budget exceeded; increase PRAGMA "
                    "query_memory or narrow the query");
    used_ += bytes;
    return Status::Ok();
  }
  void Release(size_t bytes) { used_ = bytes > used_ ? 0 : used_ - bytes; }
  size_t used() const { return used_; }
  size_t limit() const { return limit_; }

 private:
  size_t limit_;
  size_t used_ = 0;
};

struct ExecStats {
  uint64_t candidates = 0;
  uint64_t probes = 0;
  uint64_t rows = 0;
};

// Fetches a base row and decodes only the requested schema ordinals.
class TupleMaterializer {
 public:
  explicit TupleMaterializer(Transaction* txn) : txn_(txn) {}
  Status Fetch(const TableInfo& table, RID rid,
               const std::vector<size_t>& local_columns,
               std::vector<Value>* out);

 private:
  Transaction* txn_;
};

struct PlanState;
struct ExecutorState;

struct ExecutorState {
  ExecutorState(const SelectPlan* select_plan, Transaction* transaction,
                size_t query_memory)
      : plan(select_plan),
        txn(transaction),
        budget(query_memory),
        materializer(transaction) {}

  const SelectPlan* plan = nullptr;
  Transaction* txn = nullptr;
  QueryBudget budget;
  TupleMaterializer materializer;
  std::vector<std::unique_ptr<PlanState>> nodes;  // creation order; owns states
  PlanState* root = nullptr;
  ExecStats totals;
  uint64_t rows_out = 0;
};

struct PlanState {
  NodeTag tag = NodeTag::kSeqScan;
  const PlanNode* plan = nullptr;
  PlanState* left = nullptr;
  PlanState* right = nullptr;
  PlanState* parent = nullptr;
  ExecutorState* estate = nullptr;

  TupleSlot slot;               // reused output slot
  std::vector<int> layout;      // flat ids corresponding to slot.cells
  std::vector<int> cell_of_flat;  // flat id -> cell index (-1 if absent)
  ExecStats stats;

  virtual ~PlanState() = default;
  virtual Status Begin() = 0;
  virtual Status Next(TupleSlot* out, bool* eof) = 0;
  virtual Status ReScan() = 0;
  virtual Status End() = 0;

  void InitLayout(const std::vector<int>& cells) {
    layout = cells;
    cell_of_flat.assign(estate->plan->columns.size(), -1);
    for (size_t i = 0; i < layout.size(); ++i)
      cell_of_flat[static_cast<size_t>(layout[i])] = static_cast<int>(i);
  }
  bool HasCell(int flat) const {
    return cell_of_flat[static_cast<size_t>(flat)] >= 0;
  }
  const Value& Cell(const TupleSlot& s, int flat) const {
    return s.cells[static_cast<size_t>(cell_of_flat[static_cast<size_t>(flat)])];
  }
  // Deferred materialization: use a materialized cell when present, otherwise
  // fetch the base row by RID and decode just this column.
  Result<Value> ColumnValue(const TupleSlot& s, int flat) const;
  Status MaterializeBatch(const TupleSlot& s, size_t relation,
                          const std::vector<size_t>& locals,
                          std::vector<Value>* out) const;
};

bool MatchPredicate(const BoundPredicate& predicate,
                    const std::function<const Value&(int)>& get);

}  // namespace minidb
