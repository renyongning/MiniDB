#include "execution/exec_state.h"

#include "catalog/catalog.h"
#include "common/util.h"
#include "record/tuple.h"

namespace minidb {

size_t RowBytes(const std::vector<Value>& row) {
  size_t bytes = sizeof(std::vector<Value>) + row.size() * sizeof(Value);
  for (const auto& value : row) bytes += value.string_value.size();
  return bytes;
}

size_t SlotBytes(const TupleSlot& slot) {
  size_t bytes =
      sizeof(TupleSlot) + slot.rows.size() * sizeof(RowRef) +
      slot.cells.size() * sizeof(Value);
  for (const auto& value : slot.cells) bytes += value.string_value.size();
  return bytes;
}

Status TupleMaterializer::Fetch(const TableInfo& table, RID rid,
                                const std::vector<size_t>& local_columns,
                                std::vector<Value>* out) {
  out->clear();
  if (local_columns.empty()) return Status::Ok();
  auto tuple = table.heap->Get(txn_, rid);
  if (!tuple.ok()) return tuple.status();
  auto decoded =
      Tuple::DecodeColumns(table.schema, tuple.value().data().data(),
                           tuple.value().size(), local_columns);
  if (!decoded.ok()) return decoded.status();
  *out = decoded.TakeValue();
  return Status::Ok();
}

Result<Value> PlanState::ColumnValue(const TupleSlot& s, int flat) const {
  const auto& column = estate->plan->columns[static_cast<size_t>(flat)];
  const int cell = cell_of_flat[static_cast<size_t>(flat)];
  if (cell >= 0) return s.cells[static_cast<size_t>(cell)];
  const auto& ref = s.rows[column.relation];
  if (column.rid) return Value::String(FormatRid(ref.rid));
  std::vector<Value> values;
  Status status = estate->materializer.Fetch(
      *estate->plan->relations[column.relation].table, ref.rid, {column.local},
      &values);
  if (!status.ok()) return status;
  return values[0];
}

Status PlanState::MaterializeBatch(const TupleSlot& s, size_t relation,
                                   const std::vector<size_t>& locals,
                                   std::vector<Value>* out) const {
  out->clear();
  if (locals.empty()) return Status::Ok();
  const auto& ref = s.rows[relation];
  return estate->materializer.Fetch(
      *estate->plan->relations[relation].table, ref.rid, locals, out);
}

bool MatchPredicate(const BoundPredicate& predicate,
                    const std::function<const Value&(int)>& get) {
  const auto& a = predicate.left < 0 ? predicate.left_literal
                                     : get(predicate.left);
  const auto& b = predicate.right < 0 ? predicate.right_literal
                                      : get(predicate.right);
  return !a.IsNull() && !b.IsNull() &&
         Satisfies(CompareValues(a, b), predicate.op);
}

}  // namespace minidb
