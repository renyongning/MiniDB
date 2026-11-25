// SPJ binder and left-deep execution. Storage access stays in Database;
// in-memory relational algorithms live in execution/spj.cpp.
#include <algorithm>
#include <limits>
#include <set>
#include <sstream>

#include "common/util.h"
#include "db/database.h"
#include "execution/spj.h"

namespace minidb {
namespace {
struct BoundColumn {
  size_t table, local;
  Column column;
  bool rid = false;
};
struct Operand {
  int column = -1;
  Value literal;
};
struct Predicate {
  Operand left, right;
  CompareOp op;
  size_t first, last;
};
struct JoinPlan {
  std::string algorithm;
  std::vector<spj::EquiKey> equalities;
  BPlusTree* index = nullptr;
  int probe = -1;
  size_t indexed_column = 0;
  CompareOp op = CompareOp::kEq;  // inner key op outer value
};
CompareOp Reverse(CompareOp op) {
  switch (op) {
    case CompareOp::kLt:
      return CompareOp::kGt;
    case CompareOp::kLe:
      return CompareOp::kGe;
    case CompareOp::kGt:
      return CompareOp::kLt;
    case CompareOp::kGe:
      return CompareOp::kLe;
    default:
      return op;
  }
}
bool StringType(TypeId type) {
  return type == TypeId::kChar || type == TypeId::kVarchar;
}
bool Compatible(TypeId a, TypeId b) {
  if (a == b || (StringType(a) && StringType(b))) return true;
  const auto integer = [](TypeId t) {
    return t == TypeId::kInt32 || t == TypeId::kInt64;
  };
  return integer(a) && integer(b);
}
Value Minimum(TypeId type) {
  if (StringType(type)) return Value::String("");
  if (type == TypeId::kDate)
    return Value::Date(std::numeric_limits<int32_t>::min());
  return Value::Int64(std::numeric_limits<int64_t>::min());
}
bool Match(const Predicate& p, const std::function<const Value&(int)>& get) {
  const auto& a = p.left.column < 0 ? p.left.literal : get(p.left.column);
  const auto& b = p.right.column < 0 ? p.right.literal : get(p.right.column);
  return !a.IsNull() && !b.IsNull() && Satisfies(CompareValues(a, b), p.op);
}
}  // namespace

Status Database::ExecuteSelect(Statement* statement, Transaction* txn,
                               QueryResult* result) {
  const auto& spec = statement->select;
  std::vector<TableInfo*> tables;
  std::vector<BoundColumn> columns;
  std::vector<size_t> offsets;
  std::set<std::string> aliases;
  for (size_t t = 0; t < spec.from.size(); ++t) {
    const auto& ref = spec.from[t];
    if (!aliases.insert(ToLower(ref.alias)).second)
      return Status(StatusCode::kInvalidArgument,
                    "duplicate table alias: " + ref.alias);
    auto found = catalog_->GetTable(ref.name);
    if (!found.ok()) return found.status();
    auto* table = found.value();
    tables.push_back(table);
    offsets.push_back(columns.size());
    for (size_t c = 0; c < table->schema.num_columns(); ++c)
      columns.push_back({t, c, table->schema.GetColumn(c), false});
    if (table->schema.IndexOf("rid") < 0)
      columns.push_back({t, table->schema.num_columns(),
                         Column{"rid", TypeId::kVarchar, 32, 0, false}, true});
  }
  const auto resolve = [&](const std::string& text) -> Result<int> {
    const auto dot = text.find('.');
    const auto qualifier =
        dot == std::string::npos ? "" : ToLower(text.substr(0, dot));
    const auto name =
        ToLower(dot == std::string::npos ? text : text.substr(dot + 1));
    int found = -1;
    for (size_t i = 0; i < columns.size(); ++i) {
      const auto& c = columns[i];
      if (!qualifier.empty() && qualifier != ToLower(spec.from[c.table].alias))
        continue;
      if (ToLower(c.column.name) != name) continue;
      if (found >= 0)
        return Status(StatusCode::kInvalidArgument,
                      "ambiguous column: " + text);
      found = static_cast<int>(i);
    }
    if (found < 0)
      return Status(StatusCode::kNotFound, "column not found: " + text);
    return found;
  };
  std::vector<size_t> projection;
  for (const auto& text : spec.columns) {
    if (text == "*" ||
        (text.size() > 2 && text.substr(text.size() - 2) == ".*")) {
      const auto alias =
          text == "*" ? "" : ToLower(text.substr(0, text.size() - 2));
      if (!alias.empty() && !aliases.count(alias))
        return Status(StatusCode::kNotFound, "unknown table alias: " + alias);
      for (size_t c = 0; c < columns.size(); ++c)
        if (!columns[c].rid &&
            (alias.empty() ||
             ToLower(spec.from[columns[c].table].alias) == alias))
          projection.push_back(c);
    } else {
      auto c = resolve(text);
      if (!c.ok()) return c.status();
      projection.push_back(c.value());
    }
  }
  std::vector<Predicate> predicates;
  for (const auto& p : spec.predicates) {
    Predicate bound;
    bound.op = p.op;
    if (p.left.is_column) {
      auto c = resolve(p.left.column);
      if (!c.ok()) return c.status();
      bound.left.column = c.value();
    }
    if (p.right.is_column) {
      auto c = resolve(p.right.column);
      if (!c.ok()) return c.status();
      bound.right.column = c.value();
    }
    if (bound.left.column < 0 && bound.right.column < 0)
      return Status(StatusCode::kNotSupported,
                    "a comparison must reference a column");
    if (bound.left.column >= 0 && bound.right.column >= 0) {
      if (!Compatible(columns[bound.left.column].column.type,
                      columns[bound.right.column].column.type))
        return Status(StatusCode::kTypeMismatch,
                      "join columns must have compatible types");
    } else {
      const int c =
          bound.left.column >= 0 ? bound.left.column : bound.right.column;
      auto v = LiteralToValue(
          bound.left.column >= 0 ? p.right.literal : p.left.literal,
          columns[c].column);
      if (!v.ok()) return v.status();
      if (v.value().IsNull())
        return Status(StatusCode::kInvalidArgument,
                      "WHERE NULL is not supported");
      if (bound.left.column < 0)
        bound.left.literal = v.TakeValue();
      else
        bound.right.literal = v.TakeValue();
      if (columns[c].rid) {
        if (bound.op != CompareOp::kEq)
          return Status(StatusCode::kNotSupported,
                        "only rid equality is supported");
        const auto& literal =
            bound.left.column < 0 ? bound.left.literal : bound.right.literal;
        auto rid = ParseRid(literal.string_value);
        if (!rid.ok()) return rid.status();
      }
    }
    const size_t a =
        columns[bound.left.column >= 0 ? bound.left.column : bound.right.column]
            .table;
    const size_t b = columns[bound.right.column >= 0 ? bound.right.column
                                                     : bound.left.column]
                         .table;
    bound.first = std::min(a, b);
    bound.last = std::max(a, b);
    predicates.push_back(std::move(bound));
  }
  std::vector<JoinPlan> plans(tables.size());
  std::vector<std::string> plan_lines;
  for (size_t t = 1; t < tables.size(); ++t) {
    auto& plan = plans[t];
    plan.algorithm = join_algorithm_;
    for (const auto& p : predicates) {
      if (p.first == p.last || p.last != t) continue;
      const bool right_inner = columns[p.right.column].table == t;
      const int inner = right_inner ? p.right.column : p.left.column;
      const int outer = right_inner ? p.left.column : p.right.column;
      if (p.op == CompareOp::kEq)
        plan.equalities.push_back(
            {static_cast<size_t>(outer), columns[inner].local});
      if (!columns[inner].rid && (!plan.index || p.op == CompareOp::kEq)) {
        auto* index = FindIndex(*tables[t], columns[inner].local);
        if (index) {
          plan.index = index;
          plan.probe = outer;
          plan.indexed_column = columns[inner].local;
          plan.op = right_inner ? Reverse(p.op) : p.op;
        }
      }
    }
    if (plan.algorithm == "auto")
      plan.algorithm = plan.index                 ? "index"
                       : !plan.equalities.empty() ? "hash"
                                                  : "nested_loop";
    if ((plan.algorithm == "sort_merge" || plan.algorithm == "hash") &&
        plan.equalities.empty())
      return Status(StatusCode::kNotSupported,
                    plan.algorithm +
                        " requires an equality predicate at each join stage");
    if (plan.algorithm == "index" && !plan.index) {
      bool cross = true;
      for (const auto& p : predicates)
        if (p.first != p.last && p.last == t) cross = false;
      if (cross) {
        for (size_t c = 0; c < tables[t]->schema.num_columns(); ++c)
          if (auto* index = FindIndex(*tables[t], c)) {
            plan.index = index;
            plan.indexed_column = c;
            break;
          }
      }
      if (!plan.index)
        return Status(
            StatusCode::kNotSupported,
            "index join requires an index on the right table's join column");
    }
  }
  // Only row payload is budgeted; leave room for sort permutations/hash
  // buckets.
  size_t used = 0;
  const auto reserve = [&](size_t bytes) -> Status {
    if (bytes > query_memory_ || used > query_memory_ - bytes)
      return Status(StatusCode::kNoSpace,
                    "SPJ query memory budget exceeded; increase PRAGMA "
                    "query_memory or narrow the query");
    used += bytes;
    return Status::Ok();
  };
  const auto decode = [&](size_t t, RID rid,
                          const Tuple& tuple) -> Result<spj::Row> {
    auto values =
        Tuple::Decode(tables[t]->schema, tuple.data().data(), tuple.size());
    if (!values.ok()) return values.status();
    auto row = values.TakeValue();
    if (tables[t]->schema.IndexOf("rid") < 0)
      row.push_back(Value::String(FormatRid(rid)));
    return row;
  };
  const auto local_match = [&](size_t t, const spj::Row& row) {
    for (const auto& p : predicates)
      if (p.first == t && p.last == t &&
          !Match(p, [&](int c) -> const Value& { return row[c - offsets[t]]; }))
        return false;
    return true;
  };
  std::vector<spj::Rows> inputs(tables.size());
  std::string single_index;
  for (size_t t = 0; t < tables.size(); ++t) {
    if (t && plans[t].algorithm == "index") {
      plan_lines.push_back("INDEX JOIN " + plans[t].index->metadata().name +
                           (plans[t].probe < 0 ? " (CROSS, includes NULL keys)"
                                               : " (probe + residual filter)"));
      continue;
    }
    BPlusTree* index = nullptr;
    CompareOp op = CompareOp::kGe;
    Value bound;
    int direct_rid = -1;
    bool full_index = false;
    for (size_t i = 0; i < predicates.size(); ++i) {
      const auto& p = predicates[i];
      if (p.first != t || p.last != t ||
          (p.left.column >= 0 && p.right.column >= 0))
        continue;
      const int c = p.left.column >= 0 ? p.left.column : p.right.column;
      if (columns[c].rid) {
        direct_rid = static_cast<int>(i);
        continue;
      }
      if (scan_method_ == "table" ||
          (scan_method_ == "auto" && p.op == CompareOp::kNe))
        continue;
      auto* candidate = FindIndex(*tables[t], columns[c].local);
      if (candidate && (!index || p.op == CompareOp::kEq)) {
        index = candidate;
        op = p.left.column >= 0 ? p.op : Reverse(p.op);
        bound = p.left.column >= 0 ? p.right.literal : p.left.literal;
      }
    }
    if (scan_method_ == "index" && !index && direct_rid < 0) {
      for (size_t c = 0; c < tables[t]->schema.num_columns(); ++c)
        if ((index = FindIndex(*tables[t], c))) break;
      if (!index)
        return Status(StatusCode::kNotSupported,
                      "IndexScan requested but table has no usable index");
      full_index = true;
      bound = Minimum(index->metadata().type);
    }
    const std::string scan = direct_rid >= 0
                                 ? "RID LOOKUP " + spec.from[t].alias
                             : index ? "INDEX SCAN " + index->metadata().name
                                     : "TABLE SCAN " + spec.from[t].alias;
    plan_lines.push_back(scan);
    if (tables.size() == 1 && index && direct_rid < 0)
      single_index = "INDEX SCAN " + index->metadata().name;
    if (spec.explain || spec.limit == 0) continue;
    Status error;
    const auto append = [&](RID rid, const Tuple& tuple) {
      auto values = decode(t, rid, tuple);
      if (!values.ok()) {
        error = values.status();
        return false;
      }
      if (!local_match(t, values.value())) return true;
      error = reserve(spj::RowBytes(values.value()));
      if (!error.ok()) return false;
      inputs[t].push_back(values.TakeValue());
      return tables.size() != 1 || spec.limit < 0 ||
             static_cast<int64_t>(inputs[t].size()) < spec.limit;
    };
    if (direct_rid >= 0) {
      const auto& p = predicates[direct_rid];
      const auto& v = p.left.column >= 0 ? p.right.literal : p.left.literal;
      auto rid = ParseRid(v.string_value);
      auto tuple = tables[t]->heap->Get(txn, rid.value());
      if (tuple.ok())
        append(rid.value(), tuple.value());
      else if (tuple.status().code() != StatusCode::kNotFound)
        return tuple.status();
    } else if (index) {
      auto s = index->Scan(op, bound, [&](const IndexEntry& entry) {
        auto tuple = tables[t]->heap->Get(txn, entry.rid);
        if (!tuple.ok()) {
          error = tuple.status();
          return false;
        }
        return append(entry.rid, tuple.value());
      });
      if (!s.ok()) return s;
    }
    if (error.ok() && (direct_rid < 0 && (!index || full_index))) {
      auto it = tables[t]->heap->Scan(txn);
      RID rid;
      Tuple tuple;
      while (it.Next(&rid, &tuple)) {
        if (tables.size() == 1 && spec.limit >= 0 &&
            static_cast<int64_t>(inputs[t].size()) >= spec.limit)
          break;
        if (full_index) {
          auto row = decode(t, rid, tuple);
          if (!row.ok()) return row.status();
          if (!row.value()[index->metadata().column].IsNull()) continue;
        }
        if (!append(rid, tuple)) break;
      }
      if (!it.status().ok()) return it.status();
    }
    if (!error.ok()) return error;
  }
  for (size_t t = 1; t < tables.size(); ++t)
    if (plans[t].algorithm != "index")
      plan_lines.push_back(plans[t].algorithm + " JOIN " + spec.from[t].alias);
  plan_lines.push_back("PROJECT " + std::to_string(projection.size()) +
                       " column(s)");
  if (spec.explain) {
    result->columns = {{"plan", TypeId::kVarchar}};
    for (const auto& line : plan_lines)
      result->rows.push_back({Value::String(line)});
    return Status::Ok();
  }
  spj::Rows current = std::move(inputs[0]);
  spj::Stats stats;
  for (size_t t = 1; t < tables.size() && spec.limit != 0; ++t) {
    const auto& plan = plans[t];
    spj::Rows output;
    Status error;
    bool stopped = false;
    const auto emit = [&](const spj::Row& left, const spj::Row& right) {
      for (const auto& p : predicates)
        if (p.last == t && p.first < t &&
            !Match(
                p,
                [&](int c) -> const Value& {
                  return static_cast<size_t>(c) < offsets[t]
                             ? left[c]
                             : right[c - offsets[t]];
                }))
          return true;
      const size_t bytes =
          spj::RowBytes(left) + spj::RowBytes(right) - sizeof(spj::Row);
      error = reserve(bytes);
      if (!error.ok()) {
        stopped = true;
        return false;
      }
      spj::Row joined = left;
      joined.insert(joined.end(), right.begin(), right.end());
      output.push_back(std::move(joined));
      stopped = t + 1 == tables.size() && spec.limit >= 0 &&
                static_cast<int64_t>(output.size()) >= spec.limit;
      return !stopped;
    };
    if (plan.algorithm == "index") {
      // CROSS traverses all non-NULL index entries and separately the NULL
      // bucket, because B+ indexes deliberately omit SQL NULL keys.
      spj::Rows nulls;
      if (plan.probe < 0 && !current.empty()) {
        auto it = tables[t]->heap->Scan(txn);
        RID rid;
        Tuple tuple;
        while (it.Next(&rid, &tuple)) {
          auto row = decode(t, rid, tuple);
          if (!row.ok()) return row.status();
          if (row.value()[plan.indexed_column].IsNull() &&
              local_match(t, row.value())) {
            auto s = reserve(spj::RowBytes(row.value()));
            if (!s.ok()) return s;
            nulls.push_back(row.TakeValue());
          }
        }
        if (!it.status().ok()) return it.status();
      }
      for (const auto& left : current) {
        if (plan.probe >= 0 && left[plan.probe].IsNull()) continue;
        ++stats.probes;
        const auto bound = plan.probe >= 0
                               ? left[plan.probe]
                               : Minimum(plan.index->metadata().type);
        auto s = plan.index->Scan(
            plan.probe >= 0 ? plan.op : CompareOp::kGe, bound,
            [&](const IndexEntry& e) {
              ++stats.candidates;
              auto tuple = tables[t]->heap->Get(txn, e.rid);
              if (!tuple.ok()) {
                error = tuple.status();
                return false;
              }
              auto row = decode(t, e.rid, tuple.value());
              if (!row.ok()) {
                error = row.status();
                return false;
              }
              return !local_match(t, row.value()) || emit(left, row.value());
            });
        if (!s.ok()) return s;
        if (!error.ok() || stopped) break;
        for (const auto& row : nulls) {
          ++stats.candidates;
          if (!emit(left, row)) break;
        }
        if (stopped) break;
      }
      for (const auto& row : nulls) used -= spj::RowBytes(row);
    } else {
      // Account conservatively for the auxiliary hash buckets or sort arrays.
      const size_t aux = plan.algorithm == "nested_loop"
                             ? 0
                             : (current.size() + inputs[t].size()) * 128;
      error = reserve(aux);
      if (!error.ok()) return error;
      if (plan.algorithm == "nested_loop")
        spj::NestedLoop(current, inputs[t], emit, &stats);
      else if (plan.algorithm == "sort_merge")
        spj::SortMerge(current, inputs[t], plan.equalities, emit, &stats);
      else
        spj::HashJoin(current, inputs[t], plan.equalities, emit, &stats);
      used -= aux;
    }
    if (!error.ok()) return error;
    for (const auto& row : current) used -= spj::RowBytes(row);
    for (const auto& row : inputs[t]) used -= spj::RowBytes(row);
    inputs[t].clear();
    inputs[t].shrink_to_fit();
    current = std::move(output);
  }
  for (size_t c : projection) {
    const auto& column = columns[c];
    result->columns.push_back(
        {tables.size() > 1
             ? spec.from[column.table].alias + "." + column.column.name
             : column.column.name,
         column.column.type});
  }
  for (const auto& row : current) {
    if (spec.limit >= 0 &&
        static_cast<int64_t>(result->rows.size()) >= spec.limit)
      break;
    spj::Row projected;
    for (size_t c : projection) projected.push_back(row[c]);
    auto s = reserve(spj::RowBytes(projected));
    if (!s.ok()) return s;
    result->rows.push_back(std::move(projected));
  }
  const bool detailed =
      tables.size() > 1 || predicates.size() > 1 || scan_method_ != "auto";
  if (detailed) {
    std::ostringstream message;
    for (const auto& line : plan_lines) message << line << '\n';
    message << "SPJ rows=" << result->rows.size()
            << " candidates=" << stats.candidates << " probes=" << stats.probes;
    result->message = message.str();
  } else
    result->message =
        single_index;  // Keep the existing C API message contract.
  return Status::Ok();
}
}  // namespace minidb
