#include "execution/spj.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <unordered_map>

#include "common/util.h"

namespace minidb::spj {
namespace {
bool NullKey(const Row& row, const std::vector<EquiKey>& keys, bool left) {
  for (auto k : keys)
    if (row[left ? k.left : k.right].IsNull()) return true;
  return false;
}
int Compare(const Row& a, const Row& b, const std::vector<EquiKey>& keys,
            bool a_left, bool b_left) {
  for (auto k : keys) {
    int cmp = CompareValues(a[a_left ? k.left : k.right],
                            b[b_left ? k.left : k.right]);
    if (cmp) return cmp;
  }
  return 0;
}
size_t Hash(const Row& row, const std::vector<EquiKey>& keys, bool left) {
  size_t hash = 0;
  for (auto k : keys) {
    const auto& v = row[left ? k.left : k.right];
    size_t h;
    if (v.kind == ValueKind::kString)
      h = std::hash<std::string>{}(TrimRight(v.string_value));
    else {
      long double n =
          v.kind == ValueKind::kDouble ? static_cast<long double>(v.double_value)
          : v.kind == ValueKind::kInt32 || v.kind == ValueKind::kDate
              ? static_cast<long double>(v.int32_value)
              : static_cast<long double>(v.int64_value);
      if (v.kind == ValueKind::kDecimal) n /= std::pow(10.0L, v.scale);
      h = std::hash<long double>{}(n);
    }
    hash ^= h + 0x9e3779b9 + (hash << 6) + (hash >> 2);
  }
  return hash;
}
}  // namespace
size_t RowBytes(const Row& row) {
  size_t bytes = sizeof(Row) + row.size() * sizeof(Value);
  for (const auto& v : row) bytes += v.string_value.size();
  return bytes;
}
void NestedLoop(const Rows& left, const Rows& right, const Emit& emit,
                Stats* stats) {
  for (const auto& a : left)
    for (const auto& b : right) {
      ++stats->candidates;
      if (!emit(a, b)) return;
    }
}
void SortMerge(const Rows& left, const Rows& right,
               const std::vector<EquiKey>& keys, const Emit& emit,
               Stats* stats) {
  std::vector<size_t> a, b;
  for (size_t i = 0; i < left.size(); ++i)
    if (!NullKey(left[i], keys, true)) a.push_back(i);
  for (size_t i = 0; i < right.size(); ++i)
    if (!NullKey(right[i], keys, false)) b.push_back(i);
  std::sort(a.begin(), a.end(), [&](size_t x, size_t y) {
    return Compare(left[x], left[y], keys, true, true) < 0;
  });
  std::sort(b.begin(), b.end(), [&](size_t x, size_t y) {
    return Compare(right[x], right[y], keys, false, false) < 0;
  });
  size_t i = 0, j = 0;
  while (i < a.size() && j < b.size()) {
    int cmp = Compare(left[a[i]], right[b[j]], keys, true, false);
    if (cmp < 0) {
      ++i;
      continue;
    }
    if (cmp > 0) {
      ++j;
      continue;
    }
    size_t ie = i + 1, je = j + 1;
    while (ie < a.size() &&
           Compare(left[a[i]], left[a[ie]], keys, true, true) == 0)
      ++ie;
    while (je < b.size() &&
           Compare(right[b[j]], right[b[je]], keys, false, false) == 0)
      ++je;
    for (size_t x = i; x < ie; ++x)
      for (size_t y = j; y < je; ++y) {
        ++stats->candidates;
        if (!emit(left[a[x]], right[b[y]])) return;
      }
    i = ie;
    j = je;
  }
}
void HashJoin(const Rows& left, const Rows& right,
              const std::vector<EquiKey>& keys, const Emit& emit,
              Stats* stats) {
  std::unordered_map<size_t, std::vector<size_t>> buckets;
  for (size_t j = 0; j < right.size(); ++j)
    if (!NullKey(right[j], keys, false))
      buckets[Hash(right[j], keys, false)].push_back(j);
  for (const auto& a : left) {
    if (NullKey(a, keys, true)) continue;
    ++stats->probes;
    auto found = buckets.find(Hash(a, keys, true));
    if (found == buckets.end()) continue;
    for (size_t j : found->second) {
      ++stats->candidates;
      // Hash collisions (including rounded BIGINT hashes) never imply equality.
      if (Compare(a, right[j], keys, true, false) == 0 && !emit(a, right[j]))
        return;
    }
  }
}
}  // namespace minidb::spj
