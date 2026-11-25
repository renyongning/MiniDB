#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>
#include <tuple>
#include <vector>

#include "db/database.h"

using namespace minidb;
namespace {
#define CHECK(x)                                                     \
  do {                                                               \
    if (!(x)) {                                                      \
      std::cerr << __FILE__ << ':' << __LINE__ << " " << #x << '\n'; \
      std::exit(1);                                                  \
    }                                                                \
  } while (false)
QueryResult Exec(Database& db, const std::string& sql) {
  QueryResult r;
  auto s = db.Execute(sql, &r);
  if (!s.ok()) {
    std::cerr << sql.substr(0, 500) << "\n" << s.message() << '\n';
    std::exit(1);
  }
  return r;
}
using ResultRows = std::vector<std::string>;
ResultRows Rows(const QueryResult& r) {
  ResultRows rows;
  for (const auto& row : r.rows) {
    std::string text;
    for (const auto& v : row) text += ValueToString(v) + '|';
    rows.push_back(text);
  }
  std::sort(rows.begin(), rows.end());
  return rows;
}
struct Record {
  int seq, id;
  std::string name, day;
  bool null;
};
bool Test(int cmp, const std::string& op) {
  if (op == "=") return cmp == 0;
  if (op == "!=") return cmp != 0;
  if (op == "<") return cmp < 0;
  if (op == "<=") return cmp <= 0;
  if (op == ">") return cmp > 0;
  return cmp >= 0;
}
int Compare(const Record& a, const Record& b, int column) {
  if (column == 0) return a.id < b.id ? -1 : a.id > b.id ? 1 : 0;
  const auto& x = column == 1 ? a.name : a.day;
  const auto& y = column == 1 ? b.name : b.day;
  return x < y ? -1 : x > y ? 1 : 0;
}
std::string Pair(int a, int b) {
  return std::to_string(a) + '|' + std::to_string(b) + '|';
}
void SetAlgorithm(Database& db, const std::string& name) {
  Exec(db, "PRAGMA join_algorithm = " + name);
}
void All() {
  const auto dir = "/tmp/minidb_spj_" + std::to_string(getpid());
  std::filesystem::remove_all(dir);
  Config config;
  config.path = dir;
  config.buffer_pool_frames = 8;
  config.log_level = "off";
  auto opened = Database::Open(config);
  CHECK(opened.ok());
  auto db = opened.TakeValue();
  Exec(*db,
       "CREATE TABLE a(seq INTEGER, id INTEGER, name VARCHAR(30), day DATE);"
       "CREATE TABLE b(seq INTEGER, id BIGINT, name CHAR(30), day DATE);"
       "CREATE TABLE c(seq INTEGER, id INTEGER, name VARCHAR(30), day DATE)");
  std::vector<Record> a, b, c;
  std::mt19937 random(20261125);
  for (auto item : {std::make_pair("a", &a), std::make_pair("b", &b),
                    std::make_pair("c", &c)}) {
    for (int i = 0; i < 38; ++i) {
      Record r{i, static_cast<int>(random() % 9) - 4,
               "name" + std::to_string(random() % 5),
               "1998-01-0" + std::to_string(1 + random() % 5), i % 17 == 0};
      item.second->push_back(r);
      const auto values =
          r.null ? "NULL,NULL,NULL"
                 : std::to_string(r.id) + ",'" + r.name +
                       (item.first == std::string("b") ? "   " : "") + "','" +
                       r.day + "'";
      Exec(*db, "INSERT INTO " + std::string(item.first) + " VALUES (" +
                    std::to_string(i) + ',' + values + ')');
    }
  }
  // Baseline selection is independently computed for three types and all
  // comparisons.
  const std::vector<std::string> names = {"id", "name", "day"};
  const std::vector<std::string> literals = {"0", "'name2'",
                                             "DATE '1998-01-03'"};
  const Record bound{0, 0, "name2", "1998-01-03", false};
  for (const auto& table : {"a", "b", "c"})
    for (const auto& col : names)
      Exec(*db, "CREATE INDEX " + std::string(table) + '_' + col + " ON " +
                    table + '(' + col + ')');
  for (const auto& scan : {"table", "index"}) {
    Exec(*db, "PRAGMA scan_method = " + std::string(scan));
    for (int col = 0; col < 3; ++col)
      for (const std::string op : {"=", "!=", "<", "<=", ">", ">="}) {
        ResultRows expected;
        for (const auto& r : a)
          if (!r.null && Test(Compare(r, bound, col), op))
            expected.push_back(std::to_string(r.seq) + '|');
        std::sort(expected.begin(), expected.end());
        auto got = Exec(
            *db, "SELECT seq FROM a WHERE " + names[col] + op + literals[col]);
        CHECK(Rows(got) == expected);
        CHECK(got.message.find(scan == std::string("table")
                                   ? "TABLE SCAN"
                                   : "INDEX SCAN") != std::string::npos);
      }
    auto got = Exec(*db,
                    "SELECT id,name,day FROM a WHERE (id BETWEEN -2 AND 2) AND "
                    "name != 'name1'");
    ResultRows expected;
    for (const auto& r : a)
      if (!r.null && r.id >= -2 && r.id <= 2 && r.name != "name1")
        expected.push_back(std::to_string(r.id) + '|' + r.name + '|' + r.day +
                           '|');
    std::sort(expected.begin(), expected.end());
    CHECK(Rows(got) == expected);
    CHECK(Exec(*db, "SELECT * FROM a").rows.size() ==
          a.size());  // Index full scan retains NULL rows.
    CHECK(Exec(*db, "SELECT * FROM a LIMIT 2").rows.size() == 2);
    CHECK(Exec(*db, "SELECT * FROM a LIMIT 0").rows.empty());
  }
  Exec(*db, "PRAGMA scan_method = auto");
  ResultRows between_columns;
  for (const auto& r : a)
    if (!r.null && r.seq >= 0 && r.seq <= r.id)
      between_columns.push_back(std::to_string(r.seq) + '|');
  std::sort(between_columns.begin(), between_columns.end());
  CHECK(Rows(Exec(*db, "SELECT seq FROM a WHERE seq BETWEEN 0 AND id")) ==
        between_columns);
  size_t compared = 0;
  for (const auto& algorithm :
       {"nested_loop", "sort_merge", "hash", "index", "auto"}) {
    SetAlgorithm(*db, algorithm);
    for (int col = 0; col < 3; ++col)
      for (const std::string op : {"=", "!=", "<", "<=", ">", ">="}) {
        if ((std::string(algorithm) == "sort_merge" ||
             std::string(algorithm) == "hash") &&
            op != "=")
          continue;
        ResultRows expected;
        for (const auto& x : a)
          for (const auto& y : b)
            if (!x.null && !y.null && Test(Compare(x, y, col), op))
              expected.push_back(Pair(x.seq, y.seq));
        std::sort(expected.begin(), expected.end());
        const auto predicate = "x." + names[col] + op + "y." + names[col];
        auto got = Exec(
            *db, "SELECT x.seq,y.seq FROM a AS x JOIN b y ON " + predicate);
        CHECK(Rows(got) == expected);
        ++compared;
        // Reversed operands must invert index bounds, not the query semantics.
        auto reversed = Exec(*db, "SELECT x.seq,y.seq FROM a x,b y WHERE y." +
                                      names[col] + op + "x." + names[col]);
        expected.clear();
        for (const auto& x : a)
          for (const auto& y : b)
            if (!x.null && !y.null && Test(Compare(y, x, col), op))
              expected.push_back(Pair(x.seq, y.seq));
        std::sort(expected.begin(), expected.end());
        CHECK(Rows(reversed) == expected);
        ++compared;
      }
    ResultRows expected;
    for (const auto& x : a)
      for (const auto& y : b)
        if (!x.null && !y.null && x.id == y.id && x.name == y.name &&
            x.day <= y.day && x.id >= 0)
          expected.push_back(Pair(x.seq, y.seq));
    std::sort(expected.begin(), expected.end());
    CHECK(Rows(Exec(*db,
                    "SELECT x.seq,y.seq FROM a x JOIN b y ON x.id=y.id AND "
                    "x.name=y.name "
                    "WHERE x.day<=y.day AND x.id>=0")) == expected);
    expected.clear();
    for (const auto& x : a)
      for (const auto& y : b)
        for (const auto& z : c)
          if (!x.null && !y.null && !z.null && x.id == y.id && y.day == z.day &&
              x.name == z.name)
            expected.push_back(Pair(x.seq, y.seq) + std::to_string(z.seq) +
                               '|');
    std::sort(expected.begin(), expected.end());
    CHECK(Rows(Exec(*db,
                    "SELECT x.seq,y.seq,z.seq FROM a x,b y,c z "
                    "WHERE x.id=y.id AND y.day=z.day AND x.name=z.name")) ==
          expected);
    auto limited =
        Exec(*db, "SELECT x.*, y.day FROM a x JOIN b y ON x.id=y.id LIMIT 7");
    CHECK(limited.rows.size() == 7 && limited.columns.size() == 5);
    CHECK(Exec(*db, "SELECT * FROM a x JOIN b y ON x.id=y.id LIMIT 0")
              .rows.empty());
  }
  for (const auto& algorithm : {"nested_loop", "index"}) {
    SetAlgorithm(*db, algorithm);
    ResultRows expected;
    for (const auto& x : a)
      for (const auto& y : b) expected.push_back(Pair(x.seq, y.seq));
    std::sort(expected.begin(), expected.end());
    CHECK(Rows(Exec(*db, "SELECT x.seq,y.seq FROM a x CROSS JOIN b y")) ==
          expected);
    CHECK(Exec(*db, "SELECT * FROM a x,b y LIMIT 5").rows.size() == 5);
  }
  SetAlgorithm(*db, "auto");
  CHECK(Exec(*db, "SELECT x.seq,y.seq FROM a x JOIN a y ON x.seq=y.seq")
            .rows.size() == a.size());
  const auto stats = db->buffer_pool().Stats();
  auto explained =
      Exec(*db, "EXPLAIN SELECT x.id,y.name FROM a x JOIN b y ON x.day=y.day");
  CHECK(!explained.rows.empty());
  CHECK(db->buffer_pool().Stats().hit == stats.hit &&
        db->buffer_pool().Stats().miss == stats.miss);
  for (const auto& bad :
       {"SELECT id FROM a,b", "SELECT * FROM a x,a x", "SELECT q.* FROM a",
        "SELECT * FROM a WHERE missing=1", "SELECT * FROM a,b WHERE a.id=b.day",
        "SELECT * FROM a WHERE id=1 OR id=2", "SELECT * FROM a WHERE id=NULL",
        "SELECT * FROM a WHERE day='not-a-date'", "SELECT * FROM a LIMIT -1"})
    CHECK(!db->Execute(bad, nullptr).ok());
  for (const auto& algorithm : {"sort_merge", "hash"}) {
    SetAlgorithm(*db, algorithm);
    CHECK(!db->Execute("SELECT * FROM a,b", nullptr).ok());
    CHECK(!db->Execute("SELECT * FROM a,b WHERE a.id < b.id", nullptr).ok());
  }
  Exec(*db, "CREATE TABLE empty(id INTEGER); CREATE TABLE noindex(id INTEGER)");
  SetAlgorithm(*db, "index");
  CHECK(!db->Execute("SELECT * FROM a JOIN noindex ON a.id=noindex.id", nullptr)
             .ok());
  SetAlgorithm(*db, "auto");
  CHECK(Exec(*db, "SELECT * FROM a JOIN empty ON a.id=empty.id").rows.empty());
  CHECK(Exec(*db, "SELECT * FROM empty,a").rows.empty());
  Exec(*db, "PRAGMA query_memory=4096");
  auto over = db->Execute("SELECT * FROM a,b", nullptr);
  CHECK(!over.ok() && over.code() == StatusCode::kNoSpace);
  CHECK(Exec(*db, "SELECT * FROM a LIMIT 1").rows.size() == 1);
  Exec(*db, "PRAGMA query_memory=67108864");
  // DATE maintenance and persisted reopen use the same index file format.
  Exec(*db,
       "UPDATE b SET day='2001-01-01' WHERE seq=1; DELETE FROM b WHERE seq=2; "
       "PRAGMA index_check(b_day)");
  auto before = Rows(Exec(*db, "SELECT * FROM b WHERE day='2001-01-01'"));
  CHECK(before.size() == 1);
  CHECK(db->Close().ok());
  db.reset();
  opened = Database::Open(config);
  CHECK(opened.ok());
  db = opened.TakeValue();
  CHECK(Rows(Exec(*db, "SELECT * FROM b WHERE day='2001-01-01'")) == before);
  Exec(*db, "PRAGMA index_check(b_day)");
  // PAD SPACE and duplicate rows must agree across both equijoin
  // implementations.
  Exec(*db,
       "CREATE TABLE pad(k VARCHAR(12)); INSERT INTO pad VALUES ('a'),('a "
       "'),('a'),(NULL)");
  for (const auto& alg : {"nested_loop", "sort_merge", "hash"}) {
    SetAlgorithm(*db, alg);
    CHECK(Exec(*db, "SELECT * FROM pad p,pad q WHERE p.k=q.k").rows.size() ==
          9);
  }
  // BIGINT hashes may collide on arm64 but the residual equality stays exact.
  Exec(*db,
       "CREATE TABLE big(k BIGINT); INSERT INTO big VALUES "
       "(9007199254740992),(9007199254740993)");
  for (const auto& alg : {"nested_loop", "sort_merge", "hash"}) {
    SetAlgorithm(*db, alg);
    CHECK(Exec(*db, "SELECT * FROM big p,big q WHERE p.k=q.k").rows.size() ==
          2);
  }
  CHECK(db->Close().ok());
  db.reset();
  std::filesystem::remove_all(dir);
  std::cout
      << "SPJ: three-type selection/projection, " << compared
      << " oracle join comparisons, composite/three-table/cross, "
         "NULL/duplicates/aliases/LIMIT/EXPLAIN/memory/DATE persistence: OK\n";
}
}  // namespace
int main() { All(); }
