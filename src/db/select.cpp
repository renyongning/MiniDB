// SPJ entry point: binding/planning lives in query/select_binder.cpp and the
// plan is executed by the Volcano executor in execution/executor.cpp.
#include "db/database.h"

#include "execution/executor.h"
#include "query/select_binder.h"

namespace minidb {

Status Database::ExecuteSelect(Statement* statement, Transaction* txn,
                               QueryResult* result) {
  const auto& spec = statement->select;
  SelectBinder binder(
      *catalog_,
      [this](const TableInfo& table, size_t column) {
        return FindIndex(table, column);
      },
      join_algorithm_, scan_method_);
  auto bound = binder.Bind(spec);
  if (!bound.ok()) return bound.status();
  const SelectPlan& plan = bound.value();
  if (plan.explain) {
    result->columns = {{"plan", TypeId::kVarchar}};
    for (const auto& line : plan.plan_lines)
      result->rows.push_back({Value::String(line)});
    return Status::Ok();
  }
  Executor executor(plan, txn, query_memory_);
  return executor.Run(result);
}

}  // namespace minidb
