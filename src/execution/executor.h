// Volcano executor driver: builds the PlanState tree from a SelectPlan and
// runs Begin / Next / End, filling QueryResult.
#pragma once

#include "common/status.h"
#include "execution/exec_state.h"
#include "query/select_plan.h"

namespace minidb {

class Executor {
 public:
  Executor(const SelectPlan& plan, Transaction* txn, size_t query_memory)
      : plan_(plan), estate_(&plan, txn, query_memory) {}

  Status Run(QueryResult* result);

 private:
  Status InitNode(const PlanNode* node, PlanState* parent, PlanState** out);
  Status Start(PlanState* state);
  Status EndAll();
  void FillMessage(QueryResult* result);

  const SelectPlan& plan_;
  ExecutorState estate_;
};

}  // namespace minidb
