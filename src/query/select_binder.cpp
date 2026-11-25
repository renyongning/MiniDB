#include "query/select_binder.h"

#include <algorithm>
#include <limits>
#include <set>
#include <utility>
#include <vector>

#include "catalog/catalog.h"
#include "common/util.h"
#include "db/database.h"

namespace minidb {
namespace {

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

}  // namespace

SelectBinder::SelectBinder(Catalog& catalog, FindIndexFn find_index,
                           std::string join_algorithm, std::string scan_method)
    : catalog_(catalog),
      find_index_(std::move(find_index)),
      join_algorithm_(std::move(join_algorithm)),
      scan_method_(std::move(scan_method)) {}

Result<SelectPlan> SelectBinder::Bind(const SelectSpec& spec) {
  SelectPlan plan;
  std::set<std::string> aliases;
  for (size_t t = 0; t < spec.from.size(); ++t) {
    const auto& ref = spec.from[t];
    if (!aliases.insert(ToLower(ref.alias)).second)
      return Status(StatusCode::kInvalidArgument,
                    "duplicate table alias: " + ref.alias);
    auto found = catalog_.GetTable(ref.name);
    if (!found.ok()) return found.status();
    auto* table = found.value();
    plan.relations.push_back({table, ref.alias});
    for (size_t c = 0; c < table->schema.num_columns(); ++c)
      plan.columns.push_back({t, c, table->schema.GetColumn(c), false});
    if (table->schema.IndexOf("rid") < 0)
      plan.columns.push_back({t, table->schema.num_columns(),
                              Column{"rid", TypeId::kVarchar, 32, 0, false},
                              true});
  }
  if (plan.relations.empty())
    return Status(StatusCode::kInvalidArgument,
                  "SELECT requires at least one FROM relation");

  const auto resolve = [&](const std::string& text) -> Result<int> {
    const auto dot = text.find('.');
    const auto qualifier =
        dot == std::string::npos ? "" : ToLower(text.substr(0, dot));
    const auto name =
        ToLower(dot == std::string::npos ? text : text.substr(dot + 1));
    int found = -1;
    for (size_t i = 0; i < plan.columns.size(); ++i) {
      const auto& c = plan.columns[i];
      if (!qualifier.empty() && qualifier != ToLower(spec.from[c.relation].alias))
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
      for (size_t c = 0; c < plan.columns.size(); ++c)
        if (!plan.columns[c].rid &&
            (alias.empty() ||
             ToLower(spec.from[plan.columns[c].relation].alias) == alias))
          projection.push_back(c);
    } else {
      auto c = resolve(text);
      if (!c.ok()) return c.status();
      projection.push_back(static_cast<size_t>(c.value()));
    }
  }

  std::vector<BoundPredicate> predicates;
  for (const auto& p : spec.predicates) {
    BoundPredicate bound;
    bound.op = p.op;
    if (p.left.is_column) {
      auto c = resolve(p.left.column);
      if (!c.ok()) return c.status();
      bound.left = c.value();
    }
    if (p.right.is_column) {
      auto c = resolve(p.right.column);
      if (!c.ok()) return c.status();
      bound.right = c.value();
    }
    if (bound.left < 0 && bound.right < 0)
      return Status(StatusCode::kNotSupported,
                    "a comparison must reference a column");
    if (bound.left >= 0 && bound.right >= 0) {
      if (!Compatible(plan.columns[bound.left].column.type,
                      plan.columns[bound.right].column.type))
        return Status(StatusCode::kTypeMismatch,
                      "join columns must have compatible types");
    } else {
      const int c = bound.left >= 0 ? bound.left : bound.right;
      auto v = LiteralToValue(bound.left >= 0 ? p.right.literal : p.left.literal,
                              plan.columns[c].column);
      if (!v.ok()) return v.status();
      if (v.value().IsNull())
        return Status(StatusCode::kInvalidArgument,
                      "WHERE NULL is not supported");
      if (bound.left < 0)
        bound.left_literal = v.TakeValue();
      else
        bound.right_literal = v.TakeValue();
      if (plan.columns[c].rid) {
        if (bound.op != CompareOp::kEq)
          return Status(StatusCode::kNotSupported,
                        "only rid equality is supported");
        const auto& literal =
            bound.left < 0 ? bound.left_literal : bound.right_literal;
        auto rid = ParseRid(literal.string_value);
        if (!rid.ok()) return rid.status();
      }
    }
    const size_t a =
        plan.columns[bound.left >= 0 ? bound.left : bound.right].relation;
    const size_t b =
        plan.columns[bound.right >= 0 ? bound.right : bound.left].relation;
    bound.first = std::min(a, b);
    bound.last = std::max(a, b);
    predicates.push_back(std::move(bound));
  }

  const size_t nrelations = plan.relations.size();
  const size_t ncolumns = plan.columns.size();

  // Which columns are consumed by joins, and when each column is last read.
  std::vector<char> join_used(ncolumns, 0);
  std::vector<size_t> last_stage(ncolumns, 0);
  std::vector<std::vector<size_t>> local_predicates(nrelations);
  std::vector<std::vector<int>> local_columns(nrelations);
  for (size_t i = 0; i < predicates.size(); ++i) {
    const auto& p = predicates[i];
    if (p.left >= 0)
      last_stage[p.left] = std::max(last_stage[p.left], p.last);
    if (p.right >= 0)
      last_stage[p.right] = std::max(last_stage[p.right], p.last);
    if (p.IsJoin()) {
      if (p.left >= 0) join_used[p.left] = 1;
      if (p.right >= 0) join_used[p.right] = 1;
    } else {
      local_predicates[p.first].push_back(i);
      if (p.left >= 0) local_columns[p.first].push_back(p.left);
      if (p.right >= 0) local_columns[p.first].push_back(p.right);
    }
  }
  for (auto& cols : local_columns) {
    std::sort(cols.begin(), cols.end());
    cols.erase(std::unique(cols.begin(), cols.end()), cols.end());
  }

  // Scan output cells: columns this relation contributes to later joins.
  std::vector<std::vector<int>> scan_cells(nrelations);
  for (size_t c = 0; c < ncolumns; ++c)
    if (join_used[c])
      scan_cells[plan.columns[c].relation].push_back(static_cast<int>(c));

  // Per-stage join metadata (algorithm, probe key, equality keys).
  struct StagePlan {
    std::string algorithm;
    std::vector<std::pair<int, int>> equalities;  // (outer flat, inner flat)
    BPlusTree* index = nullptr;
    int probe_flat = -1;
    size_t indexed_column = 0;
    CompareOp op = CompareOp::kEq;
  };
  std::vector<StagePlan> stages(nrelations);
  for (size_t t = 1; t < nrelations; ++t) {
    auto& stage = stages[t];
    stage.algorithm = join_algorithm_;
    for (const auto& p : predicates) {
      if (!p.IsJoin() || p.last != t) continue;
      const bool right_inner = plan.columns[p.right].relation == t;
      const int inner = right_inner ? p.right : p.left;
      const int outer = right_inner ? p.left : p.right;
      if (p.op == CompareOp::kEq) stage.equalities.push_back({outer, inner});
      if (!plan.columns[inner].rid && (!stage.index || p.op == CompareOp::kEq)) {
        BPlusTree* candidate =
            find_index_(*plan.relations[t].table, plan.columns[inner].local);
        if (candidate) {
          stage.index = candidate;
          stage.probe_flat = outer;
          stage.indexed_column = plan.columns[inner].local;
          stage.op = right_inner ? Reverse(p.op) : p.op;
        }
      }
    }
    if (stage.algorithm == "auto")
      stage.algorithm = stage.index                  ? "index"
                        : !stage.equalities.empty()  ? "hash"
                                                     : "nested_loop";
    if ((stage.algorithm == "sort_merge" || stage.algorithm == "hash") &&
        stage.equalities.empty())
      return Status(StatusCode::kNotSupported,
                    stage.algorithm +
                        " requires an equality predicate at each join stage");
    if (stage.algorithm == "index" && !stage.index) {
      bool cross = true;
      for (const auto& p : predicates)
        if (p.IsJoin() && p.last == t) cross = false;
      if (cross) {
        for (size_t c = 0; c < plan.relations[t].table->schema.num_columns();
             ++c) {
          if (BPlusTree* candidate =
                  find_index_(*plan.relations[t].table, c)) {
            stage.index = candidate;
            stage.indexed_column = c;
            break;
          }
        }
      }
      if (!stage.index)
        return Status(StatusCode::kNotSupported,
                      "index join requires an index on the right table's join "
                      "column");
    }
  }

  // Right input layout per stage. Index joins materialize the inner row's
  // join/filter columns; regular joins consume the scan's cells.
  std::vector<std::vector<int>> right_layout(nrelations);
  for (size_t t = 1; t < nrelations; ++t) {
    std::vector<int> cols = scan_cells[t];
    if (stages[t].algorithm == "index")
      cols.insert(cols.end(), local_columns[t].begin(),
                  local_columns[t].end());
    std::sort(cols.begin(), cols.end());
    cols.erase(std::unique(cols.begin(), cols.end()), cols.end());
    right_layout[t] = std::move(cols);
  }

  const auto make_pos = [&](const std::vector<int>& cells) {
    std::vector<int> pos(ncolumns, -1);
    for (size_t i = 0; i < cells.size(); ++i)
      pos[static_cast<size_t>(cells[i])] = static_cast<int>(i);
    return pos;
  };

  // Column lifetimes: a column survives stage t only while a later stage or
  // a later predicate still reads it. Projection columns never enter cells.
  std::vector<std::vector<int>> left_pos_stage(nrelations);
  std::vector<std::vector<int>> right_pos_stage(nrelations);
  std::vector<std::vector<int>> out_layout(nrelations);
  std::vector<std::vector<std::pair<uint32_t, uint32_t>>> left_sel(nrelations),
      right_sel(nrelations);
  std::vector<int> layout = scan_cells[0];
  for (size_t t = 1; t < nrelations; ++t) {
    left_pos_stage[t] = make_pos(layout);
    right_pos_stage[t] = make_pos(right_layout[t]);
    std::vector<int> next;
    next.reserve(layout.size() + right_layout[t].size());
    for (int c : layout)
      if (last_stage[static_cast<size_t>(c)] > t) next.push_back(c);
    for (int c : right_layout[t])
      if (last_stage[static_cast<size_t>(c)] > t) next.push_back(c);
    for (size_t dst = 0; dst < next.size(); ++dst) {
      const int c = next[dst];
      if (plan.columns[static_cast<size_t>(c)].relation < t)
        left_sel[t].push_back({static_cast<uint32_t>(left_pos_stage[t][c]),
                               static_cast<uint32_t>(dst)});
      else
        right_sel[t].push_back({static_cast<uint32_t>(right_pos_stage[t][c]),
                                static_cast<uint32_t>(dst)});
    }
    out_layout[t] = std::move(next);
    layout = out_layout[t];
  }

  // Scan nodes (index-join inner tables are not materialized here).
  std::vector<std::unique_ptr<PlanNode>> nodes(nrelations);
  std::vector<std::string> scan_lines(nrelations);
  std::string single_index;
  for (size_t t = 0; t < nrelations; ++t) {
    if (t > 0 && stages[t].algorithm == "index") continue;
    auto node = std::make_unique<PlanNode>();
    node->relation = t;
    node->cells = scan_cells[t];
    for (size_t i : local_predicates[t]) node->filters.push_back(predicates[i]);
    node->filter_columns = local_columns[t];

    BPlusTree* index = nullptr;
    CompareOp op = CompareOp::kGe;
    Value bound;
    int direct_rid = -1;
    bool full_index = false;
    for (size_t i = 0; i < node->filters.size(); ++i) {
      const auto& p = node->filters[i];
      if (p.left >= 0 && p.right >= 0) continue;
      const int c = p.left >= 0 ? p.left : p.right;
      if (plan.columns[c].rid) {
        direct_rid = static_cast<int>(i);
        continue;
      }
      if (scan_method_ == "table" ||
          (scan_method_ == "auto" && p.op == CompareOp::kNe))
        continue;
      BPlusTree* candidate =
          find_index_(*plan.relations[t].table, plan.columns[c].local);
      if (candidate && (!index || p.op == CompareOp::kEq)) {
        index = candidate;
        op = p.left >= 0 ? p.op : Reverse(p.op);
        bound = p.left >= 0 ? p.right_literal : p.left_literal;
      }
    }
    if (scan_method_ == "index" && !index && direct_rid < 0) {
      for (size_t c = 0; c < plan.relations[t].table->schema.num_columns();
           ++c) {
        if ((index = find_index_(*plan.relations[t].table, c))) break;
      }
      if (!index)
        return Status(StatusCode::kNotSupported,
                      "IndexScan requested but table has no usable index");
      full_index = true;
      bound = Minimum(index->metadata().type);
    }
    const std::string alias = spec.from[t].alias;
    if (direct_rid >= 0) {
      const auto& p = node->filters[static_cast<size_t>(direct_rid)];
      const auto& literal =
          p.left >= 0 ? p.right_literal : p.left_literal;
      auto rid = ParseRid(literal.string_value);
      if (!rid.ok()) return rid.status();
      node->tag = NodeTag::kRidLookup;
      node->rid = rid.value();
      node->has_rid = true;
      scan_lines[t] = "RID LOOKUP " + alias;
    } else if (index) {
      node->tag = NodeTag::kIndexScan;
      node->index = index;
      node->op = op;
      node->bound = bound;
      node->full_index = full_index;
      scan_lines[t] = "INDEX SCAN " + index->metadata().name;
      if (nrelations == 1) single_index = scan_lines[t];
    } else {
      node->tag = NodeTag::kSeqScan;
      scan_lines[t] = "TABLE SCAN " + alias;
    }
    nodes[t] = std::move(node);
  }

  std::unique_ptr<PlanNode> current = std::move(nodes[0]);
  for (size_t t = 1; t < nrelations; ++t) {
    auto join = std::make_unique<PlanNode>();
    join->cells = out_layout[t];
    join->left_sel = left_sel[t];
    join->right_sel = right_sel[t];
    join->algorithm = stages[t].algorithm;
    const auto bind_operand = [&](const BoundPredicate& p, bool left_operand) {
      EvalOperand operand;
      const int column = left_operand ? p.left : p.right;
      if (column < 0) {
        operand.literal =
            left_operand ? p.left_literal : p.right_literal;
      } else if (plan.columns[column].relation < t) {
        operand.cell = left_pos_stage[t][column];
        operand.left_side = true;
      } else {
        operand.cell = right_pos_stage[t][column];
        operand.left_side = false;
      }
      return operand;
    };
    for (const auto& p : predicates) {
      if (!p.IsJoin() || p.last != t) continue;
      ResidualPredicate residual;
      residual.op = p.op;
      residual.left = bind_operand(p, true);
      residual.right = bind_operand(p, false);
      join->residual.push_back(std::move(residual));
    }
    for (const auto& [outer, inner] : stages[t].equalities)
      join->keys.push_back({static_cast<uint32_t>(left_pos_stage[t][outer]),
                            static_cast<uint32_t>(right_pos_stage[t][inner])});
    join->left = std::move(current);
    if (stages[t].algorithm == "index") {
      join->tag = NodeTag::kIndexJoin;
      join->relation = t;
      join->inner_relation = t;
      join->index = stages[t].index;
      join->indexed_column = stages[t].indexed_column;
      join->op = stages[t].op;
      join->probe_cell =
          stages[t].probe_flat >= 0 ? left_pos_stage[t][stages[t].probe_flat]
                                    : -1;
      join->inner_cells = right_layout[t];
      for (size_t i : local_predicates[t]) join->filters.push_back(predicates[i]);
      join->filter_columns = local_columns[t];
    } else {
      join->tag = stages[t].algorithm == "hash"        ? NodeTag::kHashJoin
                  : stages[t].algorithm == "sort_merge" ? NodeTag::kMergeJoin
                                                        : NodeTag::kNestLoopJoin;
      join->right = std::move(nodes[t]);
    }
    current = std::move(join);
  }

  auto projection_node = std::make_unique<PlanNode>();
  projection_node->tag = NodeTag::kProjection;
  projection_node->left = std::move(current);
  for (size_t c : projection) {
    const auto& column = plan.columns[c];
    projection_node->output_columns.push_back(
        {static_cast<uint32_t>(column.relation),
         static_cast<uint32_t>(column.local), column.rid});
    QueryColumn output;
    output.type = column.column.type;
    output.name = nrelations > 1
                      ? spec.from[column.relation].alias + "." +
                            column.column.name
                      : column.column.name;
    plan.output.push_back(std::move(output));
  }
  plan.root = std::move(projection_node);

  for (size_t t = 0; t < nrelations; ++t) {
    if (t > 0 && stages[t].algorithm == "index")
      plan.plan_lines.push_back(
          "INDEX JOIN " + stages[t].index->metadata().name +
          (stages[t].probe_flat < 0 ? " (CROSS, includes NULL keys)"
                                    : " (probe + residual filter)"));
    else
      plan.plan_lines.push_back(scan_lines[t]);
  }
  for (size_t t = 1; t < nrelations; ++t)
    if (stages[t].algorithm != "index")
      plan.plan_lines.push_back(stages[t].algorithm + " JOIN " +
                                plan.relations[t].alias);
  plan.plan_lines.push_back("PROJECT " + std::to_string(projection.size()) +
                            " column(s)");

  plan.explain = spec.explain;
  plan.limit = spec.limit;
  plan.scan_limit = nrelations == 1 && spec.limit >= 0 ? spec.limit : -1;
  plan.detailed =
      nrelations > 1 || predicates.size() > 1 || scan_method_ != "auto";
  plan.single_index = std::move(single_index);
  plan.projection = std::move(projection);
  return plan;
}

}  // namespace minidb
