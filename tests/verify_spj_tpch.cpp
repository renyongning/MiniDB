// Reproducible acceptance on real TPC-H files. Never overwrites a database.
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "db/database.h"
using namespace minidb;
namespace {
void Check(bool ok, const std::string& message) {
  if (!ok) throw std::runtime_error(message);
}
std::string Quote(const std::string& s) {
  std::string out = "'";
  for (char c : s) {
    out += c;
    if (c == '\'') out += c;
  }
  return out + "'";
}
QueryResult Exec(Database& db, const std::string& sql) {
  QueryResult r;
  auto s = db.Execute(sql, &r);
  Check(s.ok(), sql.substr(0, 200) + ": " + s.message());
  return r;
}
std::vector<std::string> Fields(const std::string& line) {
  std::vector<std::string> fields;
  std::istringstream in(line);
  std::string field;
  while (std::getline(in, field, '|')) fields.push_back(field);
  return fields;
}
using Rows = std::vector<std::string>;
Rows Values(const QueryResult& r) {
  Rows out;
  for (const auto& row : r.rows) {
    std::string s;
    for (const auto& v : row) s += ValueToString(v) + '|';
    out.push_back(s);
  }
  std::sort(out.begin(), out.end());
  return out;
}
std::string Join(std::initializer_list<std::string> fields) {
  std::string s;
  for (const auto& f : fields) s += f + '|';
  return s;
}
std::vector<std::vector<std::string>> Read(const std::string& path,
                                           size_t limit = 0) {
  std::ifstream in(path);
  Check(in.good(), "cannot read " + path);
  std::vector<std::vector<std::string>> rows;
  std::string line;
  while ((!limit || rows.size() < limit) && std::getline(in, line))
    if (!line.empty()) rows.push_back(Fields(line));
  return rows;
}
}  // namespace
int main(int argc, char** argv) {
  try {
    Check(argc >= 3 && argc <= 4,
          "usage: minidb_verify_spj_tpch TBL_DIRECTORY NEW_DATABASE "
          "[orders_rows=50000]");
    const auto data = std::filesystem::absolute(argv[1]);
    const auto directory = std::filesystem::absolute(argv[2]);
    Check(!std::filesystem::exists(directory),
          "choose a new database directory");
    const size_t limit = argc == 4 ? std::stoull(argv[3]) : 50000;
    Check(limit > 0 && limit <= 1500000, "orders_rows must be 1..1500000");
    auto supplier = Read((data / "supplier.tbl").string());
    auto nation = Read((data / "nation.tbl").string());
    auto region = Read((data / "region.tbl").string());
    auto orders = Read((data / "orders.tbl").string(), limit);
    Check(!supplier.empty() && !nation.empty() && !region.empty() &&
              !orders.empty(),
          "empty input");
    Config config;
    config.path = directory.string();
    config.buffer_pool_frames = 32;
    config.log_level = "off";
    auto opened = Database::Open(config);
    Check(opened.ok(), opened.status().message());
    auto db = opened.TakeValue();
    std::ifstream schema(std::string(MINIDB_SOURCE_DIR) +
                         "/test/tpc-h/schema.sql");
    std::ostringstream ddl;
    ddl << schema.rdbuf();
    Check(schema.good(), "schema read failed");
    Exec(*db, ddl.str());
    for (const auto* table : {"supplier", "nation", "region"})
      Exec(*db, "COPY " + std::string(table) + " FROM " +
                    Quote((data / (std::string(table) + ".tbl")).string()));
    const auto sample = directory / "orders-prefix.tbl";
    {
      std::ofstream out(sample);
      for (const auto& row : orders) {
        for (const auto& f : row) out << f << '|';
        out << '\n';
      }
      Check(out.good(), "write prefix failed");
    }
    Exec(*db, "COPY orders FROM " + Quote(sample.string()));
    Exec(*db,
         "CREATE INDEX s_id ON supplier(s_suppkey); CREATE INDEX s_name ON "
         "supplier(s_name);"
         "CREATE INDEX n_id ON nation(n_nationkey); CREATE INDEX r_id ON "
         "region(r_regionkey);"
         "CREATE INDEX o_id ON orders(o_orderkey); CREATE INDEX o_priority ON "
         "orders(o_orderpriority);"
         "CREATE INDEX o_date ON orders(o_orderdate)");
    Check(db->Close().ok(), "close failed");
    db.reset();
    opened = Database::Open(config);
    Check(opened.ok(), opened.status().message());
    db = opened.TakeValue();
    size_t checks = 0;
    const auto verify = [&](const std::string& label, const std::string& query,
                            Rows expected) {
      std::sort(expected.begin(), expected.end());
      const auto start = std::chrono::steady_clock::now();
      auto r = Exec(*db, query);
      Check(Values(r) == expected, "reference mismatch: " + label);
      ++checks;
      const auto milliseconds = std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - start)
                                    .count();
      std::cout << label << " rows=" << r.rows.size() << " ms=" << milliseconds;
      const auto at = r.message.find("SPJ rows=");
      if (at != std::string::npos) std::cout << ' ' << r.message.substr(at);
      std::cout << " OK\n";
    };
    std::cout << "supplier=" << supplier.size() << " nation=" << nation.size()
              << " region=" << region.size()
              << " orders_prefix=" << orders.size() << " buffer_frames=32\n";
    for (const auto* scan : {"table", "index"}) {
      Exec(*db, "PRAGMA scan_method=" + std::string(scan));
      Rows integer, text, date, mixed;
      for (const auto& o : orders) {
        auto row = Join({o[0], o[5], o[4]});
        const auto id = std::stoll(o[0]);
        if (id >= 100 && id <= 1000) integer.push_back(row);
        if (o[5] != "1-URGENT") text.push_back(row);
        if (o[4] >= "1995-01-01" && o[4] < "1996-01-01") date.push_back(row);
        if (id <= 1000 && o[5] != "1-URGENT") mixed.push_back(row);
      }
      const std::string select =
          "SELECT o_orderkey,o_orderpriority,o_orderdate FROM orders WHERE ";
      verify(std::string(scan) + " integer_range",
             select + "o_orderkey BETWEEN 100 AND 1000", integer);
      verify(std::string(scan) + " string_ne",
             select + "o_orderpriority != '1-URGENT'", text);
      verify(std::string(scan) + " date_range",
             select + "o_orderdate>='1995-01-01' AND o_orderdate<'1996-01-01'",
             date);
      verify(std::string(scan) + " mixed_conditions",
             select + "o_orderkey<=1000 AND o_orderpriority!='1-URGENT'",
             mixed);
    }
    Exec(*db, "PRAGMA scan_method=auto");
    std::map<int, std::vector<std::string>> nations, regions;
    for (const auto& n : nation) nations.emplace(std::stoi(n[0]), n);
    for (const auto& r : region) regions.emplace(std::stoi(r[0]), r);
    for (const auto* algorithm :
         {"nested_loop", "sort_merge", "hash", "index"}) {
      Exec(*db, "PRAGMA join_algorithm=" + std::string(algorithm));
      Rows two, three, composite, dates, strings;
      for (const auto& s : supplier) {
        const auto& n = nations.at(std::stoi(s[3]));
        const auto& r = regions.at(std::stoi(n[2]));
        two.push_back(Join({s[0], n[1]}));
        three.push_back(Join({s[0], n[1], r[1]}));
        if (std::stoi(s[0]) <= 100) composite.push_back(Join({s[0], s[1]}));
      }
      for (const auto& a : orders) {
        if (std::stoll(a[0]) > 200) continue;
        for (const auto& b : orders) {
          if (std::stoll(b[0]) > 200) continue;
          if (a[4] == b[4]) dates.push_back(Join({a[0], b[0]}));
          if (a[5] == b[5]) strings.push_back(Join({a[0], b[0]}));
        }
      }
      const auto label = std::string(algorithm) + ' ';
      verify(label + "integer_join",
             "SELECT s.s_suppkey,n.n_name FROM supplier s JOIN nation n ON "
             "s.s_nationkey=n.n_nationkey",
             two);
      verify(label + "three_tables",
             "SELECT s.s_suppkey,n.n_name,r.r_name FROM supplier s,nation "
             "n,region r WHERE s.s_nationkey=n.n_nationkey AND "
             "n.n_regionkey=r.r_regionkey",
             three);
      verify(label + "composite",
             "SELECT a.s_suppkey,b.s_name FROM supplier a,supplier b WHERE "
             "a.s_suppkey=b.s_suppkey AND a.s_name=b.s_name AND "
             "a.s_suppkey<=100 AND b.s_suppkey<=100",
             composite);
      verify(label + "date_join",
             "SELECT a.o_orderkey,b.o_orderkey FROM orders a,orders b WHERE "
             "a.o_orderdate=b.o_orderdate AND a.o_orderkey<=200 AND "
             "b.o_orderkey<=200",
             dates);
      verify(label + "string_join",
             "SELECT a.o_orderkey,b.o_orderkey FROM orders a,orders b WHERE "
             "a.o_orderpriority=b.o_orderpriority AND a.o_orderkey<=200 AND "
             "b.o_orderkey<=200",
             strings);
    }
    for (const auto* algorithm : {"nested_loop", "index"}) {
      Exec(*db, "PRAGMA join_algorithm=" + std::string(algorithm));
      Rows cross, theta;
      for (const auto& n : nation)
        for (const auto& r : region) {
          cross.push_back(Join({n[0], r[0]}));
          if (std::stoi(n[2]) < std::stoi(r[0]))
            theta.push_back(Join({n[0], r[0]}));
        }
      verify(std::string(algorithm) + " cross",
             "SELECT n.n_nationkey,r.r_regionkey FROM nation n CROSS JOIN "
             "region r",
             cross);
      verify(std::string(algorithm) + " theta",
             "SELECT n.n_nationkey,r.r_regionkey FROM nation n,region r WHERE "
             "n.n_regionkey<r.r_regionkey",
             theta);
    }
    const auto stats = db->buffer_pool().Stats();
    std::cout << "checks=" << checks
              << " all_reference_results=OK reopen=OK buffer_miss="
              << stats.miss << " evict=" << stats.evict << '\n';
    Check(db->Close().ok(), "close failed");
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
