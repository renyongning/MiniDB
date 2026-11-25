# Volcano 执行器与算子状态设计（PG 风格）

> 状态：已实现（2026-11-26）。实现细节与偏差见第 14 节。
> 适用范围：把 `src/db/select.cpp` 中的 SPJ 执行逻辑重构为 Binder/Planner + 火山模型算子树。
> 关系：本文取代 [`experiment3-spj.md`](experiment3-spj.md) 中"选择与投影 / 四种连接 / 内存与范围"的执行部分；
> 实现完成后以代码、`tests/` 与验收脚本为准。

---

## 目录

1. 目标与非目标
2. 总体架构与模块划分
3. 计划表示（Binder/Planner 的产出）
4. 执行状态模型
5. 算子状态设计
6. 状态生命周期与驱动
7. 延迟物化与列裁剪的数据流
8. 内存记账与统计
9. 与 PostgreSQL 执行器的对应关系
10. 兼容性约束
11. 测试方案
12. 实施步骤与文件清单
13. 已知取舍与风险
14. 实现备注

---

## 1. 目标与非目标

**目标**

- 把绑定/规划（表、列、谓词、投影、扫描路径、连接算法）从执行中分离，`ExecuteSelect` 只做编排。
- 用火山模型（Volcano）实现执行：每个算子提供 `Begin` / `Next` / `ReScan` / `End`，向上逐个产出 tuple。
- 参考 PostgreSQL：**计划**（静态、可复用）与**执行状态**（每次查询一个实例，记录扫描位置、游标、构建结果等）分离。
- 延迟物化与列裁剪：扫描只产出满足条件的物理位置（RID）；连接只物化连接键；投影阶段才按 RID 解析 payload。
- 保留现有 SQL 语义、四种连接算法、`EXPLAIN`、统计消息、内存预算与全部测试。

**非目标**

- 不做并行执行、JIT、外部溢写（spill）、代价模型（连接顺序仍等于 `FROM` 顺序）。
- 不做投影/谓词表达式树编译；谓词仍是"列 op 列/常量"的合取式。
- 不改变存储层（TableHeap、BPlusTree、Catalog）的对外接口；只新增一个按列解码函数。

---

## 2. 总体架构与模块划分

```
SQL 文本
  │  src/sql/parser.cpp
  ▼
Statement(SelectSpec)
  │  src/query/select_binder.cpp      ← 步骤 1–7：绑定 + 物理计划
  ▼
SelectPlan（静态计划树）
  │  src/execution/executor.cpp       ← ExecInitNode / ExecStart / ExecRun / ExecEnd
  ▼
PlanState 树（每次查询的执行状态）
  │  Begin / Next / ReScan / End
  ▼
QueryResult
```

| 文件 | 职责 |
| --- | --- |
| `src/query/select_plan.h` | `SelectPlan`、`PlanNode`、绑定结果、`KeyRef`、残余条件等计划契约 |
| `src/query/select_binder.h/.cpp` | 步骤 1–6（表/列/谓词绑定、列裁剪、逐级布局）与步骤 7（扫描路径、连接算法选择） |
| `src/execution/exec_tuple.h` | `RowRef`、`TupleSlot`（物理位置 + 已物化列 + 延迟物化入口） |
| `src/execution/exec_state.h` | `ExecutorState`、`PlanState` 基类、`QueryBudget`、`ExecStats` |
| `src/execution/materialize.h/.cpp` | `TupleMaterializer`：按 RID 取基表行并只解码请求的列 |
| `src/execution/seq_scan.cpp` | `SeqScanState`（heap scan） |
| `src/execution/index_scan.cpp` | `IndexScanState`、`RidLookupState` |
| `src/execution/join.cpp` | `NestLoopState`、`HashJoinState`、`MergeJoinState`、`IndexJoinState` |
| `src/execution/projection.cpp` | `ProjectionState`：投影并物化为内存关系 |
| `src/execution/executor.cpp` | 建状态树、生命周期驱动、结果与消息封装 |
| `src/db/select.cpp` | 薄壳：`SelectBinder` → `Executor` / `EXPLAIN` |

`QueryResult`、C API、`PRAGMA` 与 `Database` 的其它接口不变。旧的 `src/execution/spj.h/.cpp`
（基于 `spj::Row` 的回调式算法）由算子取代，从 CMake 移除。

---

## 3. 计划表示（Binder/Planner 的产出）

### 3.1 绑定结果

```cpp
struct RelationBinding {          // FROM 中的一项（别名可重复指向同一表）
  TableInfo* table;
  std::string alias;
};

struct BoundColumn {              // 扁平列空间中的一列
  size_t relation;                // FROM 序号
  size_t local;                   // 表内列号
  Column column;
  bool rid;                       // 合成 rid 伪列
};

struct BoundPredicate {           // 绑定后的比较
  int left = -1;  Value left_literal;   // -1 表示字面量
  int right = -1; Value right_literal;
  CompareOp op;
  size_t first, last;             // 引用表的最小/最大 FROM 序号
  bool IsJoin() const { return first != last; }
};

struct SelectPlan {
  std::vector<RelationBinding> relations;
  std::vector<BoundColumn> columns;      // 扁平列空间
  std::vector<size_t> projection;        // 扁平列号，保留用户顺序与重复
  std::vector<QueryColumn> output;       // 结果列元数据
  std::unique_ptr<PlanNode> root;        // 根是 Projection
  std::vector<std::string> plan_lines;   // EXPLAIN / 执行消息
  bool explain = false;
  bool detailed = false;                 // 决定 message 形式
  int64_t limit = -1;
  int64_t scan_limit = -1;               // 仅单表查询时下推到扫描节点
  std::string single_index;              // 兼容旧 message 契约
};
```

### 3.2 物理计划节点

```cpp
enum class NodeTag {
  kSeqScan, kIndexScan, kRidLookup,
  kNestLoopJoin, kHashJoin, kMergeJoin, kIndexJoin,
  kProjection,
};

struct KeyRef { uint32_t left_cell; uint32_t right_cell; };

// 负责在 join 层级求值的残余条件：操作数已映射到左右子节点的 cell 下标
struct EvalOperand { int cell = -1; Value literal; };
struct ResidualPredicate {
  EvalOperand left, right;
  CompareOp op;
};

struct PlanNode {
  NodeTag tag;
  std::vector<int> cells;        // 本节点输出 tuple 物化的扁平列（仅连接所需）
  std::string plan_line;         // 对应 EXPLAIN 行

  // ---- 扫描 ----
  size_t relation = 0;           // FROM 序号
  std::vector<BoundPredicate> filters;   // 本地谓词（first==last==t）
  std::vector<int> filter_columns;       // 本地谓词引用的扁平列
  BPlusTree* index = nullptr;
  CompareOp op = CompareOp::kEq; Value bound;
  bool full_index = false;               // PRAGMA scan_method=index 的全索引扫描
  RID rid; bool has_rid = false;         // RID LOOKUP

  // ---- 连接 ----
  std::unique_ptr<PlanNode> left, right; // IndexJoin 只有 left
  std::vector<KeyRef> keys;              // 等值键：左 cells / 右 cells 下标
  std::vector<ResidualPredicate> residual;
  std::vector<std::pair<uint32_t, uint32_t>> left_sel, right_sel;
  size_t inner_relation = 0; size_t indexed_column = 0;
  int probe_cell = -1;                   // IndexJoin 的探测键在左 cells 中的下标
  std::string algorithm;

  // ---- 投影 ----
  struct OutputColumn { uint32_t relation, local; bool rid; };
  std::vector<OutputColumn> output_columns;  // 与 SelectPlan::projection 一一对应
};
```

### 3.3 列裁剪与 cell 布局

Planner 维护扁平列的三张表：

- `last_stage[c]`：引用该列的最后一个连接级（`max(p.last)`，本地谓词为表序号）；
- 每级输入布局 `L_t`、输出布局 `O_t`；
- `cells`：进入 tuple 的列；**投影列不进入任何中间 tuple**。

规则：

1. 扫描节点的 `cells` = 本表参与**连接谓词**（`first < last`）的列；
   本地过滤列只出现在 `filter_columns`，解码后用于过滤，不向上传递。
2. 连接节点 `O_t` = `L_t` 中满足 `last_stage > t` 的列，接上右侧
   （`cells` 中满足 `last_stage > t` 的列）。连接键用完即弃。
3. 投影列由 `ProjectionState` 按 RID 延迟物化，绝不进入 `cells`。
4. 每个节点额外生成 `cell_of_flat: flat -> cell 下标`，供 `Cell()` 与残余条件使用。

因此中间 tuple 只含连接所需的列；RID 是唯一的其余信息。

### 3.4 计划树示例

```sql
SELECT s.s_suppkey, n.n_name
FROM supplier s JOIN nation n ON s.s_nationkey = n.n_nationkey;
```

```
Projection [s_suppkey, n_name]                  ← 按 RID 取 payload
└─ IndexJoin (inner=nation, probe=s_nationkey)
   └─ SeqScan supplier cells=[s_nationkey]
```

三表：

```sql
SELECT s.s_suppkey, n.n_name, r.r_name
FROM supplier s, nation n, region r
WHERE s.s_nationkey = n.n_nationkey AND n.n_regionkey = r.r_regionkey;
```

```
Projection [s_suppkey, n_name, r_name]
└─ IndexJoin (inner=region, probe=n_regionkey)      cells=[]
   └─ IndexJoin (inner=nation, probe=s_nationkey)   cells=[n_regionkey]
      └─ SeqScan supplier cells=[s_nationkey]
```

`n_regionkey` 是唯一被带到第二级的 payload；`s_nationkey`、`n_nationkey` 在第一级
连接后立即丢弃。

---

## 4. 执行状态模型

### 4.1 ExecutorState（对应 PG 的 `EState`）

一次查询执行的全部全局信息都挂在这里：

```cpp
struct ExecutorState {
  const SelectPlan* plan = nullptr;       // 静态计划，生命周期长于本次执行
  Transaction* txn = nullptr;
  QueryBudget budget;                     // 逻辑内存预算（对应 PG MemoryContext 的额度）
  TupleMaterializer materializer;         // 统一的 RID -> 列 解码入口
  std::vector<std::unique_ptr<PlanState>> nodes;  // 初始化顺序，用于统一回收
  PlanState* root = nullptr;
  ExecStats totals;                       // 所有节点累计的 candidates/probes
  uint64_t rows_out = 0;                  // 根节点产出的行数（LIMIT 之后）
  bool explain = false;                   // EXPLAIN：只打印计划，不建状态树
};
```

```cpp
class QueryBudget {
 public:
  explicit QueryBudget(size_t limit);
  Status Reserve(size_t bytes);   // 超限返回 kNoSpace，保留现有错误文本
  void Release(size_t bytes);
  size_t used() const;
  size_t limit() const;
};
```

预算只在 `ExecutorState` 中存在一份，所有算子共享；`Begin`/`Next`/`End`/`ReScan`
的所有记账都走它。

### 4.2 PlanState 基类（对应 PG 的 `PlanState`）

**每个计划节点在每次执行时创建一个 PlanState 实例**；它同时是"算子"和"本次执行的
状态"。上一版方案里的 `Operator` 基类并入此类。

```cpp
enum class NodeTag;  // 见 3.2

struct PlanState {
  NodeTag tag;
  const PlanNode* plan = nullptr;      // 静态计划节点
  PlanState* left = nullptr;           // 子状态（IndexJoin 只有 left）
  PlanState* right = nullptr;
  PlanState* parent = nullptr;
  ExecutorState* estate = nullptr;

  TupleSlot slot;                      // 复用的输出槽（PG: ps_ResultTupleSlot）
  std::vector<int> layout;             // slot.cells[i] 对应的扁平列号
  std::vector<int> cell_of_flat;       // 扁平列 -> cell 下标（-1 = 未物化）
  ExecStats stats;                     // 本节点计数（PG Instrumentation 的简化）

  virtual ~PlanState() = default;
  virtual Status Begin() = 0;          // 只初始化本节点资源，不拉取 tuple
  virtual Status Next(TupleSlot* out, bool* eof) = 0;
  virtual Status ReScan() = 0;         // 重置本节点及必要的子树游标
  virtual Status End() = 0;            // 释放本节点资源（子节点由 executor 统一 End）

  // 已物化的列
  const Value& Cell(const TupleSlot& s, int flat) const {
    return s.cells[cell_of_flat[flat]];
  }
  // 延迟物化单列：优先取 cell，否则按 RID 回表只解码这一列
  Result<Value> ColumnValue(const TupleSlot& s, int flat) const;
  // 按 relation 批量解码（投影用）
  Status MaterializeBatch(const TupleSlot& s, uint32_t relation,
                          const std::vector<size_t>& locals,
                          std::vector<Value>* out) const;
};
```

约定：

- `Next` 返回的 `out` 只在下次调用前有效；算子内部复用 `slot`。
- 所有 `Next` 在 `Begin` 之后调用；`Begin` 只做资源准备（打开迭代器、准备解码计划等），
  不拉取 tuple，也不物化结果。
- `End` 不递归调用子节点；executor 按初始化逆序统一结束。
- 状态对象不跨查询复用；`EXPLAIN` 不创建状态。

### 4.3 TupleSlot 与延迟物化（对应 PG `TupleTableSlot`）

```cpp
struct RowRef {                 // 一个基表行的物理位置
  const TableInfo* table;
  RID rid;
};

struct TupleSlot {              // 只保存物理位置 + 已物化的列
  std::vector<RowRef> rows;     // 按 FROM 序号，每个基表一项
  std::vector<Value> cells;     // 与 PlanState::layout 对应
};
```

物化函数（供投影、残余条件、索引连接调用）：

```cpp
class TupleMaterializer {
 public:
  TupleMaterializer(Transaction* txn, QueryBudget* budget);
  // 取 rows 中第 relation 个基表行，只解码 local_columns（表内列号）
  Status Fetch(const TableInfo& table, RID rid,
               const std::vector<size_t>& local_columns,
               std::vector<Value>* out);
};
```

底层依赖新增的 `Tuple::DecodeColumns(schema, data, length, local_columns)`：只解析
请求的列。VARCHAR 需要按 var offset 前缀推进，但不需要解码整行。

`rid` 伪列不访问堆：由 `RowRef.rid` 经 `FormatRid` 得到。

### 4.4 ExecStats

```cpp
struct ExecStats {
  uint64_t candidates = 0;   // 检查过的候选记录对/索引项
  uint64_t probes = 0;       // 索引探测/散列探测次数
  uint64_t rows = 0;         // 本节点产出的行数（诊断用）
};
```

统计口径与现状一致：`NestLoop` 每个记录对、`IndexJoin` 每个索引候选、`HashJoin`
每个桶内候选、`MergeJoin` 每个组内候选都计 `candidates`；`IndexJoin` 每个左行、
`HashJoin` 每个非 NULL 左行计 `probes`。`ExecutorState::totals` 在执行结束时汇总，
消息格式保持：

```
<plan lines...>
SPJ rows=<N> candidates=<C> probes=<P>
```

---

## 5. 算子状态设计

### 5.1 SeqScanState（heap scan，对应 PG `SeqScanState`）

```cpp
struct SeqScanState : PlanState {
  TableInfo* table = nullptr;
  std::vector<BoundPredicate> filters;   // 本地谓词
  std::vector<int> filter_columns;       // 扁平列
  std::vector<size_t> decode_local;      // 需要解码的表内列（filter ∪ cells）
  std::vector<int> value_of_flat;        // 扁平列 -> decode_local 结果下标
  TableHeap::Iterator iterator;          // 当前扫描位置（PG: ss_currentScanDesc）
  bool started = false;
  int64_t remaining = -1;                // scan_limit（仅单表 LIMIT 下推）
  RID rid;                               // 当前行位置
};
```

| 阶段 | 行为 |
| --- | --- |
| `Begin` | 打开 `heap->Scan(txn)`；计算 `decode_local` / `value_of_flat` |
| `Next` | `iterator.Next(&rid,&tuple)` 循环：`DecodeColumns(decode_local)`；按 `filters` 过滤；通过则填 `rows={{table,rid}}`、按 `cells` 拷贝已解码值；`remaining` 递减 |
| `ReScan` | 重新打开迭代器，重置 `remaining`（供 NestLoop 内表复用） |
| `End` | 释放迭代器 |

无过滤列且 `cells` 为空时（如 CROSS 的纯 RID 扫描）**不解码任何列**。

### 5.2 IndexScanState（对应 PG `IndexScanState`）

```cpp
struct IndexScanState : PlanState {
  TableInfo* table = nullptr;
  BPlusTree* index = nullptr;
  CompareOp op; Value bound;
  bool full_index = false;               // scan_method=index：索引全扫 + NULL 补扫
  std::vector<IndexEntry> entries;       // Begin 时收集（B+ Scan 回调不可挂起）
  size_t entry_pos = 0;                  // 当前索引条目位置
  enum class Phase { kEntries, kNullHeap, kDone } phase = Phase::kEntries;
  TableHeap::Iterator null_iter;         // full_index 的 NULL 键补扫位置
  size_t null_column = 0;                // 索引列（判断 NULL）
  std::vector<BoundPredicate> filters;
  std::vector<size_t> decode_local;      // 残余过滤/cells 需要的列
  std::vector<int> value_of_flat;
  bool need_fetch = false;               // 是否必须回表
  int64_t remaining = -1;
};
```

| 阶段 | 行为 |
| --- | --- |
| `Begin` | `index->Scan(op,bound)` 收集候选 `{key,rid}`（按 `scan_limit` 截断，按条目大小计预算）；`need_fetch = 残余过滤列或 cells 中存在非索引列` |
| `Next` | `kEntries`：逐条产出；索引列比较直接用 `entry.key`，其余过滤/`cells` 需要时 `heap->Get` 并只解码 `decode_local`。`full_index` 时进入 `kNullHeap`：流式扫堆，只解码索引列，NULL 键行通过本地谓词后产出 |
| `ReScan` | `entry_pos=0; phase=kEntries`；必要时重开 NULL 补扫迭代器 |
| `End` | 清空 `entries` |

`entries` 是唯一为"不可挂起的 B+ Scan 回调"付出的内存，按
`sizeof(IndexEntry)+key 字节数` 记账。

### 5.3 RidLookupState

```cpp
struct RidLookupState : PlanState {
  TableInfo* table = nullptr;
  RID rid; bool has_rid = false;
  bool emitted = false;
  std::vector<BoundPredicate> filters;   // 可能含 rid 条件
  std::vector<size_t> decode_local;
  std::vector<int> value_of_flat;
};
```

| 阶段 | 行为 |
| --- | --- |
| `Begin` | 无 |
| `Next` | 首次调用 `heap->Get(txn, rid)`（`kNotFound` 视为空）；解码过滤列；`rid` 操作数用 `FormatRid`；通过本地谓词则产出 1 行，否则 EOF |
| `ReScan` | `emitted=false` |
| `End` | 无 |

### 5.4 JoinState 公共部分

```cpp
struct JoinState : PlanState {
  std::vector<KeyRef> keys;                    // 等值键（cell 下标）
  std::vector<ResidualPredicate> residual;     // 本级残余条件
  std::vector<std::pair<uint32_t, uint32_t>> left_sel, right_sel;

  bool ResidualMatch(const TupleSlot& l, const TupleSlot& r) const;
  void FormJoinedSlot(const TupleSlot& l, const TupleSlot& r, TupleSlot* out) const;
};
```

- `FormJoinedSlot`：`out->rows = l.rows ++ r.rows`；`out->cells` 按 `left_sel` / `right_sel`
  从左右 cells 拷贝。拼接顺序天然保持 FROM 顺序（左子树关系 < 右表）。
- 键比较/散列沿用现有语义：字符串 PAD SPACE（`TrimRight`）、数值统一 `long double`
  比较、DECIMAL 按 scale 归一；散列命中后必须做精确比较。

### 5.5 NestLoopState（对应 PG `NestLoopState`）

```cpp
struct NestLoopState : JoinState {
  TupleSlot outer;              // 当前外表 tuple（PG: nl_ ...）
  bool have_outer = false;
  bool inner_done = false;
};
```

| 阶段 | 行为 |
| --- | --- |
| `Begin` | 无（子节点已 Begin） |
| `Next` | 没有当前外表则从左子拉一行；反复从右子拉一行：右子 EOF 时 `right->ReScan()` 并换下一外表；每对记录 `candidates++`、检查残余条件，通过则 `FormJoinedSlot` 返回 |
| `ReScan` | `have_outer=false; inner_done=false`；左右子 `ReScan` |
| `End` | 无 |

实现 **PG 式重扫**：内表不物化，右子每轮 `ReScan`。右子为空时通过
`inner_done` 标记换外表，保证终止。

### 5.6 HashJoinState（对应 PG `HashJoinState` + `HashState`）

```cpp
struct HashJoinState : JoinState {
  std::vector<TupleSlot> build_rows;                 // 右子（构建侧）slot
  std::unordered_map<size_t, std::vector<uint32_t>> buckets;
  bool built = false;
  TupleSlot outer; bool have_outer = false;
  const std::vector<uint32_t>* bucket = nullptr;
  size_t bucket_pos = 0;
};
```

| 阶段 | 行为 |
| --- | --- |
| `Begin` | 无；构建延迟到首次 `Next`（PG 同样在首次调用时建哈希表） |
| `Next` | 首次：拉取右子直到 EOF，跳过 NULL 键，按 `keys` 计算散列并存入 `build_rows`/`buckets`（按 slot 字节 + 128 B/行桶开销记账）。之后拉左子、跳过 NULL 键、`probes++`；桶内逐项 `candidates++`，精确比较键并检查残余条件，通过则产出 |
| `ReScan` | 只重置探测游标（`have_outer=false; bucket=null`）；构建结果保留 |
| `End` | 释放 `build_rows`/`buckets` |

### 5.7 MergeJoinState（sort-merge，对应 PG `MergeJoinState`）

```cpp
struct MergeJoinState : JoinState {
  std::vector<TupleSlot> left_rows, right_rows;   // 物化的两侧
  std::vector<uint32_t> left_order, right_order;  // 按 key 排序后的下标
  bool sorted = false;
  size_t li = 0, ri = 0;        // 归并位置
  size_t le = 0, re = 0;        // 当前等值组结束
  size_t gx = 0, gy = 0;        // 组内笛卡尔积游标
  bool in_group = false;
};
```

| 阶段 | 行为 |
| --- | --- |
| `Begin` | 无 |
| `Next` | 首次：拉取左右子到 EOF，过滤 NULL 键，按 `keys` 排序下标（按 slot 字节 + 128 B/行排序数组记账）。之后双指针前进；找到等值组 `[li,le) × [ri,re)`，组内逐对 `candidates++`、检查残余条件并产出 |
| `ReScan` | 重置 `li/ri/le/re/gx/gy/in_group`；已物化数据保留 |
| `End` | 释放两侧物化数据 |

### 5.8 IndexJoinState（对应 PG 的索引嵌套循环连接）

```cpp
struct IndexJoinState : JoinState {
  TableInfo* inner = nullptr;
  BPlusTree* index = nullptr;
  size_t indexed_column = 0;
  CompareOp op = CompareOp::kEq;     // 内表键 op 外表值
  int probe_cell = -1;               // 左 cells 中的探测键；-1 = CROSS
  std::vector<BoundPredicate> inner_filters;
  std::vector<size_t> inner_decode_local;   // 内表过滤/cells 需要的列
  std::vector<int> inner_value_of_local;    // 表内列 -> 解码结果下标
  std::vector<TupleSlot> null_rows;         // CROSS：索引不存的 NULL 键行
  bool nulls_built = false;
  TupleSlot outer; bool have_outer = false;
  std::vector<TupleSlot> pending;           // 当前外行命中的内表行
  size_t pending_pos = 0;
};
```

| 阶段 | 行为 |
| --- | --- |
| `Begin` | 无；CROSS 的 `null_rows` 延迟到首次 `Next` 构建 |
| `Next` | 有待产出命中则继续；否则拉下一左行（`probes++`），用 `Cell(probe_cell)` 或 `Minimum(type)` 做 `index->Scan`；每个候选 `candidates++`：仅当需要过滤/cells 时回表并批量解码；应用内表本地谓词与残余条件；命中放入 `pending`。CROSS 时追加 `null_rows` 的匹配。最终 `FormJoinedSlot` 产出 |
| `ReScan` | 重置 `have_outer/pending`；`null_rows` 保留；左子 `ReScan` |
| `End` | 释放 `null_rows`/`pending` |

当内表 `cells` 为空且无残余条件时（如两表只投影 `n_name`），**不回表**，
只返回内表 RID；payload 由投影阶段解析。

### 5.9 ProjectionState

```cpp
struct ProjectionState : PlanState {
  struct Use { size_t value_index; size_t position; };
  struct Fetch {                      // 按基表分组的解码计划
    size_t relation;
    std::vector<size_t> locals;       // 去重后的表内列
    std::vector<Use> uses;            // 解码值 -> 输出列位置
  };
  std::vector<PlanNode::OutputColumn> columns;
  std::vector<Fetch> fetches;
  int64_t remaining = -1;             // LIMIT
};
```

| 阶段 | 行为 |
| --- | --- |
| `Begin` | 只准备按 relation 分组的解码计划与 LIMIT 计数；不拉取 tuple，不缓冲结果 |
| `Next` | 拉取一个子 tuple，用 `rows[relation]` 的 RID 回表并 `DecodeColumns(locals)`；`rid` 列直接格式化；按投影顺序返回一行；达到 LIMIT 即 EOF |
| `ReScan` | 重置 LIMIT 计数，子节点 `ReScan` |
| `End` | 无内部状态可释放 |

投影是唯一把 payload 物化的算子，也是 LIMIT 的最终执行点；`rows_out` 由 executor 统计。
结果行由 executor 逐行移入 `QueryResult`，**不参与 `query_memory` 记账**：这与 PG 把结果
发送给 `DestReceiver` 而不是当作算子内部状态一致，避免"结果集本身大于预算"导致简单查询失败。

---

## 6. 状态生命周期与驱动

### 6.1 ExecInitNode

```cpp
Status ExecInitNode(const PlanNode* plan, ExecutorState* estate,
                    PlanState* parent, PlanState** out);
```

- 按 `NodeTag` 创建对应状态对象，从 `PlanNode` 拷贝静态配置
  （`cells`、`filters`、`keys`、`residual`、选择器、索引、边界值等），
  计算 `layout` / `cell_of_flat`；
- 递归左子（`IndexJoin` 只有左子）；
- 把状态登记到 `estate->nodes`（持有所有权，初始化顺序即遍历顺序）；
- 根节点回填 `estate->root`。

### 6.2 ExecStart（Begin）

按**后序**（子先父后）对每个状态调用 `Begin()`：

1. 扫描状态打开迭代器/收集索引条目；
2. 连接状态准备游标（不构建）；
3. 投影状态最后 Begin，驱动子节点完成物化。

后序保证任何 `Begin` 中都不会访问尚未 Begin 的子状态。

### 6.3 ExecRun（Next）

```cpp
Status Executor::Run(QueryResult* result) {
  result->columns = plan_->output;
  Status s = Start();
  if (!s.ok()) { End(); return s; }
  for (;;) {
    TupleSlot slot;
    bool eof = false;
    s = estate_->root->Next(&slot, &eof);
    if (!s.ok()) { End(); return s; }
    if (eof) break;
    estate_->rows_out++;
    result->rows.push_back(std::move(slot.cells));   // 根是投影：cells 即结果行
  }
  s = End();
  if (!s.ok()) return s;
  FillMessage(result);
  return Status::Ok();
}
```

`EXPLAIN` 不走这里：`ExecuteSelect` 直接把 `plan_lines` 填进结果。

### 6.4 ExecReScan

```cpp
Status PlanState::ReScan();  // 递归重置
```

- 扫描：重开迭代器/索引游标；
- 连接：重置探测/归并游标；哈希/排序构建结果保留；
- 投影：重置 LIMIT 计数；
- 唯一调用方是 `NestLoopState`（内表重扫）以及可能的嵌套重扫。

### 6.5 ExecEnd

按 `estate->nodes` 的**逆序**（叶子先于父）调用 `End()`，释放：
迭代器、索引 RID 列表、构建侧 slot、散列桶、排序数组、NULL 桶。
预算按节点释放；即使执行中途失败，`Executor::Run` 也保证调用 `End()`。

### 6.6 错误处理约定

- 所有接口返回 `Status`；任何一步失败立即停止拉取，`End()` 清理后原样返回；
- `kNoSpace` 保留现有错误文本（`SPJ query memory budget exceeded; ...`）；
- `heap->Get` 的 `kNotFound` 在 RID 查找中视为"无此行"，其余错误上抛；
- 索引条目失效/损坏按存储层返回的错误上抛，不静默跳过。

---

## 7. 延迟物化与列裁剪的数据流

```
                 RID + join keys            RID + join keys          值
HeapScan ──► Join ──────────────► Join ──────────────► Projection ──► QueryResult
  (只解码过滤列)   (只物化 key)        (用完即弃 key)      (按 RID 取 payload)
```

- **扫描**：只定位满足本地谓词的 RID；解码范围 = 本地谓词列 ∪ 需要向上的连接键。
- **连接**：只物化等值键和本级残余条件列；输出 tuple 只保留后续仍需要的列。
- **投影**：按 `(relation, rid)` 回表，按基表分组一次取齐该表所有投影列。
- 每个中间 tuple 的大小与"连接涉及的列数"成正比，而不是表宽；对于
  `supplier ⋈ nation` 的 10 万行结果，中间 tuple 只含 RID 与 1–2 个键列。

延迟物化的代价是投影阶段每个结果行、每个基表一次 `heap->Get`（缓冲池命中时是
内存拷贝 + 部分解码），这是用 I/O/CPU 换内存的有意取舍。

---

## 8. 内存记账与统计

| 分配点 | 记账方式 | 释放点 |
| --- | --- | --- |
| 扫描瞬时解码 | 不记账（生命期一个 tuple，对应 PG per-tuple context） | `Next` 返回前 |
| 索引 RID 列表 `rids` | `sizeof(RID)` 每项（不需要 key，过滤与 cells 都从基表列解码） | `End` / `ReScan` 保留 |
| NestLoop 内表 | 不物化 | — |
| Hash 构建侧 | `SlotBytes` + 128 B/行桶开销 | `End` |
| SortMerge 两侧 | `SlotBytes` + 128 B/行排序数组 | `End` |
| IndexJoin NULL 桶/待产出命中 | `SlotBytes` | `End` |
| 结果集 | **不记账**：由 `QueryResult` 承接（PG `DestReceiver` 类比），流式投影不缓冲整个关系 | — |

`SlotBytes(slot) = sizeof(TupleSlot) + rows*sizeof(RowRef)
+ cells*sizeof(Value) + 字符串字节`。

统计口径见 4.4；`EXPLAIN` 与执行消息的格式不变：

- 单表、`auto` 扫描、无多谓词：`message` 为空或 `INDEX SCAN <name>`（旧契约）；
- 其余：逐行 `plan_lines`，加 `SPJ rows=N candidates=C probes=P`。

---

## 9. 与 PostgreSQL 执行器的对应关系

| PostgreSQL | 本设计 | 说明 |
| --- | --- | --- |
| `PlannedStmt` | `SelectPlan` | 静态计划，可重复执行 |
| `EState` | `ExecutorState` | 查询级全局状态 |
| `PlanState` | `PlanState` | 计划节点 + 执行状态合一 |
| `ExecInitNode` | `Executor::ExecInitNode` | 建状态树 |
| `ExecProcNode` | `PlanState::Next` | 火山调用 |
| `ExecReScan` | `PlanState::ReScan` | NestLoop 内表重扫 |
| `ExecEndNode` | `PlanState::End` | 逆序释放 |
| `SeqScanState`（`ss_currentScanDesc`） | `SeqScanState.iterator` | 当前扫描位置 |
| `IndexScanState` | `IndexScanState.entries/entry_pos` | 索引游标（回调不可挂起，故预收集） |
| `NestLoopState` | `NestLoopState.outer/have_outer` | 外表与重扫 |
| `HashJoinState` / `HashState` | `HashJoinState.build_rows/buckets` | 构建侧与桶 |
| `MergeJoinState` | `MergeJoinState.left/right_order/li/ri` | 等值组归并 |
| `TupleTableSlot` | `TupleSlot` | RID + 已物化列 |
| `MemoryContext` | `QueryBudget` | 逻辑额度，不是 RSS 硬上限 |
| `Instrumentation` | `ExecStats` | candidates/probes/rows |

差异：无表达式编译（`ExecQual` 简化为列比较）、无并行、无外部溢写、
无 bitmap/仅索引扫描、索引游标通过预收集模拟。

---

## 10. 兼容性约束

- `Database::Execute` / `QueryResult` / C API / `PRAGMA` 接口不变；
- 四种连接算法与 `auto` 的选择规则不变；`PRAGMA scan_method=table/index` 语义不变；
- 结果列命名（多表 `alias.column`）、重复行、NULL 比较、字符串 PAD SPACE、
  日期/整数/字符串类型规则不变；
- `rid` 伪列、`RID LOOKUP`、CROSS + NULL 键、复合键、自连接、三表左深顺序不变；
- `LIMIT`：单表下推到扫描；多表由投影停止拉取，不截断中间级；
- 错误文本不变；预算现在只覆盖算子内部状态（哈希/排序构建侧、索引 RID 列表、
  索引连接 NULL 桶），结果集不计费：`query_memory=4096` 时哈希等值连接仍因构建侧
  超限失败，而流式扫描、嵌套循环与 `LIMIT 1` 正常返回；
- `EXPLAIN` 行与 `SPJ rows=… candidates=… probes=…` 统计格式不变。

---

## 11. 测试方案

1. `ctest`：integration / storage / index / spj 四个套件全部通过。
2. `minidb_verify_spj_tpch test/tpc-h/data/sf10 <新目录> 50000`：32 项真实数据核对
   （选择、四种连接、三表、复合键、非等值、CROSS、重开），SF10 十万行 supplier
   在默认 64 MiB 预算内完成。
3. `scripts/demo_spj_small.sql` 与 `scripts/demo_spj.txt` 无错误。
4. 专项回归：
   - `EXPLAIN` 只打印计划、不读表；
   - `LIMIT 0/1` 与小预算组合；
   - CROSS IndexJoin 的 NULL 键补扫；
   - 自连接（同一 `TableInfo` 出现两次时 `rows` 按 FROM 序号区分）；
   - 复合键的残余条件与仅索引列可跳过的回表；
   - 投影重复列与 `rid` 列；
   - 四种算法结果与参考模型一致（排序比较），统计值按现有消息检查。
5. 新增单元测试：`Tuple::DecodeColumns`（定长/变长/NULL/越界）、`QueryBudget`
   的 Reserve/Release，以及各算子的 `Begin/Next/ReScan/End` 幂等性。

---

## 12. 实施步骤与文件清单

1. `src/record/tuple.h/.cpp`：新增 `Tuple::DecodeColumns`。
2. `src/query/select_plan.h`、`select_binder.h/.cpp`：迁移绑定、裁剪、布局、
   扫描/连接选择；`src/db/select.cpp` 变薄壳。
3. `src/execution/exec_tuple.h`、`exec_state.h`、`materialize.*`：
   `TupleSlot`、`PlanState`、`ExecutorState`、`QueryBudget`、`ExecStats`。
4. `seq_scan.cpp`、`index_scan.cpp`：三个扫描算子。
5. `join.cpp`：`IndexJoin`（先保验收）→ `NestLoop` → `HashJoin` → `MergeJoin`。
6. `projection.cpp`、`executor.cpp`：投影物化、生命周期、消息与 `EXPLAIN`。
7. `CMakeLists.txt`：加入新源文件，移除 `src/execution/spj.cpp`。
8. 回归并更新 `doc/experiment3-spj.md` 的架构描述与源码导航。

建议按"先搬运后替换"的节奏：第 2 步完成后先用旧执行路径跑通全部测试，
再逐类替换算子，便于定位语义差异。

---

## 13. 已知取舍与风险

| 风险 | 说明与对策 |
| --- | --- |
| 延迟物化的回表成本 | 每个结果行按基表回表一次；缓冲池命中时开销可接受。必要时给投影加"同 RID/同页"缓存，或对小表把投影列并入 cells（需重新评估内存预算）。 |
| B+ `Scan` 回调不可挂起 | `IndexScanState` 在 `Begin` 只预收集 RID（`sizeof(RID)`/项），不保存 key；`IndexJoin` 每次探测在回调内收集当前外行的命中。 |
| NestLoop 重扫正确性 | 内表为空、外表为空、残余条件与 NULL 组合需专项测试；`inner_done` 标记保证终止。 |
| 排序归并组内笛卡尔积 | 重复键组必须输出完整乘积；游标 `gx/gy` 的状态机需覆盖组切换与 EOF。 |
| 预算语义 | 结果集不计费（与 PG `DestReceiver` 一致）；预算只约束算子内部状态。`query_memory=4096` 的断言改为验证哈希构建侧超限。 |
| 索引 RID 内存 | 全索引扫描（`scan_method=index`）把匹配 RID 收进内存；每项 8 B，75 万行约 6 MB，`scan_limit`/预算仍可兜底。 |
| 消息兼容 | 计划行、`INDEX JOIN` 形式与统计口径必须逐字保留；用现有测试与验收脚本锁住。 |
| 物化行的 `Value` 开销 | `sizeof(Value)=64` 的估算沿用现状；投影行字节数沿用 `RowBytes` 口径，避免预算行为跳变。 |


---

## 14. 实现备注

本节记录落地代码与前述设计的对应关系及少量差异，便于报告与答辩时对照。

**最终文件**

| 文件 | 内容 |
| --- | --- |
| `src/query/select_plan.h` | 绑定结果、`PlanNode`、`KeyRef`、`ResidualPredicate` 等计划契约 |
| `src/query/select_binder.h/.cpp` | 步骤 1–7：绑定、列裁剪、逐级布局、扫描/连接算法选择、EXPLAIN 计划行 |
| `src/db/select.cpp` | 薄入口：`SelectBinder` + `Executor`，EXPLAIN 直接输出计划 |
| `src/execution/exec_tuple.h` | `RowRef`、`TupleSlot`、`RowBytes/SlotBytes` |
| `src/execution/exec_state.h/.cpp` | `ExecutorState`、`PlanState`、`QueryBudget`、`ExecStats`、`TupleMaterializer` |
| `src/execution/scan_state.h/.cpp` | `SeqScanState`、`IndexScanState`、`RidLookupState` |
| `src/execution/join_state.h/.cpp` | `NestLoopState`、`HashJoinState`、`MergeJoinState`、`IndexJoinState` |
| `src/execution/projection_state.h/.cpp` | `ProjectionState`：物化内存关系、LIMIT |
| `src/execution/executor.h/.cpp` | `ExecInitNode` / `ExecStart`（后序 Begin）/ 结果循环 / 逆序 End |
| `src/record/tuple.h/.cpp` | 新增 `Tuple::DecodeColumns` |
| `src/execution/spj.h/.cpp` | 已删除；旧的回调式算法由上述算子取代 |

**与设计的差异**

1. `EvalOperand` 额外携带 `left_side`，残余条件据此选择左/右子 slot 的 cell。
2. `HashJoinState`、`MergeJoinState` 在输出 EOF 时立即释放构建侧/排序数据并置 `finished`，避免左深连接树把各级物化数据持有到 `End()`；`ReScan` 会重新构建。
3. `IndexScanState` 在 `Begin` 只收集 RID（不保存 `IndexEntry` 的 key），按 `sizeof(RID)` `Reserve`；预算失败会中止收集并返回 `kNoSpace`，扫描到 `scan_limit` 时干净停止。
4. `ProjectionState` 流式产出结果：按 relation 分组去重局部列，重复投影列共享一次回表解码；结果行直接交给 `QueryResult`，不缓冲、不计入 `query_memory`。
5. `NestLoopState` 使用内表 `ReScan`（不做内表物化）；三个扫描算子都实现 `ReScan`。
6. `IndexJoinState` 的 CROSS NULL 桶在首个外表行到达时构建一次；普通探测只在内表需要过滤/cells 时回表。

**验证结果（2026-11-26）**

- `ctest` 4/4 通过（integration / storage / index / spj）。
- `minidb_verify_spj_tpch` 使用 SF10 数据（supplier 100,000 行）的 32 项核对全部通过；四种连接算法、三表、复合键、非等值、CROSS、重开均一致。
- 默认 64 MiB 预算下，`SELECT s.s_suppkey, n.n_name FROM supplier s JOIN nation n ON s.s_nationkey = n.n_nationkey` 返回 100,000 行；三表版本同样通过。
- SF0.5 全量 orders（750,000 行）上，`SELECT o_orderkey,o_orderpriority,o_orderdate FROM orders WHERE o_orderpriority != '1-URGENT'` 在 `scan_method=table` 与 `index` 两种路径下均以默认预算返回 599,586 行。
- `scripts/demo_spj_small.sql` 与 `scripts/demo_spj.txt` 无错误。
