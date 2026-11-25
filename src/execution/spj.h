// Relational operators. Rows retain duplicates (SQL bag semantics).
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "common/status.h"
#include "record/value.h"

namespace minidb::spj {
using Row = std::vector<Value>;
using Rows = std::vector<Row>;
struct EquiKey {
  size_t left, right;
};
struct Stats {
  uint64_t candidates = 0;
  uint64_t probes = 0;
};
using Emit = std::function<bool(const Row&, const Row&)>;
using Accept = std::function<bool(const Row&, const Row&)>;
// All algorithms emit the full duplicate cross product for each equal group.
void NestedLoop(const Rows& left, const Rows& right, const Emit& emit,
                Stats* stats);
void SortMerge(const Rows& left, const Rows& right,
               const std::vector<EquiKey>& keys, const Emit& emit,
               Stats* stats);
void HashJoin(const Rows& left, const Rows& right,
              const std::vector<EquiKey>& keys, const Emit& emit, Stats* stats);
size_t RowBytes(const Row& row);
}  // namespace minidb::spj
