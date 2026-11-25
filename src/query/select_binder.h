// Binds a SelectSpec and produces a static SelectPlan (steps 1-7).
// No storage pages are read here: only catalog, schema and index metadata.
#pragma once

#include <functional>
#include <string>

#include "common/status.h"
#include "query/select_plan.h"
#include "sql/parser.h"

namespace minidb {

class Catalog;

class SelectBinder {
 public:
  using FindIndexFn = std::function<BPlusTree*(const TableInfo&, size_t)>;

  SelectBinder(Catalog& catalog, FindIndexFn find_index,
               std::string join_algorithm, std::string scan_method);

  Result<SelectPlan> Bind(const SelectSpec& spec);

 private:
  Catalog& catalog_;
  FindIndexFn find_index_;
  std::string join_algorithm_;
  std::string scan_method_;
};

}  // namespace minidb
