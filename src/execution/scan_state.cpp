#include "execution/scan_state.h"

#include <algorithm>

#include "catalog/catalog.h"
#include "common/util.h"
#include "record/tuple.h"

namespace minidb {

void ScanState::InitScan(const PlanNode& node) {
  filters = node.filters;
  filter_columns = node.filter_columns;
  InitLayout(node.cells);

  std::vector<size_t> locals;
  const auto consider = [&](int flat) {
    const auto& column = estate->plan->columns[static_cast<size_t>(flat)];
    if (!column.rid) locals.push_back(column.local);
  };
  for (int flat : filter_columns) consider(flat);
  for (int flat : node.cells) consider(flat);
  std::sort(locals.begin(), locals.end());
  locals.erase(std::unique(locals.begin(), locals.end()), locals.end());
  decode_local = std::move(locals);

  value_of_flat.assign(estate->plan->columns.size(), -1);
  const auto index_of = [&](size_t local) {
    return static_cast<int>(
        std::lower_bound(decode_local.begin(), decode_local.end(), local) -
        decode_local.begin());
  };
  const auto mark = [&](int flat) {
    const auto& column = estate->plan->columns[static_cast<size_t>(flat)];
    if (!column.rid)
      value_of_flat[static_cast<size_t>(flat)] = index_of(column.local);
  };
  for (int flat : filter_columns) mark(flat);
  for (int flat : node.cells) mark(flat);
}

bool ScanState::Match(const std::vector<Value>& values, RID rid) const {
  Value rid_value;
  const auto get = [&](int flat) -> const Value& {
    const auto& column = estate->plan->columns[static_cast<size_t>(flat)];
    if (column.rid) {
      rid_value = Value::String(FormatRid(rid));
      return rid_value;
    }
    return values[static_cast<size_t>(
        value_of_flat[static_cast<size_t>(flat)])];
  };
  for (const auto& predicate : filters)
    if (!MatchPredicate(predicate, get)) return false;
  return true;
}

void ScanState::FillSlot(RID rid, const std::vector<Value>& values,
                         TupleSlot* out) const {
  out->rows.clear();
  out->rows.push_back(
      {estate->plan->relations[plan->relation].table, rid});
  out->cells.clear();
  out->cells.reserve(layout.size());
  for (int flat : layout) {
    const auto& column = estate->plan->columns[static_cast<size_t>(flat)];
    if (column.rid)
      out->cells.push_back(Value::String(FormatRid(rid)));
    else
      out->cells.push_back(
          values[static_cast<size_t>(value_of_flat[static_cast<size_t>(flat)])]);
  }
}

Status ScanState::DecodeRow(const TableInfo& table, RID rid,
                            const Tuple* known,
                            std::vector<Value>* values) const {
  values->clear();
  if (decode_local.empty()) return Status::Ok();
  const Tuple* tuple = known;
  Tuple fetched;
  if (tuple == nullptr) {
    auto got = table.heap->Get(estate->txn, rid);
    if (!got.ok()) return got.status();
    fetched = got.TakeValue();
    tuple = &fetched;
  }
  auto decoded = Tuple::DecodeColumns(table.schema, tuple->data().data(),
                                      tuple->size(), decode_local);
  if (!decoded.ok()) return decoded.status();
  *values = decoded.TakeValue();
  return Status::Ok();
}

Status SeqScanState::Begin() {
  InitScan(*plan);
  table = estate->plan->relations[plan->relation].table;
  iterator = table->heap->Scan(estate->txn);
  remaining = estate->plan->scan_limit;
  return Status::Ok();
}

Status SeqScanState::Next(TupleSlot* out, bool* eof) {
  for (;;) {
    if (remaining == 0) {
      *eof = true;
      return Status::Ok();
    }
    Tuple tuple;
    if (!iterator.Next(&rid, &tuple)) {
      if (!iterator.status().ok()) return iterator.status();
      *eof = true;
      return Status::Ok();
    }
    std::vector<Value> values;
    Status status = DecodeRow(*table, rid, &tuple, &values);
    if (!status.ok()) return status;
    if (!Match(values, rid)) continue;
    FillSlot(rid, values, out);
    if (remaining > 0) --remaining;
    ++stats.rows;
    *eof = false;
    return Status::Ok();
  }
}

Status SeqScanState::ReScan() {
  iterator = table->heap->Scan(estate->txn);
  remaining = estate->plan->scan_limit;
  return Status::Ok();
}

Status SeqScanState::End() {
  iterator = TableHeap::Iterator();
  return Status::Ok();
}

Status RidLookupState::Begin() {
  InitScan(*plan);
  table = estate->plan->relations[plan->relation].table;
  emitted = false;
  return Status::Ok();
}

Status RidLookupState::Next(TupleSlot* out, bool* eof) {
  if (emitted) {
    *eof = true;
    return Status::Ok();
  }
  emitted = true;
  auto tuple = table->heap->Get(estate->txn, plan->rid);
  if (!tuple.ok()) {
    if (tuple.status().code() == StatusCode::kNotFound) {
      *eof = true;
      return Status::Ok();
    }
    return tuple.status();
  }
  std::vector<Value> values;
  Status status = DecodeRow(*table, plan->rid, &tuple.value(), &values);
  if (!status.ok()) return status;
  if (!Match(values, plan->rid)) {
    *eof = true;
    return Status::Ok();
  }
  FillSlot(plan->rid, values, out);
  ++stats.rows;
  *eof = false;
  return Status::Ok();
}

Status RidLookupState::ReScan() {
  emitted = false;
  return Status::Ok();
}

Status RidLookupState::End() { return Status::Ok(); }

Status IndexScanState::Begin() {
  InitScan(*plan);
  table = estate->plan->relations[plan->relation].table;
  index = plan->index;
  op = plan->op;
  bound = plan->bound;
  full_index = plan->full_index;
  null_column = index->metadata().column;
  remaining = estate->plan->scan_limit;
  entry_pos = 0;
  phase = Phase::kEntries;
  rids.clear();
  entry_bytes = 0;

  if (remaining == 0) return Status::Ok();
  Status reserve_error;
  Status status = index->Scan(op, bound, [&](const IndexEntry& entry) {
    Status reserved = estate->budget.Reserve(sizeof(RID));
    if (!reserved.ok()) {
      reserve_error = reserved;
      return false;
    }
    entry_bytes += sizeof(RID);
    rids.push_back(entry.rid);
    return remaining < 0 || static_cast<int64_t>(rids.size()) < remaining;
  });
  if (!status.ok()) return status;
  if (!reserve_error.ok()) return reserve_error;
  return Status::Ok();
}

Status IndexScanState::Emit(RID rid, const Tuple* known, TupleSlot* out,
                            EmitResult* result) {
  std::vector<Value> values;
  Status status = DecodeRow(*table, rid, known, &values);
  if (!status.ok()) return status;
  if (!Match(values, rid)) {
    *result = EmitResult::kSkipped;
    return Status::Ok();
  }
  FillSlot(rid, values, out);
  *result = EmitResult::kEmitted;
  return Status::Ok();
}

Status IndexScanState::Next(TupleSlot* out, bool* eof) {
  for (;;) {
    if (remaining == 0) {
      *eof = true;
      return Status::Ok();
    }
    if (phase == Phase::kEntries) {
      if (entry_pos < rids.size()) {
        const RID rid = rids[entry_pos++];
        EmitResult result;
        Status status = Emit(rid, nullptr, out, &result);
        if (!status.ok()) return status;
        if (result == EmitResult::kSkipped) continue;
        if (remaining > 0) --remaining;
        ++stats.rows;
        *eof = false;
        return Status::Ok();
      }
      if (!full_index) {
        phase = Phase::kDone;
        *eof = true;
        return Status::Ok();
      }
      phase = Phase::kNullHeap;
      null_iter = table->heap->Scan(estate->txn);
    }
    if (phase == Phase::kNullHeap) {
      RID rid;
      Tuple tuple;
      if (!null_iter.Next(&rid, &tuple)) {
        phase = Phase::kDone;
        if (!null_iter.status().ok()) return null_iter.status();
        *eof = true;
        return Status::Ok();
      }
      auto key = Tuple::DecodeColumns(table->schema, tuple.data().data(),
                                      tuple.size(), {null_column});
      if (!key.ok()) return key.status();
      if (!key.value()[0].IsNull()) continue;
      EmitResult result;
      Status status = Emit(rid, &tuple, out, &result);
      if (!status.ok()) return status;
      if (result == EmitResult::kSkipped) continue;
      if (remaining > 0) --remaining;
      ++stats.rows;
      *eof = false;
      return Status::Ok();
    }
    *eof = true;
    return Status::Ok();
  }
}

Status IndexScanState::ReScan() {
  entry_pos = 0;
  phase = Phase::kEntries;
  remaining = estate->plan->scan_limit;
  null_iter = TableHeap::Iterator();
  return Status::Ok();
}

Status IndexScanState::End() {
  rids.clear();
  estate->budget.Release(entry_bytes);
  entry_bytes = 0;
  return Status::Ok();
}

}  // namespace minidb
