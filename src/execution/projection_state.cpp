#include "execution/projection_state.h"

#include "catalog/catalog.h"
#include "common/util.h"

namespace minidb {

Status ProjectionState::Begin() {
  InitLayout(plan->cells);
  columns = plan->output_columns;
  fetches.clear();
  for (size_t position = 0; position < columns.size(); ++position) {
    const auto& column = columns[position];
    if (column.rid) continue;
    size_t group = fetches.size();
    for (size_t i = 0; i < fetches.size(); ++i)
      if (fetches[i].relation == column.relation) {
        group = i;
        break;
      }
    if (group == fetches.size()) {
      Fetch fetch;
      fetch.relation = column.relation;
      fetches.push_back(std::move(fetch));
    }
    Fetch& fetch = fetches[group];
    size_t value_index = fetch.locals.size();
    for (size_t i = 0; i < fetch.locals.size(); ++i)
      if (fetch.locals[i] == column.local) {
        value_index = i;
        break;
      }
    if (value_index == fetch.locals.size()) fetch.locals.push_back(column.local);
    fetch.uses.push_back({value_index, position});
  }
  remaining = estate->plan->limit;
  return Status::Ok();
}

Status ProjectionState::Next(TupleSlot* out, bool* eof) {
  if (remaining == 0) {
    *eof = true;
    return Status::Ok();
  }
  TupleSlot input;
  bool input_done = false;
  Status status = left->Next(&input, &input_done);
  if (!status.ok()) return status;
  if (input_done) {
    *eof = true;
    return Status::Ok();
  }
  std::vector<Value> row(columns.size());
  for (const auto& fetch : fetches) {
    if (fetch.locals.empty()) continue;
    std::vector<Value> values;
    status = estate->materializer.Fetch(
        *estate->plan->relations[fetch.relation].table,
        input.rows[fetch.relation].rid, fetch.locals, &values);
    if (!status.ok()) return status;
    for (const auto& use : fetch.uses)
      row[use.position] = values[use.value_index];
  }
  for (size_t i = 0; i < columns.size(); ++i)
    if (columns[i].rid)
      row[i] = Value::String(FormatRid(input.rows[columns[i].relation].rid));
  if (remaining > 0) --remaining;
  out->rows.clear();
  out->cells = std::move(row);
  ++stats.rows;
  *eof = false;
  return Status::Ok();
}

Status ProjectionState::ReScan() {
  remaining = estate->plan->limit;
  return left->ReScan();
}

Status ProjectionState::End() { return Status::Ok(); }

}  // namespace minidb
