#include "execution/join_state.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "catalog/catalog.h"
#include "common/util.h"
#include "record/tuple.h"

namespace minidb {
namespace {

Value Minimum(TypeId type) {
  if (type == TypeId::kChar || type == TypeId::kVarchar)
    return Value::String("");
  if (type == TypeId::kDate)
    return Value::Date(std::numeric_limits<int32_t>::min());
  return Value::Int64(std::numeric_limits<int64_t>::min());
}

}  // namespace

void JoinState::InitJoin(const PlanNode& node) {
  keys = node.keys;
  residual = node.residual;
  left_sel = node.left_sel;
  right_sel = node.right_sel;
  InitLayout(node.cells);
}

bool JoinState::NullKey(const TupleSlot& slot, bool left_side) const {
  for (const auto& key : keys) {
    const auto& value =
        slot.cells[left_side ? key.left_cell : key.right_cell];
    if (value.IsNull()) return true;
  }
  return false;
}

size_t JoinState::HashKey(const TupleSlot& slot, bool left_side) const {
  size_t hash = 0;
  for (const auto& key : keys) {
    const auto& value =
        slot.cells[left_side ? key.left_cell : key.right_cell];
    size_t h;
    if (value.kind == ValueKind::kString) {
      h = std::hash<std::string>{}(TrimRight(value.string_value));
    } else {
      long double number =
          value.kind == ValueKind::kDouble
              ? static_cast<long double>(value.double_value)
          : value.kind == ValueKind::kInt32 || value.kind == ValueKind::kDate
              ? static_cast<long double>(value.int32_value)
              : static_cast<long double>(value.int64_value);
      if (value.kind == ValueKind::kDecimal)
        number /= std::pow(10.0L, value.scale);
      h = std::hash<long double>{}(number);
    }
    hash ^= h + 0x9e3779b9 + (hash << 6) + (hash >> 2);
  }
  return hash;
}

int JoinState::CompareKey(const TupleSlot& a, const TupleSlot& b, bool a_left,
                          bool b_left) const {
  for (const auto& key : keys) {
    const int cmp = CompareValues(a.cells[a_left ? key.left_cell : key.right_cell],
                                  b.cells[b_left ? key.left_cell : key.right_cell]);
    if (cmp != 0) return cmp;
  }
  return 0;
}

bool JoinState::ResidualMatch(const TupleSlot& left,
                              const TupleSlot& right) const {
  for (const auto& predicate : residual) {
    const auto& a = predicate.left.cell < 0
                        ? predicate.left.literal
                        : (predicate.left.left_side
                               ? left.cells[static_cast<size_t>(
                                     predicate.left.cell)]
                               : right.cells[static_cast<size_t>(
                                     predicate.left.cell)]);
    const auto& b = predicate.right.cell < 0
                        ? predicate.right.literal
                        : (predicate.right.left_side
                               ? left.cells[static_cast<size_t>(
                                     predicate.right.cell)]
                               : right.cells[static_cast<size_t>(
                                     predicate.right.cell)]);
    if (a.IsNull() || b.IsNull()) return false;
    if (!Satisfies(CompareValues(a, b), predicate.op)) return false;
  }
  return true;
}

void JoinState::FormJoinedSlot(const TupleSlot& left, const TupleSlot& right,
                               TupleSlot* out) const {
  out->rows.clear();
  out->rows.reserve(left.rows.size() + right.rows.size());
  out->rows.insert(out->rows.end(), left.rows.begin(), left.rows.end());
  out->rows.insert(out->rows.end(), right.rows.begin(), right.rows.end());
  out->cells.assign(plan->cells.size(), Value());
  for (const auto& [src, dst] : left_sel) out->cells[dst] = left.cells[src];
  for (const auto& [src, dst] : right_sel) out->cells[dst] = right.cells[src];
}

Status NestLoopState::Begin() {
  InitJoin(*plan);
  have_outer = false;
  inner_done = false;
  outer.Clear();
  return Status::Ok();
}

Status NestLoopState::Next(TupleSlot* out, bool* eof) {
  for (;;) {
    if (!have_outer) {
      bool input_done = false;
      Status status = left->Next(&outer, &input_done);
      if (!status.ok()) return status;
      if (input_done) {
        *eof = true;
        return Status::Ok();
      }
      have_outer = true;
      inner_done = false;
    }
    if (inner_done) {
      Status status = right->ReScan();
      if (!status.ok()) return status;
      have_outer = false;
      continue;
    }
    TupleSlot inner;
    bool input_done = false;
    Status status = right->Next(&inner, &input_done);
    if (!status.ok()) return status;
    if (input_done) {
      inner_done = true;
      continue;
    }
    ++stats.candidates;
    if (!ResidualMatch(outer, inner)) continue;
    FormJoinedSlot(outer, inner, out);
    ++stats.rows;
    *eof = false;
    return Status::Ok();
  }
}

Status NestLoopState::ReScan() {
  Status status = left->ReScan();
  if (!status.ok()) return status;
  status = right->ReScan();
  if (!status.ok()) return status;
  have_outer = false;
  inner_done = false;
  outer.Clear();
  return Status::Ok();
}

Status NestLoopState::End() {
  outer.Clear();
  return Status::Ok();
}

Status HashJoinState::Begin() {
  InitJoin(*plan);
  built = false;
  finished = false;
  bucket = nullptr;
  bucket_pos = 0;
  outer.Clear();
  return Status::Ok();
}

Status HashJoinState::Build() {
  for (;;) {
    TupleSlot slot;
    bool eof = false;
    Status status = right->Next(&slot, &eof);
    if (!status.ok()) return status;
    if (eof) break;
    if (NullKey(slot, false)) continue;
    const size_t bytes = SlotBytes(slot) + 128;
    status = estate->budget.Reserve(bytes);
    if (!status.ok()) return status;
    build_bytes += bytes;
    buckets[HashKey(slot, false)].push_back(
        static_cast<uint32_t>(build_rows.size()));
    build_rows.push_back(std::move(slot));
  }
  built = true;
  return Status::Ok();
}

Status HashJoinState::Next(TupleSlot* out, bool* eof) {
  if (finished) {
    *eof = true;
    return Status::Ok();
  }
  if (!built) {
    Status status = Build();
    if (!status.ok()) return status;
  }
  for (;;) {
    if (bucket != nullptr && bucket_pos < bucket->size()) {
      const TupleSlot& right_row = build_rows[(*bucket)[bucket_pos++]];
      ++stats.candidates;
      if (CompareKey(outer, right_row, true, false) != 0) continue;
      if (!ResidualMatch(outer, right_row)) continue;
      FormJoinedSlot(outer, right_row, out);
      ++stats.rows;
      *eof = false;
      return Status::Ok();
    }
    bucket = nullptr;
    bucket_pos = 0;
    bool input_done = false;
    Status status = left->Next(&outer, &input_done);
    if (!status.ok()) return status;
    if (input_done) {
      // The build side is no longer needed once the probe side is exhausted.
      estate->budget.Release(build_bytes);
      build_bytes = 0;
      build_rows.clear();
      buckets.clear();
      finished = true;
      *eof = true;
      return Status::Ok();
    }
    if (NullKey(outer, true)) continue;
    ++stats.probes;
    auto found = buckets.find(HashKey(outer, true));
    if (found == buckets.end()) continue;
    bucket = &found->second;
    bucket_pos = 0;
  }
}

Status HashJoinState::ReScan() {
  bucket = nullptr;
  bucket_pos = 0;
  outer.Clear();
  Status status = left->ReScan();
  if (!status.ok()) return status;
  if (finished) {
    status = right->ReScan();
    if (!status.ok()) return status;
    built = false;
    finished = false;
  }
  return Status::Ok();
}

Status HashJoinState::End() {
  estate->budget.Release(build_bytes);
  build_bytes = 0;
  build_rows.clear();
  buckets.clear();
  built = false;
  return Status::Ok();
}

Status MergeJoinState::Begin() {
  InitJoin(*plan);
  sorted = false;
  finished = false;
  li = ri = le = re = gx = gy = 0;
  in_group = false;
  return Status::Ok();
}

Status MergeJoinState::Drain(PlanState* child, bool left_side,
                             std::vector<TupleSlot>* rows,
                             std::vector<uint32_t>* order) {
  for (;;) {
    TupleSlot slot;
    bool eof = false;
    Status status = child->Next(&slot, &eof);
    if (!status.ok()) return status;
    if (eof) break;
    if (NullKey(slot, left_side)) continue;
    const size_t bytes = SlotBytes(slot) + 128;
    status = estate->budget.Reserve(bytes);
    if (!status.ok()) return status;
    sort_bytes += bytes;
    order->push_back(static_cast<uint32_t>(rows->size()));
    rows->push_back(std::move(slot));
  }
  return Status::Ok();
}

Status MergeJoinState::Sort() {
  Status status = Drain(left, true, &left_rows, &left_order);
  if (!status.ok()) return status;
  status = Drain(right, false, &right_rows, &right_order);
  if (!status.ok()) return status;
  std::sort(left_order.begin(), left_order.end(),
            [&](uint32_t x, uint32_t y) {
              return CompareKey(left_rows[x], left_rows[y], true, true) < 0;
            });
  std::sort(right_order.begin(), right_order.end(),
            [&](uint32_t x, uint32_t y) {
              return CompareKey(right_rows[x], right_rows[y], false, false) < 0;
            });
  sorted = true;
  return Status::Ok();
}

Status MergeJoinState::Next(TupleSlot* out, bool* eof) {
  if (finished) {
    *eof = true;
    return Status::Ok();
  }
  if (!sorted) {
    Status status = Sort();
    if (!status.ok()) return status;
  }
  for (;;) {
    if (in_group) {
      if (gx < le) {
        if (gy < re) {
          const TupleSlot& left_row = left_rows[left_order[gx]];
          const TupleSlot& right_row = right_rows[right_order[gy]];
          ++gy;
          ++stats.candidates;
          if (!ResidualMatch(left_row, right_row)) continue;
          FormJoinedSlot(left_row, right_row, out);
          ++stats.rows;
          *eof = false;
          return Status::Ok();
        }
        ++gx;
        gy = ri;
        continue;
      }
      in_group = false;
      li = le;
      ri = re;
    }
    while (li < left_order.size() && ri < right_order.size()) {
      const int cmp = CompareKey(left_rows[left_order[li]],
                                 right_rows[right_order[ri]], true, false);
      if (cmp < 0) {
        ++li;
        continue;
      }
      if (cmp > 0) {
        ++ri;
        continue;
      }
      le = li + 1;
      while (le < left_order.size() &&
             CompareKey(left_rows[left_order[li]], left_rows[left_order[le]],
                        true, true) == 0)
        ++le;
      re = ri + 1;
      while (re < right_order.size() &&
             CompareKey(right_rows[right_order[ri]],
                        right_rows[right_order[re]], false, false) == 0)
        ++re;
      gx = li;
      gy = ri;
      in_group = true;
      break;
    }
    if (!in_group) {
      // Both inputs are fully consumed; drop their materialized copies now.
      estate->budget.Release(sort_bytes);
      sort_bytes = 0;
      left_rows.clear();
      right_rows.clear();
      left_order.clear();
      right_order.clear();
      finished = true;
      *eof = true;
      return Status::Ok();
    }
  }
}

Status MergeJoinState::ReScan() {
  li = ri = le = re = gx = gy = 0;
  in_group = false;
  Status status = left->ReScan();
  if (!status.ok()) return status;
  status = right->ReScan();
  if (!status.ok()) return status;
  estate->budget.Release(sort_bytes);
  sort_bytes = 0;
  left_rows.clear();
  right_rows.clear();
  left_order.clear();
  right_order.clear();
  sorted = false;
  finished = false;
  return Status::Ok();
}

Status MergeJoinState::End() {
  estate->budget.Release(sort_bytes);
  sort_bytes = 0;
  left_rows.clear();
  right_rows.clear();
  left_order.clear();
  right_order.clear();
  sorted = false;
  return Status::Ok();
}

Status IndexJoinState::Begin() {
  InitJoin(*plan);
  inner = estate->plan->relations[plan->inner_relation].table;
  index = plan->index;
  indexed_column = plan->indexed_column;
  op = plan->op;
  probe_cell = plan->probe_cell;
  filters = plan->filters;
  filter_columns = plan->filter_columns;
  pending.clear();
  pending_pos = 0;
  null_rows.clear();
  nulls_built = false;
  null_bytes = 0;
  outer.Clear();

  inner_cell_of_flat.assign(estate->plan->columns.size(), -1);
  for (size_t i = 0; i < plan->inner_cells.size(); ++i)
    inner_cell_of_flat[static_cast<size_t>(plan->inner_cells[i])] =
        static_cast<int>(i);

  std::vector<size_t> locals;
  for (int flat : plan->inner_cells) {
    const auto& column = estate->plan->columns[static_cast<size_t>(flat)];
    if (!column.rid) locals.push_back(column.local);
  }
  for (int flat : filter_columns) {
    const auto& column = estate->plan->columns[static_cast<size_t>(flat)];
    if (!column.rid) locals.push_back(column.local);
  }
  std::sort(locals.begin(), locals.end());
  locals.erase(std::unique(locals.begin(), locals.end()), locals.end());
  inner_decode_local = std::move(locals);
  inner_value_of_flat.assign(estate->plan->columns.size(), -1);
  const auto mark = [&](int flat) {
    const auto& column = estate->plan->columns[static_cast<size_t>(flat)];
    if (column.rid) return;
    inner_value_of_flat[static_cast<size_t>(flat)] = static_cast<int>(
        std::lower_bound(inner_decode_local.begin(), inner_decode_local.end(),
                         column.local) -
        inner_decode_local.begin());
  };
  for (int flat : plan->inner_cells) mark(flat);
  for (int flat : filter_columns) mark(flat);
  return Status::Ok();
}

Status IndexJoinState::MaterializeInner(RID rid, const Tuple* known,
                                        TupleSlot* out) {
  out->rows.clear();
  out->rows.push_back({inner, rid});
  out->cells.clear();
  out->cells.reserve(plan->inner_cells.size());
  std::vector<Value> values;
  if (!inner_decode_local.empty()) {
    const Tuple* tuple = known;
    Tuple fetched;
    if (tuple == nullptr) {
      auto got = inner->heap->Get(estate->txn, rid);
      if (!got.ok()) return got.status();
      fetched = got.TakeValue();
      tuple = &fetched;
    }
    auto decoded = Tuple::DecodeColumns(inner->schema, tuple->data().data(),
                                        tuple->size(), inner_decode_local);
    if (!decoded.ok()) return decoded.status();
    values = decoded.TakeValue();
  }
  for (int flat : plan->inner_cells) {
    const auto& column = estate->plan->columns[static_cast<size_t>(flat)];
    if (column.rid)
      out->cells.push_back(Value::String(FormatRid(rid)));
    else
      out->cells.push_back(values[static_cast<size_t>(
          inner_value_of_flat[static_cast<size_t>(flat)])]);
  }
  return Status::Ok();
}

bool IndexJoinState::MatchInner(const TupleSlot& row, RID rid) const {
  Value rid_value;
  const auto get = [&](int flat) -> const Value& {
    const auto& column = estate->plan->columns[static_cast<size_t>(flat)];
    if (column.rid) {
      rid_value = Value::String(FormatRid(rid));
      return rid_value;
    }
    return row.cells[static_cast<size_t>(
        inner_cell_of_flat[static_cast<size_t>(flat)])];
  };
  for (const auto& predicate : filters)
    if (!MatchPredicate(predicate, get)) return false;
  return true;
}

Status IndexJoinState::Probe(const TupleSlot& left, const Value& key,
                             CompareOp probe_op) {
  Status error;
  Status scan = index->Scan(probe_op, key, [&](const IndexEntry& entry) {
    ++stats.candidates;
    TupleSlot inner_row;
    Status status = MaterializeInner(entry.rid, nullptr, &inner_row);
    if (!status.ok()) {
      error = status;
      return false;
    }
    if (!MatchInner(inner_row, entry.rid)) return true;
    if (!ResidualMatch(left, inner_row)) return true;
    pending.push_back(std::move(inner_row));
    return true;
  });
  if (!scan.ok()) return scan;
  return error;
}

Status IndexJoinState::ProbeCross(const TupleSlot& left) {
  Status error;
  Status scan =
      index->Scan(CompareOp::kGe, Minimum(index->metadata().type),
                  [&](const IndexEntry& entry) {
                    ++stats.candidates;
                    TupleSlot inner_row;
                    Status status =
                        MaterializeInner(entry.rid, nullptr, &inner_row);
                    if (!status.ok()) {
                      error = status;
                      return false;
                    }
                    if (!MatchInner(inner_row, entry.rid)) return true;
                    if (!ResidualMatch(left, inner_row)) return true;
                    pending.push_back(std::move(inner_row));
                    return true;
                  });
  if (!scan.ok()) return scan;
  if (!error.ok()) return error;
  for (const auto& row : null_rows) {
    ++stats.candidates;
    if (!ResidualMatch(left, row)) continue;
    pending.push_back(row);
  }
  return Status::Ok();
}

Status IndexJoinState::BuildNullRows() {
  auto iterator = inner->heap->Scan(estate->txn);
  RID rid;
  Tuple tuple;
  while (iterator.Next(&rid, &tuple)) {
    auto key = Tuple::DecodeColumns(inner->schema, tuple.data().data(),
                                    tuple.size(), {indexed_column});
    if (!key.ok()) return key.status();
    if (!key.value()[0].IsNull()) continue;
    TupleSlot row;
    Status status = MaterializeInner(rid, &tuple, &row);
    if (!status.ok()) return status;
    if (!MatchInner(row, rid)) continue;
    const size_t bytes = SlotBytes(row);
    status = estate->budget.Reserve(bytes);
    if (!status.ok()) return status;
    null_bytes += bytes;
    null_rows.push_back(std::move(row));
  }
  if (!iterator.status().ok()) return iterator.status();
  nulls_built = true;
  return Status::Ok();
}

Status IndexJoinState::Next(TupleSlot* out, bool* eof) {
  for (;;) {
    if (pending_pos < pending.size()) {
      FormJoinedSlot(outer, pending[pending_pos++], out);
      ++stats.rows;
      *eof = false;
      return Status::Ok();
    }
    pending.clear();
    pending_pos = 0;
    bool input_done = false;
    Status status = left->Next(&outer, &input_done);
    if (!status.ok()) return status;
    if (input_done) {
      *eof = true;
      return Status::Ok();
    }
    if (probe_cell >= 0) {
      if (outer.cells[static_cast<size_t>(probe_cell)].IsNull()) continue;
      ++stats.probes;
      status = Probe(outer, outer.cells[static_cast<size_t>(probe_cell)], op);
    } else {
      ++stats.probes;
      if (!nulls_built) {
        status = BuildNullRows();
        if (!status.ok()) return status;
      }
      status = ProbeCross(outer);
    }
    if (!status.ok()) return status;
  }
}

Status IndexJoinState::ReScan() {
  pending.clear();
  pending_pos = 0;
  outer.Clear();
  return left->ReScan();
}

Status IndexJoinState::End() {
  estate->budget.Release(null_bytes);
  null_bytes = 0;
  null_rows.clear();
  pending.clear();
  return Status::Ok();
}

}  // namespace minidb
