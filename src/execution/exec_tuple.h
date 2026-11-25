// Executor-level tuple: physical row positions plus the few columns the
// current plan node needs. Payload columns are resolved lazily by RID.
#pragma once

#include <cstddef>
#include <vector>

#include "common/types.h"
#include "record/value.h"

namespace minidb {

struct TableInfo;

struct RowRef {
  const TableInfo* table = nullptr;
  RID rid;
};

struct TupleSlot {
  std::vector<RowRef> rows;  // one per FROM relation, in FROM order
  std::vector<Value> cells;  // materialized columns, per the node's layout

  void Clear() {
    rows.clear();
    cells.clear();
  }
};

// Logically budgeted size of a materialized value row.
size_t RowBytes(const std::vector<Value>& row);
size_t SlotBytes(const TupleSlot& slot);

}  // namespace minidb
