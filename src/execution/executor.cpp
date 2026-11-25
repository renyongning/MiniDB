#include "execution/executor.h"

#include <sstream>

#include "execution/join_state.h"
#include "execution/projection_state.h"
#include "execution/scan_state.h"

namespace minidb {

Status Executor::InitNode(const PlanNode* node, PlanState* parent,
                          PlanState** out) {
  std::unique_ptr<PlanState> state;
  switch (node->tag) {
    case NodeTag::kSeqScan:
      state = std::make_unique<SeqScanState>();
      break;
    case NodeTag::kIndexScan:
      state = std::make_unique<IndexScanState>();
      break;
    case NodeTag::kRidLookup:
      state = std::make_unique<RidLookupState>();
      break;
    case NodeTag::kNestLoopJoin:
      state = std::make_unique<NestLoopState>();
      break;
    case NodeTag::kHashJoin:
      state = std::make_unique<HashJoinState>();
      break;
    case NodeTag::kMergeJoin:
      state = std::make_unique<MergeJoinState>();
      break;
    case NodeTag::kIndexJoin:
      state = std::make_unique<IndexJoinState>();
      break;
    case NodeTag::kProjection:
      state = std::make_unique<ProjectionState>();
      break;
  }
  state->tag = node->tag;
  state->plan = node;
  state->parent = parent;
  state->estate = &estate_;
  estate_.nodes.push_back(std::move(state));
  PlanState* self = estate_.nodes.back().get();
  if (node->left != nullptr) {
    Status status = InitNode(node->left.get(), self, &self->left);
    if (!status.ok()) return status;
  }
  if (node->right != nullptr) {
    Status status = InitNode(node->right.get(), self, &self->right);
    if (!status.ok()) return status;
  }
  *out = self;
  return Status::Ok();
}

Status Executor::Start(PlanState* state) {
  if (state->left != nullptr) {
    Status status = Start(state->left);
    if (!status.ok()) return status;
  }
  if (state->right != nullptr) {
    Status status = Start(state->right);
    if (!status.ok()) return status;
  }
  return state->Begin();
}

Status Executor::EndAll() {
  Status first;
  for (auto it = estate_.nodes.rbegin(); it != estate_.nodes.rend(); ++it) {
    Status status = (*it)->End();
    if (!status.ok() && first.ok()) first = status;
  }
  return first;
}

void Executor::FillMessage(QueryResult* result) {
  estate_.totals = ExecStats();
  for (const auto& node : estate_.nodes) {
    estate_.totals.candidates += node->stats.candidates;
    estate_.totals.probes += node->stats.probes;
    estate_.totals.rows += node->stats.rows;
  }
  if (!plan_.detailed) {
    result->message = plan_.single_index;
    return;
  }
  std::ostringstream message;
  for (const auto& line : plan_.plan_lines) message << line << '\n';
  message << "SPJ rows=" << result->rows.size()
          << " candidates=" << estate_.totals.candidates
          << " probes=" << estate_.totals.probes;
  result->message = message.str();
}

Status Executor::Run(QueryResult* result) {
  result->columns = plan_.output;
  if (plan_.limit == 0) {
    FillMessage(result);
    return Status::Ok();
  }
  Status status = InitNode(plan_.root.get(), nullptr, &estate_.root);
  if (!status.ok()) return status;
  status = Start(estate_.root);
  if (status.ok()) {
    for (;;) {
      TupleSlot slot;
      bool eof = false;
      status = estate_.root->Next(&slot, &eof);
      if (!status.ok() || eof) break;
      ++estate_.rows_out;
      result->rows.push_back(std::move(slot.cells));
    }
  }
  const Status end = EndAll();
  if (!status.ok()) return status;
  if (!end.ok()) return end;
  FillMessage(result);
  return Status::Ok();
}

}  // namespace minidb
