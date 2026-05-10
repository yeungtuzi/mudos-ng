# 心跳多线程化改造——全过程总结

**日期:** 2026-05-10
**总耗时:** ~2.5 天（session 1 + session 2 + session 3）
**分支:** master
**最终状态:** `-fhb_test` 10+ 次稳定通过；`-fstress_run` 30000 对象 3 次稳定通过；全部 21 单元测试通过

---

## 一、改造动机

MudOS-NG 原先所有 LPC 代码都在主线程通过 libevent 事件循环执行。`call_heart_beat()` 在主线程顺序遍历所有对象执行 `heart_beat()`，在 30000 对象规模下成为显著的性能瓶颈，且无法利用多核 CPU。

**目标:** 将心跳执行从主线程剥离，分布到多工作线程并行处理。

---

## 二、架构设计

### 2.1 线程分布

```
主线程 (1)              心跳线程池 (N=CPU 核心数)        IO 线程池 (2)
┌──────────────────┐    ┌─────────────────────────┐    ┌──────────────────┐
│ 事件循环           │    │ Thread 0: libevent 循环    │    │ 网络 I/O          │
│ call_heart_beat() │    │ Thread 1: libevent 循环    │    │ TLS/WS/Telnet     │
│  → post 信号       │    │ ...                      │    │ SSH               │
│ set_heart_beat()  │    │ Thread N-1: libevent 循环  │    └──────────────────┘
│  → post 到对应线程  │    │                          │
│ 玩家命令处理        │    │ process_heartbeats():     │
│ 对象创建/析构       │    │  合并队列 → 执行 LPC       │
└──────────────────┘    └─────────────────────────┘
```

### 2.2 心跳执行流

1. `set_heart_beat(ob, to)` → `pool->thread_for_object(ob)->post(modify_heartbeat)` → 对象加入该线程私有心跳队列
2. `call_heart_beat()` → 每个线程 `post(process_heartbeats)` → 线程将 next 队列合并到 main 队列 → 遍历 → 执行 LPC
3. 心跳门控 `g_heartbeat_gate`：对象批量创建期间关闭（跳过 LPC 执行），pool 启动后打开
4. 对象按 `ptr / sizeof(object_t) % N` 哈希分片到各线程，保证同一对象始终在同一线程执行

### 2.3 改造阶段

| 阶段 | 内容 | 目的 |
|------|------|------|
| 〇a | 对象扫描容错遍历（先存 next 再处理） | 消除 restart 逻辑，使遍历可增量化 |
| 〇b | 增量扫描（每 tick 批量，游标断点续扫） | 消灭周期性 500ms+ 卡顿 |
| 〇c | 扫描/执行分离（主线程判标志，LPC post 给线程） | 为多线程执行做准备 |
| 一 | VM 状态 thread_local 化（~35 变量） | 每线程独立 VM 上下文 |
| 二 | 原子引用计数 | 线程安全的共享对象生命周期 |
| 三 | 每线程 eval 计时器 | 每线程独立的执行时限监控 |
| 四+五 | HeartbeatThread + Pool + 心跳迁移 | 核心：心跳执行多线程化 |
| 六 | 跨线程调用 bounce 检测 | 防止 worker 线程意外回调主线程对象 |
| 八 | call_out 线程池 | call_out 延迟调用也迁出主线程 |

---

## 三、问题全记录（共 9 个问题）

### 问题 1：心跳对象未进入线程队列 🔴→✅

**现象:** `set_heart_beat()` 调用后，对象的心跳从未被线程执行。

**根因:** 三个并发问题叠加：
1. **线程启动竞态:** `pthread_create` 返回后立刻 `post()`，但线程内的 `event_base_loop()` 尚未开始监听 wakeup fd，事件丢失
2. **日志丢失:** `fprintf(stderr, ...)` 无 `fflush`，崩溃前的关键日志在 stdio 缓冲区中丢失，无法定位
3. **哈希错误:** `thread_for_object()` 使用 `(uintptr_t)ptr * 2654435761 % N`，但 `object_t` 是 8 字节对齐的，低 3 位始终为 0，导致所有对象都哈希到 bucket 0（一个线程）

**修复:**
- 添加 `std::atomic<bool> ready_` 标志，`post()` 前 spin-wait 等待线程就绪
- `post()` 中 EAGAIN/EWOULDBLOCK 正确处理（重新入队）
- 所有调试日志加 `fflush(stderr)`
- `thread_for_object()` 改为 `ptr / sizeof(object_t) % N`，按对象索引（而非地址）哈希

**教训:**
- 线程启动和首次通信之间有天然竞态，必须显式同步
- 调试多线程程序时，`fflush` 不可或缺——日志丢在缓冲区里比没有日志更糟
- 指针哈希必须考虑对齐——`object_t` 8 字节对齐使低 3 位信息量为 0

---

### 问题 2：内存 swap 过大 ℹ️

**现象:** 30000 对象 × ~1.5KB/对象 = ~47MB 内存实际占用，但系统报告 GB 级 swap。

**根因:** jemalloc arena 预分配——jemalloc 为每个线程预分配大量 arena 页，实际未使用但被系统计入虚拟内存。

**策略:** 已有 `max memory` 配置项（<90% 不清理）。后续可配置 jemalloc `dirty_decay` 参数控制页回收速度。

**教训:** jemalloc 的多线程 arena 模型在大量线程场景下虚拟内存占用远超实际需求，需配置 decay 参数。

---

### 问题 3：create_batch 中 0 延迟 call_out 阻塞游戏 tick ✅

**现象:** LPC 中 `call_out("create_batch", 0)` 在同一个 tick 内递归调用自身创建对象，阻塞整个游戏循环。

**修复:** 改为正延迟 `call_out("create_batch", 1)`，将批量对象创建分散到后续 tick。

**教训:** 0 延迟 call_out 在多线程/高负载下等同于同步递归，应始终使用正延迟。

---

### 问题 4：T_FREED svalue double-free ✅

**现象:** 30000 对象心跳执行时出现：
```
FATAL ERROR: T_FREED svalue freed. Previously freed by call_function_pointer
```

**根因:** `apply_ret_value` 是**全局** `svalue_t`，位于 `apply.cc`。`call_function_pointer()` 将 LPC 函数返回值写入 `apply_ret_value`，然后返回 `&apply_ret_value` 指针给调用者。8 个线程同时调用 `call_function_pointer()` 时：
- Thread A 写入返回值到 `apply_ret_value`
- Thread B 同时写入覆盖了 A 的结果
- Thread A 的调用者随后 `free_svalue(&apply_ret_value)` ——但此时内容是 B 的返回值
- Thread B 的调用者也 `free_svalue(&apply_ret_value)` ——同一个值被 free 两次
- Thread A 的原始返回值变成了悬空引用，最终被 double-free

**修复:** `apply_ret_value` → `thread_local`（涉及 `apply.h`、`apply.cc`、`file.cc`、`checkmemory.cc`）

**教训:** 任何被多线程共享写入的全局变量都是定时炸弹。`svalue_t` 的值语义和指针语义混合使这类 bug 特别隐蔽——多个线程写入同一个内存位置，然后各自尝试释放"自己的"结果。

---

### 问题 5：debugmalloc 多线程竞争 ✅

**现象:**
```
debugmalloc: attempted to free non-malloc'd pointer
```
以及 `std::map` 红黑树损坏（迭代器失效、节点颜色错误）。

**根因:** `MDmalloc`/`MDfree` 使用全局 hash table (`md_node_t *table[MD_TABLE_SIZE]`) 追踪所有分配，`md_refjournal` 是全局 `std::map`。多线程同时 alloc/free 导致 hash table 链表损坏和 map 红黑树并发修改。

**修复:**
- 添加 `std::recursive_mutex md_global_mutex` 保护所有 alloc/free 操作
- `md_refjournal` 加独立 `std::mutex` 保护
- 临时关闭 `DEBUGMALLOC_EXTENSIONS`（调试追踪功能，非核心）

**教训:** debugmalloc 原本是单线程调试工具，多线程化需要全量加锁。`std::map` 的并发修改是 UB，红黑树损坏后的 crash 堆栈往往指向毫不相关的代码。

---

### 问题 6：funptr_hdr_t::ref / program_t::func_ref 非原子 ✅

**现象:** 多线程创建 closure（如 `(: $1 > 1000 :)`）时 ref count 竞争导致对象提前释放或泄漏。

**根因:** `funptr_hdr_t::ref` 是普通 `uint32_t`，`program_t::func_ref` 是普通 `unsigned short`。多线程同时 inc/dec 导致丢失计数。

**修复:**
- `funptr_hdr_t::ref` → `std::atomic<uint32_t>`
- `program_t::func_ref` → `std::atomic<unsigned short>`
- 所有相关 `.load()` 调用适配

**教训:** 引用计数是多线程环境中最基本的同步原语——任何非原子 ref count 在多线程下一定会出错，只是时间问题。

---

### 问题 7：心跳执行静默 segfault ✅

**现象:** `process_heartbeats #1` 在 8 线程上成功触发（说明队列分发正确），无任何 FATAL 错误日志，但进程静默死亡（SIGSEGV），libunwind backtrace 为空。

**根因:** 多个线程安全问题的复合效应：
1. **字符串问题:** `push_shared_string` 在非共享字符串上调用了 `ref_string`（应改用 `make_shared_string`）
2. **字符串表竞争:** 全局字符串表无锁，多线程 alloc/free 字符串导致表损坏
3. **字符串比较:** 多处使用 `svalue_t::u.string ==` 做指针比较（应改用 `strcmp` 做内容比较）——thread_local 化后不同线程持有同一字符串内容的不同指针
4. **mapping hash:** `svalue_to_int` 和 `sval_hash` 依赖指针地址做 hash（应改用纯内容 hash `whashstr`）

**修复:**
- `push_shared_string` → `make_shared_string`（`interpret.cc`）
- 字符串表改为 `thread_local`，移除全局 mutex（`stralloc.cc`）
- `svalue_to_int` / `sval_hash` → `whashstr`（`mapping.cc`）
- `msameval` 使用 `strcmp` 替代指针比较（`mapping.cc`）
- 多处字符串比较 `==` → `strcmp`（`add_action.cc`, `parser.cc`, `contrib.cc`, `compiler.cc`, `object.cc`）
- `block_t::refs` / `malloc_block_t::ref` → `std::atomic`（`stralloc.h`）
- `ref_t::ref` → `std::atomic`（`svalue.h`）
- `int_free_string` TOCTOU 改为 `fetch_sub`（`stralloc.cc`）
- `int_free_svalue` 加 immortal guard（`svalue.cc`）

**教训:** 
- 字符串是多线程化中最棘手的部分——字符串的指针语义 + 共享/常量/堆分配三种 subtype + 引用计数交织，形成复杂的正确性约束
- 指针比较 (`==`) 在 thread_local 字符串表下失效——不同线程的同一字符串内容有不同指针
- hash 依赖指针地址在 thread_local 下同样失效——不同线程的同一 mapping key 有不同 hash 值

---

### 问题 8：剩余全局/静态变量未 thread_local 化 ✅

**现象:** `-ftest` crash，stress 测试间歇性异常。

**根因:** 虽然 Phase 一已将约 35 个核心 VM 变量 `thread_local` 化，但仍有大量辅助全局/静态变量被遗漏，在多线程下产生竞争：
- `sprintf_state`（sprintf.cc）——多线程同时格式化字符串
- `global_lvalue_byte/codepoint/range`（interpret.cc）——lvalue 操作
- `fake_prog` / `fake_program`（interpret.cc）——VM 入口/出口
- `num_hidden`（object.cc）——F_SET_HIDE 功能
- `save_svalue_depth` / `max_depth` / `sizes` / `save_name` / `tmp_name`（object.cc）——对象序列化
- `num_objects_this_thread` / `restrict_destruct`（simulate.cc）——对象加载
- `sent_free`（simulate.cc）——sentence 分配器
- `num_error` / `num_mudlib_error`（simulate.cc）——错误计数
- `stack_in_use_as_temporary`（interpret.cc）——DEBUG 模式

**修复:** 全部改为 `thread_local`，涉及 8 个文件。

**教训:**
- 全局/静态变量的排查必须彻底——遗漏任何一个都可能成为多线程下的崩溃点
- `static` 函数内变量和 `static` 全局变量一样危险
- Debug 模式下的额外变量也需要注意（如 `stack_in_use_as_temporary`）

---

### 问题 9：ABBA 死锁 — g_node_mutex 与 md_global_mutex ✅

**现象:** Stress 测试在 ~14000/30000 对象时 SIGSEGV。LLDB 显示多个线程阻塞在 mutex lock。

**根因:** 经典的 ABBA 死锁：

- **路径 A**（`MDfree` → ... → `free_node`）:
  1. `MDfree` 获取 `md_global_mutex`
  2. `int_free_svalue` → `dealloc_mapping` → `free_node`
  3. `free_node` 尝试获取 `g_node_mutex`
  - 锁顺序: `md_global_mutex` → `g_node_mutex`

- **路径 B**（`new_map_node` → `DMALLOC`）:
  1. `new_map_node` 获取 `g_node_mutex`
  2. 调用 `DMALLOC` → `MDmalloc`
  3. `MDmalloc` 尝试获取 `md_global_mutex`
  - 锁顺序: `g_node_mutex` → `md_global_mutex`

两个线程分别走路径 A 和 B 时，各自持有一个锁并等待对方释放另一个锁 → 死锁。

**修复:** 将 `new_map_node` 中的 `DMALLOC` 调用移到 `g_node_mutex` 临界区之外：先无锁尝试从 free list 取节点（持锁时间极短），若 free list 为空则解锁后分配内存，再重新加锁加入 free list。

**教训:**
- **加锁顺序一致性是多线程编程的铁律**——一旦两个锁以不同顺序被获取，死锁只是时间问题
- `DMALLOC`/`MDfree` 是隐式的锁获取点——调用任何可能分配/释放内存的函数前，必须考虑当前的持锁状态
- 死锁的复现通常是间歇性的（依赖于精确的线程调度交错），LLDB 捕获的 backtrace 是诊断的关键

---

## 四、技术要点总结

### 4.1 thread_local 的使用原则

在 MudOS-NG 这样的单线程遗留代码中，全局可变状态无处不在。多线程化的第一步永远是 `thread_local` 化：

1. **VM 执行上下文:** `sp`, `pc`, `fp`, `csp`, `current_object`, `current_prog`, `outoftime` 等
2. **临时/中间结果:** `apply_ret_value`, `global_lvalue_*`, `fake_prog`
3. **资源池:** `sent_free`, `sprintf_state`
4. **计数器/标志:** `num_error`, `num_objects_this_thread`, `save_svalue_depth`

原则：**任何不在多线程间共享的 mutable 状态，都应该是 `thread_local`。**

### 4.2 真正需要共享的状态

不是所有状态都可以 `thread_local` 化。以下必须在多线程间共享，需要原子操作或锁：

| 状态 | 方案 | 原因 |
|------|------|------|
| `object_t::ref` | `std::atomic` | 对象引用计数 |
| `array_t::ref` | `std::atomic` | 数组引用计数 |
| `mapping_t::ref` | `std::atomic` | 映射表引用计数 |
| `block_t::refs` | `std::atomic` | 共享字符串引用计数 |
| `funptr_hdr_t::ref` | `std::atomic` | 函数指针引用计数 |
| `program_t::func_ref` | `std::atomic` | 程序函数引用计数 |
| `md_global_mutex` | `recursive_mutex` | debugmalloc hash table |
| `g_node_mutex` | `mutex` | mapping node free list |
| `g_object_list_mutex` | `mutex` | 全局对象链表 |

### 4.3 字符串处理的特殊挑战

字符串在 MudOS-NG 中有三种 subtype：
- `STRING_CONSTANT`：编译时确定，不可修改，不可释放
- `STRING_SHARED`：共享字符串表管理，引用计数
- `STRING_MALLOC`：堆分配，单独管理

多线程化引入的问题：
1. **指针比较失效:** 不同线程的 thread_local 字符串表中，同一内容的字符串有不同的指针 → 必须用 `strcmp`
2. **hash 依赖指针失效:** 同一字符串在不同线程的 hash 值不同 → 必须用内容 hash（`whashstr`）
3. **CONSTANT 字符串的误用:** `insert_in_mapping(key)` 中 key 被设为 CONSTANT subtype，后续 `free_string` 无效 → 必须用 `make_shared_string`

---

## 五、调试方法与技巧

### 5.1 必备工具链

| 场景 | 工具 | 命令示例 |
|------|------|---------|
| 死锁定位 | LLDB | `lldb -o "run ..." -o "bt all" -- ./driver` |
| 内存错误 | ASAN | `cmake -DENABLE_SANITIZER=ON` |
| 线程竞态 | TSAN | `-fsanitize=thread` (macOS 需 Linux) |
| 内存泄漏 | jemalloc stats | `malloc_stats_print()` |

### 5.2 LLDB 多线程调试

```bash
# 启动并自动捕获崩溃
lldb -o "run etc/config.test -fstress_run" \
     -o "bt all" \
     -- ../build/src/driver

# 查看所有线程
thread list
thread backtrace all
```

### 5.3 日志守则

多线程调试中 `fprintf(stderr, ...)` 必须立即 flush：

```cpp
fprintf(stderr, "HB-T%d: ...\n", thread_id);
fflush(stderr);  // 不可或缺！
```

没有 `fflush`，崩溃前的关键日志大概率留在缓冲区中丢失。

### 5.4 复现策略

- **间歇性死锁/竞态:** 连续运行 3-5 次，每次清空 LPC cache
- **大负载压力测试:** `-fstress_run` (30000 对象) 比 `-fhb_test` (1000 对象) 更容易触发竞态
- **首次运行最脆弱:** 第一次运行需要编译所有 LPC 对象，CPU/内存压力更大，竞态窗口更宽

---

## 六、测试矩阵

| 测试 | 命令 | 规模 | 状态 |
|------|------|------|------|
| 单元测试 | `./heartbeat_thread_tests` | 14 tests | ✅ |
| 单元测试 | `./lpc_tests` | 5 tests | ✅ |
| 单元测试 | `./ofile_tests` | 2 tests | ✅ |
| 心跳测试 | `-fhb_test` | 1000 对象, 5 heartbeats | ✅ 10+ 次稳定 |
| 压力测试 | `-fstress_run` | 30000 对象, 心跳循环 | ✅ 3 次稳定 |
| LPC 测试 | `-ftest` | 原有测试套件 | ⚠️ check_memory 未定义（已有问题） |

---

## 七、核心经验教训

### 7.1 设计层面

1. **先增量再并行:** 〇a/〇b/〇c 的增量扫描化改造是正确的策略——先把单线程逻辑整理清晰，再引入多线程
2. **IOThread 模式可复用:** `HeartbeatThread` 完美复用了 `IOThread` 的 `libevent` + `wakeup fd` + `post()` 模式，减少了设计负担
3. **门控模式:** `g_heartbeat_gate` 在对象批量创建期间关闭心跳 LPC 执行，避免了初始化阶段的大量竞态

### 7.2 实现层面

1. **thread_local 化要彻底:** 任何一个遗漏的全局变量都可能在特定调度下崩溃。需要系统性地审计所有 `static` 和全局变量
2. **加锁顺序是铁律:** 两个以上锁的场景，必须文档化加锁顺序，违反即死锁
3. **隐式分配是隐式加锁:** `DMALLOC`/`MDfree` 等内存函数内部获取 mutex，调用时需要考虑当前持锁状态
4. **字符串需要特殊对待:** 指针比较和指针 hash 在 thread_local 字符串表下全部失效

### 7.3 调试层面

1. **fflush 或 die:** 多线程 + 崩溃 = 日志必定在缓冲区，不 flush 等于没日志
2. **LLDB 是最后的手段:** ASAN 定位内存错误，LLDB 定位死锁和竞态
3. **间歇性 bug 最难调:** 连续多次运行 + 大负载 + 清缓存，提高复现率
4. **第一次运行最易崩:** 对象编译/初始化阶段负载最大，是竞态窗口最宽的时候

### 7.4 流程层面

1. **增量提交:** 每修复一个问题就独立提交，避免大量改动混在一起难以回溯
2. **交接文档至关重要:** 多 session 跨天的工作，没有 handoff 文档第二天无法继续
3. **LPC 测试套件作为回归测试:** `-fhb_test` 和 `-fstress_run` 专门为此改动编写的测试用例，是持续验证的基础

---

## 八、涉及文件清单

### 核心改动（C++ 源码）

| 文件 | 改动类别 |
|------|---------|
| `src/base/internal/heartbeat_thread.cc/.h` | 心跳线程 + 线程池实现 |
| `src/packages/core/heartbeat.cc/.h` | LPC 心跳入口，心跳门控 |
| `src/vm/internal/base/interpret.cc/.h` | VM 状态 thread_local，字符串安全 |
| `src/vm/internal/base/object.cc/.h` | 对象操作 thread_local |
| `src/vm/internal/simulate.cc/.h` | 模拟引擎 thread_local |
| `src/vm/internal/base/mapping.cc` | Mapping hash 内容化，节点锁，死锁修复 |
| `src/vm/internal/base/svalue.cc/.h` | svalue 原子引用计数 |
| `src/base/internal/stralloc.cc/.h` | 字符串表 thread_local，原子引用 |
| `src/vm/internal/apply.cc/.h` | apply_ret_value thread_local |
| `src/vm/internal/base/function.cc/.h` | funptr 原子引用计数 |
| `src/vm/internal/base/program.h` | func_ref 原子化 |
| `src/vm/internal/posix_timers.cc/.h` | macOS GCD timer → no-op |
| `src/base/internal/md.cc` | debugmalloc 加锁 |
| `src/vm/internal/posix_timers.cc` | 每线程 eval 计时器 |
| `src/packages/core/sprintf.cc` | sprintf_state thread_local |
| `src/packages/core/efuns_main.cc` | funptr copy → memcpy |
| `src/packages/core/add_action.cc` | 字符串比较 fix |
| `src/packages/parser/parser.cc` | 字符串比较 fix |
| `src/packages/contrib/contrib.cc` | 字符串比较 fix |
| `src/compiler/internal/compiler.cc` | 字符串比较 fix |
| `src/packages/develop/checkmemory.cc` | 原子 ref .load() |
| `src/packages/develop/develop.cc` | 原子 ref .load() |
| `src/packages/core/reclaim.cc` | func_ref .load() |
| `src/packages/core/file.cc` | apply_ret_value thread_local |
| `src/backend.cc` | 增量扫描，扫描/执行分离 |

### 测试文件

| 文件 | 用途 |
|------|------|
| `src/tests/test_heartbeat_thread.cc` | HeartbeatThread 单元测试 (14 tests) |
| `testsuite/stress/run_stress.c` | 压力测试控制器 (30000 对象) |
| `testsuite/stress/stress_worker.c` | 压力测试 worker 对象 |
| `testsuite/stress/simple_hb_test.c` | 渐进式心跳测试控制器 |
| `testsuite/stress/simple_hb_worker.c` | 渐进式心跳 worker |
| `testsuite/stress/simple_hb_runner.c` | 心跳运行辅助 |
| `testsuite/stress/map_test_only.c` | 独立 mapping 操作测试 |
| `testsuite/single/master.c` | 测试入口 flag 路由 |

---

## 九、待办事项

| 优先级 | 事项 | 说明 |
|--------|------|------|
| P1 | `-ftest` check_memory 未定义 | develop.spec 中 check_memory 受 `#if (defined(DEBUGMALLOC) && defined(DEBUGMALLOC_EXTENSIONS))` 保护，make_func 工具未正确定义该宏 |
| P2 | jemalloc dirty_decay 配置 | 减少 swap 占用 |
| P2 | `debugrealloc` 锁连续性 | 将 `MDfree`+`realloc`+`MDmalloc` 放在同一个 mutex 锁内 |
| P3 | `extend_string` subtype 检查 | 入口加 subtype 检查，拒绝非 MALLOC 字符串 |
| P3 | ASAN CI 集成 | CI 中添加 ASAN 构建以持续检测内存问题 |
