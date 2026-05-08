# MudOS-NG 测试报告

**生成时间:** 2026-05-08
**分支:** master
**提交:** 最新

---

## 摘要

| 测试套件 | 测试数 | 通过 | 失败 | 耗时 |
|----------|--------|------|------|------|
| lpc_tests (LPC 编译器/驱动) | 5 | 5 | 0 | 22ms |
| io_thread_tests (IO 线程池) | 14 | 14 | 0 | 143ms |
| heartbeat_thread_tests (心跳线程池) | 23 | 23 | 0 | 356ms |
| ofile_tests (对象文件) | 2 | 2 | 0 | 3ms |
| **总计** | **44** | **44** | **0** | **524ms** |

---

## heartbeat_thread_tests 详细结果

### HeartbeatThread 生命周期 (3 tests)
- `CreateStartStop` — 线程创建、启动、停止
- `MultipleStartStopCycles` — 5 次启动/停止循环
- `IdAssignment` — 线程 ID 正确分配

### 任务投递 (3 tests)
- `PostSingleTask` — 单任务投递并在工作线程执行
- `TaskRunsOnWorkerThread` — 验证任务运行在独立线程（非调用线程）
- `TaskFifoOrdering` — 20 个任务按 FIFO 顺序执行

### 并发压力 (1 test)
- `ConcurrentPosting` — 8 线程 × 500 任务 = 4000 并发投递，零丢失

### 心跳队列操作 (5 tests)
- `AddAndQueryHeartbeat` — 添加心跳并查询间隔
- `RemoveHeartbeat` — 删除心跳，查询返回 0
- `ModifyHeartbeat` — 修改心跳间隔 (5→10)
- `MultipleObjectsHeartbeats` — 100 对象同时添加心跳，间隔正确
- `HeartbeatCount` — 心跳计数准确

### 线程追踪 (1 test)
- `CurrentThreadTracker` — `g_current_heartbeat_thread` 正确设置和重置

### 执行时限 (1 test)
- `EvalTimerLifecycle` — 每线程 eval 计时器正常工作（无崩溃）

### VM 状态隔离 (1 test)
- `VmStateIsolation` — 两线程独立运行心跳处理，互不干扰

### 死锁检测 (2 tests)
- `NoDeadlockConcurrentPostAndStop` — 并发投递过程中停止，无死锁（<5s）
- `NoDeadlockDoubleStop` — 重复停止安全无害

### 压力测试 (1 test)
- `StressManyTasksManyCallers` — 8 caller × 200 batch 并发投递

### HeartbeatThreadPool (5 tests)
- `CreateAndDestroy` — 线程池创建和销毁
- `ObjectShardingDeterministic` — 同一对象始终映射到同一线程
- `ObjectShardingDeterministicAcrossPool` — 100 对象全部映射到合法线程
- `PostToShardedThread` — 按分片投递任务
- `MultiplePoolsDoNotConflict` — 多线程池独立工作

---

## io_thread_tests (已有，回归验证)

| 类别 | 测试 | 状态 |
|------|------|------|
| IOThread 生命周期 | CreateStartStop, MultipleStartStopCycles | PASSED |
| 任务投递 | PostSingleTask, TaskRunsOnIOThread, TaskFifoOrdering, NestedPost | PASSED |
| 并发 | ConcurrentPosting | PASSED |
| 事件循环 | EventBaseOnceOnIOThread | PASSED |
| IOThreadPool | CreateAndDestroy, SingleThreadDistribution, MultipleThreadsRoundRobin, ThreadIdAssignment, TasksRunOnCorrectThreads, ConcurrentRoundRobinWork | PASSED |

---

## lpc_tests / ofile_tests (已有，回归验证)

| 测试 | 状态 |
|------|------|
| TestCompileDumpProgWorks | PASSED |
| TestInMemoryCompileFile | PASSED |
| TestInMemoryCompileFileFail | PASSED |
| TestValidLPC_FunctionDeafultArgument | PASSED |
| TestLPC_FunctionInherit | PASSED |
| OFile test_load | PASSED |
| OFile test_read | PASSED |

---

## 覆盖率分析

| 模块 | 测试覆盖 |
|------|---------|
| HeartbeatThread 生命周期 | ✅ start/stop/restart |
| HeartbeatThread 任务系统 | ✅ post/fifo/concurrent |
| HeartbeatThread 心跳队列 | ✅ add/remove/modify/query/process |
| HeartbeatThreadPool 分片 | ✅ deterministic/valid-thread |
| 跨线程 bounce | ✅ g_current_heartbeat_thread tracker |
| 每线程 eval 计时器 | ✅ init/set/get 生命周期 |
| 死锁安全 | ✅ concurrent-post+stop, double-stop |
| 并发压力 | ✅ 4000 任务 / 8 callers |
| 回归：IO 线程池 | ✅ 14 tests |
| 回归：LPC 编译器 | ✅ 5 tests |
| 回归：对象文件 | ✅ 2 tests |

---

## 已知限制

1. **心跳 LPC 执行未覆盖** — 测试使用 `heart_beat=0` 的程序跳过实际 LPC 执行。完整的 LPC 心跳执行需要完整的 mudlib 环境，适合在 LPC 测试套件中验证。
2. **跨线程 bounce 端到端未覆盖** — `g_current_heartbeat_thread` 的设置和重置已验证，但 `call_direct` 中的实际 bounce 逻辑需要多线程 LPC 执行环境。
3. **TSan 构建** — 尚未运行 ThreadSanitizer 验证。建议后续使用 `-fsanitize=thread` 构建。
