// Projection resolves the payload columns by RID and applies LIMIT. It streams
// rows upward; the materialized result set lives in QueryResult, which is the
// query's output rather than operator-internal state.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "execution/exec_state.h"

namespace minidb {

struct ProjectionState : PlanState {
  struct Use {
    size_t value_index = 0;  // index into Fetch::locals / decoded values
    size_t position = 0;     // output column position
  };
  struct Fetch {
    size_t relation = 0;
    std::vector<size_t> locals;  // unique local columns
    std::vector<Use> uses;
  };

  std::vector<PlanNode::OutputColumn> columns;
  std::vector<Fetch> fetches;
  int64_t remaining = -1;

  Status Begin() override;
  Status Next(TupleSlot* out, bool* eof) override;
  Status ReScan() override;
  Status End() override;
};

}  // namespace minidb
