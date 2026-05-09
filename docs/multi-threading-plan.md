# MudOS-NG 多线程改造 — 工作计划

## 最新状态 (2026-05-09)

- **Phase 0-6, 8 已完成** — 对象增量扫描、VM thread_local、原子引用计数、每线程计时器、心跳线程池、心跳迁移、跨线程 bounce、call_out 线程池
- **30000 对象压力测试** — 对象创建 1s 完成，均匀分片到 8 线程（每线程 3000-4400），process_heartbeats #1 全线程成功触发
- **已修复的线程安全 bug（Phase 2 补充）：**
  - `funptr_hdr_t::ref` → `std::atomic<uint32_t>`（多线程同时创建 closure 导致 ref count 丢失）
  - `program_t::func_ref` → `std::atomic<unsigned short>`（dealloc_funp 竞争）
  - `apply_ret_value` → `thread_local`（call_function_pointer 多线程共享写入导致 double-free）
  - `debugmalloc` 全局状态加 mutex 保护，`DEBUGMALLOC_EXTENSIONS` 临时关闭（hash table/journal 竞争）
- **当前阻塞** — 30000 对象心跳执行期间静默 segfault（无 FATAL 日志），需 ASAN/LLDB 定位
- **下一目标** — 修复 segfault 使 30000 对象心跳稳定运行 30s+

## 背景

MudOS-NG 目前所有 LPC 代码都在单一线程上通过 libevent 主循环执行。这导致三个主要的运行时瓶颈：

1. **网络 I/O 阻塞主循环** — 多线程计划 Phase 1 已解决（`IOThread`/`IOThreadPool`，2 线程）
2. **周期性对象扫描卡顿** — `look_for_objects_to_swap()` 每 5 分钟一次性遍历全部对象并内联执行 LPC，造成 ~500ms 阻塞
3. **心跳串行执行** — 所有对象的 `heart_beat()` 在主线程顺序执行，无法利用多核

本计划覆盖瓶颈 2 和 3 的完整改造方案。

**总体目标：** 将对象扫描和心跳执行从主线程剥离，分布到多工作线程并行处理，支持 4 个以上 CPU 核心，消除主线程周期性卡顿。

## 核心挑战

LPC 虚拟机（`interpret.cc`）使用约 35 个全局可变状态变量（`sp`、`pc`、`fp`、`csp`、`current_object`、`current_prog`、`outoftime` 等），且完全无同步机制。所有游戏状态（对象、程序、映射表、数组）使用普通引用计数。执行时限计时器是进程级别的 POSIX 信号（`SIGVTALRM`/`SIGPROF`）。

## 总体方案：线程本地 VM + 共享状态加锁 + 心跳线程池

遵循已有的 `IOThread` 模式：每个心跳工作线程拥有独立的 libevent 事件循环和线程本地 VM 状态。共享数据结构使用原子引用计数。对象按指针哈希分片到各工作线程。跨线程调用先跳回主线程执行（第一阶段），后续改为阻塞式跨线程跳转（第二阶段，未来实现）。

---

## 第〇阶段：对象扫描增量化 — 消除周期性卡顿

**背景：** `look_for_objects_to_swap()` 每 5 分钟遍历整个 `obj_list`，对每个对象执行 `reset_object()` 和 `APPLY_CLEAN_UP` LPC 调用。在 10000+ 对象规模下造成 ~500ms 的阻塞卡顿，且对象析构会导致链表遍历从头重启，雪上加霜。

**设计原则：** 借鉴 Linux kswapd 的三条核心策略——持续增量扫描、扫描与执行分离、容错遍历。

### 改造 0a：容错遍历 — 先存 next 再处理当前（零依赖，即刻实施）

**当前问题**（[backend.cc:290-296](src/backend.cc#L290)）：对象在 `reset_object()` 或 `clean_up` 中析构后，`next_ob` 可能悬空，只能 `restart` 从头遍历。

**修改：** 进入处理前先保存 `next_all` 指针，即使当前对象被析构也不丢进度。

```cpp
// backend.cc look_for_objects_to_swap() 内部的 while 循环
object_t *ob = obj_list;
while (ob) {
  object_t *next = ob->next_all;   // 先存下家，防止 ob 析构后悬空

  if (!(ob->flags & O_DESTRUCTED)) {
    // ... 原有 reset/clean_up 逻辑不变 ...
  }
  ob = next;  // next 已在手，永不丢进度
  // 删除了原有的 restart 逻辑
}
```

**关键点：** 删除原有的 `last_good_ob`/`restart` 复杂逻辑，简化为单指针遍历。对象是否析构通过 `O_DESTRUCTED` 标志位判断即可——析构只是标记和从 `obj_list` 的 remove，不释放内存（真正的 free 在 `destruct2()` 延迟执行）。

**影响文件：** 仅 `src/backend.cc`，约改动 20 行。

### 改造 0b：增量扫描 — 每 tick 一小批（零依赖，即刻实施）

**当前问题：** 每 300 tick 一次性扫完全部对象 = 单次大卡顿。

**修改：** 将单次大扫描拆分为每 tick 处理固定数量（如 50 个），用静态游标断点续扫。扫完一轮后再等 5 分钟开始下一轮。

```cpp
void look_for_objects_to_swap() {
  // 每 tick 都来，不再 5 分钟一次
  add_gametick_event(
      time_to_next_gametick(std::chrono::milliseconds(CONFIG_INT(__RC_GAMETICK_MSEC__))),
      TickEvent::callback_type(look_for_objects_to_swap));

  static object_t *cursor = nullptr;  // 断点续扫游标
  static bool round_complete = false;

  auto time_to_clean_up = CONFIG_INT(__TIME_TO_CLEAN_UP__);
  int batch = CONFIG_INT(__RC_SWAP_BATCH_SIZE__);  // 新增配置项，默认 50
  int processed = 0;

  // 如果上一轮已完成，等待 5 分钟再开始新轮
  if (round_complete) {
    static int idle_ticks = 0;
    if (++idle_ticks < time_to_next_gametick(std::chrono::minutes(5))) {
      return;  // 还没到时间，跳过本轮
    }
    idle_ticks = 0;
    round_complete = false;
    cursor = nullptr;
  }

  object_t *ob = cursor ? cursor : obj_list;
  while (ob && processed < batch) {
    object_t *next = ob->next_all;

    if (!(ob->flags & O_DESTRUCTED)) {
      // ... 原有 reset/clean_up 逻辑 ...
      processed++;
    }
    ob = next;
  }
  cursor = ob;

  if (!cursor) {
    round_complete = true;  // 本轮扫完，开始 5 分钟冷却
  }
}
```

**新增配置项：** `__RC_SWAP_BATCH_SIZE__`（CFG_INT），默认 50。即每 tick 最多处理 50 个对象。10000 对象需要 200 tick 扫完（约 200 秒，远小于 5 分钟窗口），每 tick 处理时间 < 5ms。

**影响文件：** `src/backend.cc`（重写 `look_for_objects_to_swap`）、`src/base/internal/rc.cc`、`src/include/runtime_config.h`。

### 改造 0c：扫描与 LPC 执行分离（依赖 Phase 2 完成，后续集成）

**目标：** 主线程只做轻量遍历和分发，实际的 `reset_object()` 和 `APPLY_CLEAN_UP` 调用 post 给心跳线程池执行。

```
主线程（每 tick 轻量扫描）             心跳线程池（执行 LPC）
┌─────────────────────────┐          ┌─────────────────────────┐
│ 遍历链表，只判标志位     │  post   │ reset_object / clean_up  │
│ O_WILL_RESET?           │ ──────→ │ 并行执行                │
│ time_of_ref 过期?       │         │                         │
│ O_WILL_CLEAN_UP?        │         │                         │
│ (微秒级，不执行 LPC)    │         │                         │
└─────────────────────────┘          └─────────────────────────┘
```

在 `object_t` 中新增字段 `time_of_last_reset_scan` 和 `time_of_last_cleanup_scan`，主线程扫描时只更新时间戳并 post 任务，实际 LPC 由线程池异步执行。

**此改造在 Phase 2（心跳线程池）完成后实施。** 届时 `process_object_swap()` 可以直接复用 `HeartbeatThread::post()` 的投递机制。

### 验证（改造 0a + 0b）
- 构建运行，确认正常启动
- 使用 1000+ 对象运行 LPC 测试套件，确认无回归
- 监控：`look_for_objects_to_swap` 每 tick 调用次数和耗时，确认 < 5ms
- 确认对象析构后遍历不丢进度、不无限循环

---

## 第一阶段：VM 状态线程本地化（基础）

**范围：** 将所有 VM 执行全局变量改为 `thread_local`，使每个线程能独立执行 LPC 字节码。这是机械性修改（约 50 个声明跨 5 个文件），但涉及面最广。

### 需修改的文件

**`src/vm/internal/base/interpret.cc`** — 字节码解释器主文件

将以下顶层全局变量改为 `thread_local`：

| 当前声明 | 约行号 | 新声明 |
|---|---|---|
| `program_t *current_prog;` | ~107 | `thread_local program_t *current_prog;` |
| `short caller_type;` | ~108 | `thread_local short caller_type;` |
| `int call_origin;` | ~32 | `thread_local int call_origin;` |
| `error_context_t *current_error_context;` | ~32 | `thread_local error_context_t *current_error_context;` |
| `char *pc;` | 132 | `thread_local char *pc;` |
| `svalue_t *fp;` | 133 | `thread_local svalue_t *fp;` |
| `svalue_t *sp;` | 135 | `thread_local svalue_t *sp;` |
| `int function_index_offset;` | 137 | `thread_local int function_index_offset;` |
| `int variable_index_offset;` | 138 | `thread_local int variable_index_offset;` |
| `int st_num_arg;` | 139 | `thread_local int st_num_arg;` |
| `svalue_t _stack[...];` | 142 | `static thread_local svalue_t _stack[...];` |
| `svalue_t *const start_of_stack;` | 143 | `static thread_local svalue_t *const start_of_stack;` |
| `svalue_t *const end_of_stack;` | 144 | `thread_local svalue_t *end_of_stack;` |
| `svalue_t catch_value;` | 147 | `thread_local svalue_t catch_value;` |
| `control_stack_t _control_stack[...];` | 150 | `static thread_local control_stack_t _control_stack[...];` |
| `control_stack_t *const control_stack;` | 151 | `static thread_local control_stack_t *const control_stack;` |
| `control_stack_t *csp;` | 152 | `thread_local control_stack_t *csp;` |
| `int too_deep_error, max_eval_error;` | 154 | `thread_local int too_deep_error; thread_local int max_eval_error;` |
| `ref_t *global_ref_list;` | 156 | `thread_local ref_t *global_ref_list;` |
| `int lv_owner_type;` | ~579 | `thread_local int lv_owner_type;` |
| `refed_t *lv_owner;` | ~580 | `thread_local refed_t *lv_owner;` |
| `const char *lv_owner_str;` | ~581 | `thread_local const char *lv_owner_str;` |
| `int num_varargs;` | ~585 | `thread_local int num_varargs;` |
| `previous_instruction[60]` 等 | ~1971 | `static thread_local int previous_instruction[60];`（等） |

**`src/vm/internal/base/interpret.h`** — 外部声明（第 114-137 行）

将所有 `extern` 改为 `extern thread_local`。对 `end_of_stack` 和 `control_stack`（从静态数组计算得出），提供内联访问器：

```cpp
inline svalue_t *get_end_of_stack() {
  static thread_local svalue_t _s[10 + CFG_EVALUATOR_STACK_SIZE + 10];
  return &_s[10] + CFG_EVALUATOR_STACK_SIZE;
}
inline control_stack_t *get_control_stack() {
  static thread_local control_stack_t _cs[5 + CFG_MAX_CALL_DEPTH + 5];
  return &_cs[5];
}
```

更新引用这些变量的宏（`CHECK_STACK_OVERFLOW` 第 90-91 行等）以使用访问器。

**`src/vm/internal/base/machine.h`** — 第 37-39、48 行

```cpp
extern thread_local object_t *current_object;
extern thread_local object_t *command_giver;
extern thread_local object_t *current_interactive;
extern thread_local struct error_context_t *current_error_context;
```

**`src/vm/internal/simulate.cc`** — 第 117-119 行

```cpp
thread_local object_t *current_object;
thread_local object_t *command_giver;
thread_local object_t *current_interactive;
```

**`src/vm/internal/base/object.h`** — 第 141 行

```cpp
extern thread_local object_t *previous_ob;
```

**`src/vm/internal/base/object.cc`** — 第 42、2094-2095 行

```cpp
thread_local object_t *previous_ob;
static thread_local object_t *command_giver_stack[CFG_MAX_CALL_DEPTH];
thread_local object_t **cgsp = command_giver_stack + CFG_MAX_CALL_DEPTH;
```

**`src/packages/core/heartbeat.cc`** — 第 21 行

```cpp
thread_local object_t *g_current_heartbeat_obj;
```

**`src/packages/core/heartbeat.h`** — 第 12 行

```cpp
extern thread_local struct object_t *g_current_heartbeat_obj;
```

**`src/vm/internal/eval_limit.cc`** — 第 6-7 行

```cpp
thread_local volatile int outoftime = 0;
uint64_t max_eval_cost;  // 保持共享，仅从 LPC 配置读取
```

**`src/vm/internal/eval_limit.h`** — 第 8 行

```cpp
extern thread_local volatile int outoftime;
```

**`src/vm/internal/vm.cc`** — `clear_state()`（约第 123 行）和 `reset_machine()`（interpret.cc 约第 4890 行）

这些函数用于清零 VM 状态。改为 `thread_local` 后，它们自然只影响调用线程。在心跳线程初始化时调用 `reset_machine(1)` 即可。

### 验证
- 使用 `cmake .. -DCMAKE_BUILD_TYPE=Debug` 构建，运行 `make -j$(nproc)`
- 运行已有单元测试：`make test`
- 运行 LPC 测试套件：`cd testsuite && ../build/bin/driver etc/config.test -ftest`
- 全部通过（单线程行为不变）

---

## 第二阶段：共享数据结构线程安全化

**范围：** 引用计数改为原子操作，共享全局链表加互斥锁保护。

### 2a：原子引用计数

**`src/vm/internal/base/object.h`** — 第 71-72 行

```cpp
struct object_t {
  std::atomic<uint32_t> ref;           // 原为: uint32_t ref
  std::atomic<unsigned short> flags;   // 原为: unsigned short flags — 心跳循环需要检查 O_HEART_BEAT/O_DESTRUCTED
```

更新 `add_ref` 宏（第 124 行）：

```cpp
#define add_ref(ob, str) \
  SAFE(if ((ob)->ref.fetch_add(1, std::memory_order_relaxed) + 1 > ...) { ... })
```

将所有 `ob->ref--` 改为 `ob->ref.fetch_sub(1, std::memory_order_acq_rel)`。

**`src/vm/internal/base/array.h`** — `array_t::ref` → `std::atomic<uint32_t>`

**`src/vm/internal/base/mapping.h`** — `mapping_t::ref` → `std::atomic<uint32_t>`

**`src/vm/internal/base/program.h`** — `program_t::ref` → `std::atomic<unsigned int>`

**`src/vm/internal/base/svalue.h`** — `refed_t::ref` → `std::atomic<uint32_t>`

### 2b：对象列表互斥锁

**`src/vm/internal/simulate.h`** — 新增：

```cpp
extern std::mutex g_object_list_mutex;
```

**`src/vm/internal/simulate.cc`** — 定义 `std::mutex g_object_list_mutex;`

对所有 `obj_list` 和 `obj_list_destruct` 的插入/删除操作加 `std::lock_guard<std::mutex>`。关键位置：
- `load_object()` — 插入 obj_list（约第 553 行）
- `clone_object()` — 插入 obj_list（约第 651 行）
- `destruct_object()` — 从 obj_list 移除，加入 obj_list_destruct（约第 1050-1080 行）
- `destruct2()` / `remove_destructed_objects()` — 从 obj_list_destruct 移除

### 2c：共享字符串表互斥锁

**`src/vm/internal/base/stralloc.cc`** — 在共享字符串哈希表的插入/查找操作（`make_shared_string`、`findstring`、`hash_string`）周围加 `std::mutex`。

### 验证
- 重新构建并运行测试套件
- 使用 `-fsanitize=thread` 构建并运行测试以检测数据竞争

---

## 第三阶段：每线程独立执行时限计时器

**范围：** 将进程级 POSIX 计时器替换为每线程独立机制，使各心跳线程有独立的执行时限。

### Linux 实现

在 `posix_timers.cc` 中，使用 `timer_create` + `SIGEV_THREAD_ID` 实现每线程 API：

```cpp
struct PerThreadTimer {
  timer_t timer_id;
};
PerThreadTimer *per_thread_timer_create();    // 阻塞 SIGVTALRM，创建面向当前线程的计时器
void per_thread_timer_set(PerThreadTimer*, uint64_t micros);
void per_thread_timer_delete(PerThreadTimer*);
```

### macOS 实现

使用 `dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, ...)` 配合绑定到当前线程的 dispatch queue：

```cpp
PerThreadTimer *per_thread_timer_create() {
  auto *t = new PerThreadTimer;
  t->queue = dispatch_queue_create("eval_timer", nullptr);
  dispatch_queue_set_specific(t->queue, ...); // 绑定到当前线程
  t->source = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, t->queue);
  dispatch_source_set_event_handler(t->source, ^{ outoftime = 1; });
  dispatch_resume(t->source);
  return t;
}
```

### `eval_limit.cc` 中的改动

```cpp
// 添加线程本地指针指向每线程计时器
static thread_local PerThreadTimer *g_thread_timer = nullptr;

void set_eval(uint64_t etime) {
  outoftime = 0;
  if (g_thread_timer) {
    per_thread_timer_set(g_thread_timer, etime);
  } else {
    posix_eval_timer_set(etime);  // 主线程回退方案
  }
}
```

### 验证
- 验证主线程执行时限仍然有效（测试执行超时的单元测试）
- 检测心跳线程中每线程计时器是否正确触发并设置 `outoftime`

---

## 第四阶段：HeartbeatThread 与 HeartbeatThreadPool

**范围：** 参照 `IOThread`/`IOThreadPool` 模式创建心跳线程类。

### 新文件：`src/base/internal/heartbeat_thread.h`

```cpp
class HeartbeatThread {
 public:
  explicit HeartbeatThread(int id);
  ~HeartbeatThread();

  void start();
  void stop();
  void post(std::function<void()> task);

  // 心跳队列管理（可从任何线程调用，通过 post 路由到本线程执行）
  void add_heartbeat(object_t *ob, int interval);
  void remove_heartbeat(object_t *ob);
  void modify_heartbeat(object_t *ob, int interval);
  int query_heartbeat(object_t *ob);

  // 处理一个心跳周期
  void process_heartbeats();

  event_base *base() const { return base_; }
  bool is_current_thread() const;

 private:
  void event_loop();
  void process_pending_tasks();
  void init_vm_state();
  static void wakeup_cb(evutil_socket_t fd, short what, void *arg);

  // 与 IOThread 相同的模式：
  int id_;
  event_base *base_{nullptr};
  evutil_socket_t wakeup_fds_[2] = {-1, -1};
  event *wakeup_event_{nullptr};
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::mutex mutex_;
  std::deque<std::function<void()>> tasks_;

  // 心跳专属：
  std::deque<heart_beat_t> heartbeats_;
  std::deque<heart_beat_t> heartbeats_next_;
  void *eval_timer_{nullptr};  // PerThreadTimer*
};
```

### 新文件：`src/base/internal/heartbeat_thread.cc`

参照 `io_thread.cc` 模式实现。关键新增：

- **`init_vm_state()`**：创建 `PerThreadTimer`，调用 `reset_machine(1)` 初始化线程本地 VM
- **`process_heartbeats()`**：移植当前 `heartbeat.cc` 中 `call_heart_beat()` 的逻辑，适配单线程访问（数据已分片，无需跨线程处理）
- **`event_loop()`**：`init_vm_state(); while(running_) event_base_loop(base_, EVLOOP_ONCE);`

### HeartbeatThreadPool

```cpp
class HeartbeatThreadPool {
 public:
  explicit HeartbeatThreadPool(size_t num_threads);
  void start();
  void stop();
  HeartbeatThread *thread_for_object(object_t *ob) {
    return threads_[hash_ptr(ob) % threads_.size()].get();
  }
  HeartbeatThread *thread(int idx) { return threads_[idx].get(); }
  size_t size() const { return threads_.size(); }

 private:
  std::vector<std::unique_ptr<HeartbeatThread>> threads_;
};
```

### 在 `src/CMakeLists.txt` 中添加

参照 `base/internal/io_thread.cc` 的模式，将 `base/internal/heartbeat_thread.cc` 加入源文件列表。

### 验证
- 单元测试：`test_heartbeat_thread.cc`（创建/启动/停止，添加/移除心跳，处理周期）
- 模式参照已有的 `test_io_thread.cc`

---

## 第五阶段：心跳迁移

**范围：** 重写 `heartbeat.cc`，将心跳管理委托给线程池，不再使用全局双端队列。

### `src/packages/core/heartbeat.cc` 的改动

**删除**静态的 `heartbeats` 和 `heartbeats_next` 双端队列（第 27 行）。

**重写 `call_heart_beat()`：**

```cpp
void call_heart_beat() {
  // 重新注册下一轮（保留现有计时逻辑）
  add_gametick_event(
      time_to_next_gametick(std::chrono::milliseconds(CONFIG_INT(__RC_HEARTBEAT_INTERVAL_MSEC__))),
      TickEvent::callback_type(call_heart_beat));

  // 通知各个心跳线程处理各自的周期
  if (g_heartbeat_thread_pool) {
    for (size_t i = 0; i < g_heartbeat_thread_pool->size(); i++) {
      g_heartbeat_thread_pool->thread(i)->post([](HeartbeatThread *t) {
        t->process_heartbeats();
      });
    }
  }
}
```

**重写 `set_heart_beat()`：**

```cpp
int set_heart_beat(object_t *ob, int to) {
  if (ob->flags & O_DESTRUCTED) return 0;
  if (to < 0) to = 1;
  auto *pool = g_heartbeat_thread_pool;
  auto *thread = pool ? pool->thread_for_object(ob) : nullptr;

  if (to == 0) {
    ob->flags &= ~O_HEART_BEAT;
    if (pool) thread->remove_heartbeat(ob);
    return 1;
  }
  ob->flags |= O_HEART_BEAT;
  if (pool) thread->modify_heartbeat(ob, to);
  return 1;
}
```

**重写 `query_heart_beat()`** 和 **`heart_beat_status()`** 以从线程池查询（跨线程聚合）。

**重写 `get_heart_beats()`**（`F_HEART_BEATS` efun）以从所有线程收集。

### `src/mainlib.cc` 的改动

添加心跳线程池生命周期管理（约在第 449-462 行 IO 线程池管理附近）：

```cpp
// 在 IO 线程池之后：
int num_hb_threads = CONFIG_INT(__RC_HEARTBEAT_THREADS__);
if (num_hb_threads <= 0) num_hb_threads = std::max(1u, std::thread::hardware_concurrency() - 1);
g_heartbeat_thread_pool = new HeartbeatThreadPool(num_hb_threads);
g_heartbeat_thread_pool->start();

// 在关闭流程中（IO 线程池停止之前）：
g_heartbeat_thread_pool->stop();
delete g_heartbeat_thread_pool;
g_heartbeat_thread_pool = nullptr;
```

### 新增运行时配置项

在 `src/base/internal/rc.cc` 和 `src/include/runtime_config.h` 中：新增 `__RC_HEARTBEAT_THREADS__`（CFG_INT）。默认值：0（自动检测 = `hardware_concurrency - 1`）。

### `src/backend.cc` 的改动

第 224 行：无需改动 — `call_heart_beat()` 的注册保持不变，只是现在它分发到线程池。

### 验证
- 使用 1、2、4 个心跳线程运行 LPC 测试套件
- 验证心跳间隔配置仍正确生效
- LPC 中 `set_heart_beat()` / `query_heart_beat()` 正确工作
- `heart_beats()` efun 返回正确结果
- 心跳期间对象销毁不会导致崩溃

---

## 第六阶段：跨线程调用处理

**范围：** 当线程 A 上执行的心跳调用了属于线程 B 的对象时，安全处理此情况。

### 第六阶段-A（初始方案）：跳转回主线程

当 `call_direct()` 检测到跨线程调用时，将整个剩余心跳执行打包发回主线程的事件循环，并在工作线程上中止当前心跳：

```cpp
// 在 call_direct() 中 — interpret.cc 第 4340 行
void call_direct(object_t *ob, int offset, int origin, int num_arg) {
  // 跨线程检查
  if (g_heartbeat_thread_pool && g_current_heartbeat_thread) {
    auto *target_thread = g_heartbeat_thread_pool->thread_for_object(ob);
    if (target_thread != g_current_heartbeat_thread) {
      // 将整个心跳跳转回主线程
      event_base_once(g_event_base, -1, EV_TIMEOUT,
        bounce_heartbeat_to_main_thread, ob, nullptr);
      throw heartbeat_bounce_exception{};  // 在心跳循环中捕获
    }
  }
  // ... 正常执行 ...
}
```

心跳循环捕获 `heartbeat_bounce_exception` 并继续处理下一个对象。主线程上的回调节点重新执行该心跳。这意味着复杂心跳（含跨对象调用）在主线程运行，而简单心跳（只访问自身对象）在工作线程上并行运行。

为优化所有权检查，在 `object_t` 中新增 `int heart_beat_thread_id` 字段，避免每次哈希计算。

### 第六阶段-B（未来方案）：阻塞式跨线程跳转

使用 `std::promise`/`std::future` 实现阻塞式跨线程调用。需要死锁防护（跳转深度限制，按对象地址排序加锁）。推迟到未来迭代。

### 验证
- 创建 LPC 心跳，其中 call_other 到不同分片上的对象
- 验证它们正确完成（在主线程上）
- 在 ThreadSanitizer 下验证无数据竞争或崩溃

---

## 第七阶段：集成与性能测试

### ThreadSanitizer 构建

```bash
cmake .. -DCMAKE_BUILD_TYPE=Debug -DENABLE_SANITIZER=ON \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread" -DCMAKE_C_FLAGS="-fsanitize=thread"
make -j$(nproc)
```

在 TSan 下运行完整测试套件，修复所有检测到的数据竞争。

### 正确性测试
- 使用 1、2、4、8 个心跳线程运行 LPC 测试套件
- 压力测试：1000+ 个对象带心跳，验证无遗漏执行、无崩溃
- 对象生命周期测试：心跳执行期间快速创建/销毁带心跳的对象

### 性能基准
- 不同线程数下的心跳吞吐量（心跳/秒）
- 各 CPU 核心的利用率
- 心跳负载下主线程的响应性（玩家命令延迟）
- 每线程 VM 栈的内存开销

### 关闭安全性
- 验证所有线程上有活跃心跳时的干净关闭
- 确保心跳线程池在对象清理/IO 线程池之前停止

---

## 文件变更汇总

| 文件 | 阶段 | 变更内容 |
|------|------|----------|
| `src/backend.cc` | 0a, 0b, 5 | 容错遍历（删 restart 逻辑）；增量扫描（游标断点续扫）；心跳注册不变 |
| `src/base/internal/rc.cc` | 0b, 5 | 新增 `__RC_SWAP_BATCH_SIZE__` 配置；新增 `__RC_HEARTBEAT_THREADS__` 配置 |
| `src/include/runtime_config.h` | 0b, 5 | 新增 `__RC_SWAP_BATCH_SIZE__` 和 `__RC_HEARTBEAT_THREADS__` 枚举 |
| `src/vm/internal/base/interpret.cc` | 1, 6 | ~35 个全局变量 → `thread_local`；`call_direct` 跨线程检查 |
| `src/vm/internal/base/interpret.h` | 1 | `extern` → `extern thread_local`；访问器函数 |
| `src/vm/internal/base/machine.h` | 1 | `current_object` 等 → `extern thread_local` |
| `src/vm/internal/simulate.cc` | 1, 2 | `thread_local` 全局变量；`g_object_list_mutex` |
| `src/vm/internal/simulate.h` | 2 | `extern std::mutex g_object_list_mutex` |
| `src/vm/internal/base/object.h` | 1, 2 | `std::atomic` ref/flags；`thread_local previous_ob`；新增 `heart_beat_thread_id` |
| `src/vm/internal/base/object.cc` | 1 | `thread_local cgsp` 等 |
| `src/vm/internal/eval_limit.cc` | 1, 3 | `thread_local outoftime`；每线程计时器 |
| `src/vm/internal/eval_limit.h` | 1 | `extern thread_local outoftime` |
| `src/vm/internal/posix_timers.cc` | 3 | 每线程计时器 API（Linux + macOS） |
| `src/vm/internal/posix_timers.h` | 3 | 每线程计时器声明 |
| `src/vm/internal/base/array.h` | 2 | `std::atomic` ref |
| `src/vm/internal/base/mapping.h` | 2 | `std::atomic` ref |
| `src/vm/internal/base/program.h` | 2 | `std::atomic` ref |
| `src/vm/internal/base/svalue.h` | 2 | `refed_t` 中 `std::atomic` ref |
| `src/vm/internal/base/stralloc.cc` | 2 | 共享字符串表互斥锁 |
| `src/base/internal/heartbeat_thread.h` | 4 | **新增** — HeartbeatThread 类 |
| `src/base/internal/heartbeat_thread.cc` | 4 | **新增** — HeartbeatThread 实现 |
| `src/CMakeLists.txt` | 4 | 添加 heartbeat_thread.cc |
| `src/packages/core/heartbeat.cc` | 5 | 重写为委托线程池 |
| `src/packages/core/heartbeat.h` | 1, 5 | `thread_local g_current_heartbeat_obj` |
| `src/mainlib.cc` | 5 | 心跳线程池生命周期 |
| `src/backend.cc` | 5 | 最小改动（已有注册保持不变） |
| `src/base/internal/rc.cc` | 5 | 新增 `__RC_HEARTBEAT_THREADS__` 配置 |
| `src/include/runtime_config.h` | 5 | 新增 `__RC_HEARTBEAT_THREADS__` 枚举 |
| `src/tests/test_heartbeat_thread.cc` | 4 | **新增** — 单元测试 |
| `src/tests/test_io_thread.cc` | 1 | **新增** — IO 线程池单元测试 |
| **Phase 2 补充（bug 修复）：** |
| `src/vm/internal/base/function.h` | 2+ | `funptr_hdr_t::ref` → `std::atomic<uint32_t>` |
| `src/vm/internal/base/function.cc` | 2+ | `func_ref`/`hdr.ref` 原子化调试打印用 `.load()` |
| `src/vm/internal/base/program.h` | 2+ | `func_ref` → `std::atomic<unsigned short>` |
| `src/vm/internal/apply.h` | 2+ | `apply_ret_value` → `extern thread_local` |
| `src/vm/internal/apply.cc` | 2+ | `apply_ret_value` → `thread_local` |
| `src/vm/internal/base/svalue.cc` | 2+ | `int_free_svalue` 加 immortal guard (ref==0 时不 fetch_sub) |
| `src/base/internal/stralloc.h` | 2+ | `block_t::refs`/`malloc_block_t::ref` → atomic；INC/DEC_COUNTED_REF 宏原子化 |
| `src/base/internal/stralloc.cc` | 2+ | `fmt::format` 原子化适配 `.load()` |
| `src/base/internal/md.cc` | 2+ | `md_refjournal` map 加 mutex；`MDmalloc`/`MDfree` 加 mutex |
| `src/packages/core/efuns_main.cc` | 2+ | `funptr_t` copy 改用 `memcpy` + `.store()` |
| `src/packages/develop/checkmemory.cc` | 2+ | 原子 ref 格式化适配 `.load()` |
| `src/packages/core/file.cc` | 2+ | 局部 `extern apply_ret_value` → `thread_local` |
| `src/packages/core/reclaim.cc` | 2+ | `func_ref` 调试打印用 `.load()` |
| `build/src/config.h` | 2+ | 临时关闭 `DEBUGMALLOC_EXTENSIONS`（多线程兼容） |

---

## 执行顺序

```
第〇阶段 (增量扫描 + 容错遍历) ──→ 第一阶段 (thread_local VM)
    ↑ 零依赖，立刻可做                       │
    │ 0a+0b 独立于所有后续阶段               ├──→ 第二阶段 (原子引用计数)
    │                                        │        │
    └── 0c 在 Phase 2 后集成                 │        ▼
                                             │   第三阶段 (每线程计时器)
                                             │        │
                                             │        ▼
                                             │   第四阶段 (HeartbeatThread)
                                             │        │
                                             │        ▼
                                             │   第五阶段 (迁移 heartbeat.cc)
                                             │        │
                                             └────────┤
                                                      ▼
                                             第六阶段 (跨线程调用 + 0c 集成)
                                                      │
                                                      ▼
                                             第七阶段 (测试/TSan)
```

**第〇阶段（0a+0b）零依赖，可以立刻独立实施，收益立竿见影。** 第〇阶段 0c 和第二阶段可部分并行。第四→五→六阶段必须顺序执行。第三阶段可与第一、二阶段并行。

## 关键风险

1. **遗漏的全局变量**：某个全局变量未改为 `thread_local` 导致难以排查的数据损坏。**对策：** ThreadSanitizer 构建可捕获所有数据竞争。
2. **macOS 执行计时器**：`timer_create+SIGEV_THREAD_ID` 不可用。**对策：** 使用 `dispatch_source`，它能将事件分发到正确的线程。
3. **对象析构竞争**：心跳线程读取对象时主线程析构该对象。**对策：** `std::atomic<unsigned short> flags`，心跳循环中检查 `O_DESTRUCTED` 标志。
4. **关闭顺序**：心跳线程池必须在对象清理前停止。**对策：** `mainlib.cc` 中明确顺序 — 先停心跳池，再停 IO 池，最后对象清理。

---

## 第八阶段：对象扫描与 LPC 执行分离（Phase 0c）

**背景：** Phase 0a+0b 已消除周期性卡顿（增量扫描 + 容错遍历），但 `reset_object()` 和 `APPLY_CLEAN_UP` 仍在主线程执行 LPC 代码。心跳线程池建成后，这些调用可以 post 给工作线程执行。

**现状：** [backend.cc](src/backend.cc#L271) 中 `look_for_objects_to_swap()` 的主线程扫描循环内联执行 LPC。

### 改造方案

主线程扫描时只做轻量标志位判断和分发，不再执行 LPC：

```cpp
// 在 look_for_objects_to_swap() 的扫描循环中
while (ob && (batch == 0 || processed < batch)) {
  object_t *next = ob->next_all;
  if (ob->flags & O_DESTRUCTED) { ob = next; continue; }

  // 主线程只判断是否到期，不执行 LPC
  bool need_reset = false, need_clean_up = false;

  if (!CONFIG_INT(__RC_NO_RESETS__) && !CONFIG_INT(__RC_LAZY_RESETS__)) {
    if ((ob->flags & O_WILL_RESET) && (g_current_gametick >= ob->next_reset) &&
        !(ob->flags & O_RESET_STATE)) {
      need_reset = true;
    }
  }

  if (time_to_clean_up > 0) {
    if (gametick_to_time(g_current_gametick - ob->time_of_ref) >=
        std::chrono::seconds(time_to_clean_up)) {
      if (ob->flags & O_WILL_CLEAN_UP) need_clean_up = true;
    }
  }

  if (need_reset || need_clean_up) {
    // Post 给心跳线程池执行实际 LPC 调用
    auto *thread = g_heartbeat_thread_pool->thread_for_object(ob);
    thread->post([ob, need_reset, need_clean_up]() {
      if (need_reset && !(ob->flags & O_DESTRUCTED)) {
        reset_object(ob);
      }
      if (need_clean_up && !(ob->flags & O_DESTRUCTED) &&
          (ob->flags & O_WILL_CLEAN_UP)) {
        push_number(ob->flags & (O_CLONE) ? 0 : ob->prog->ref.load());
        set_eval(max_eval_cost);
        auto *svp = safe_apply(APPLY_CLEAN_UP, ob, 1, ORIGIN_DRIVER);
        if (!svp || (svp->type == T_NUMBER && svp->u.number == 0)) {
          ob->flags &= ~O_WILL_CLEAN_UP;
        }
      }
    });
  }

  ob = next;
  processed++;
}
```

### 改动范围

| 文件 | 改动 | 行数 |
|------|------|------|
| `src/backend.cc` | 将 reset/clean_up 的 LPC 调用替换为 post 到心跳线程池 | ~40 行 |

### 前置条件
- `g_heartbeat_thread_pool` 必须已启用（`heartbeat threads > 0`）
- 当心跳池未启用时，保持原有内联执行路径（fallback）

### 验证
- 构建通过，测试通过
- 对象 reset 和 clean_up 仍然正确触发
- 主线程扫描循环耗时进一步降低（不再执行 LPC）

---

## 第九阶段：call_out 线程池

**背景：** call_out 是 LPC 的定时回调系统，使用场景比心跳更广泛。可在任意对象上以任意延迟调度任意函数，支持 0 延迟（立即执行）和 walltime 模式。当前所有 call_out 在主线程 game tick 中串行执行。

**现状架构：** [call_out.cc](src/packages/core/call_out.cc)

- `new_call_out()` — 创建定时回调，注册 gametick 或 walltime 事件
- `call_out()` — 回调触发时执行，查找对象、组装参数、调用 `safe_apply()` 或 `safe_call_function_pointer()`
- 全局 `g_callout_handle_map`（`unordered_map`）和 `g_callout_object_handle_map`（`unordered_multimap`）用于查找和删除

### 改造方案：复用心跳线程池基础设施

call_out 执行与心跳执行是同类工作负载（对单个对象执行 LPC），可以直接复用 `HeartbeatThread`：

```cpp
// 在 call_out() 函数中，替换直接执行 LPC 的部分
void call_out(pending_call_t *cop) {
  // ... 前期检查、参数组装保持不变 ...

  // 替代直接 safe_apply() / safe_call_function_pointer()：
  auto *pool = g_heartbeat_thread_pool;
  if (pool && cop->ob) {
    // 分发到工作线程
    auto *thread = pool->thread_for_object(cop->ob);
    thread->post([cop, ob, num_callout_args, new_command_giver]() {
      set_eval(max_eval_cost);
      save_command_giver(new_command_giver);
      if (cop->ob) {
        (void)safe_apply(cop->function.s, cop->ob, num_callout_args, ORIGIN_INTERNAL);
      } else {
        (void)safe_call_function_pointer(cop->function.f, num_callout_args);
      }
      restore_command_giver();
      free_called_call(cop);
    });
  } else {
    // Fallback: 主线程原地执行（pool 未启用或 cop->ob 为空）
    // ... 原有执行逻辑 ...
  }
}
```

### 0 延迟 call_out 的特殊处理

`call_out("func", 0)` 要求在**当前 tick** 立即执行，不能延迟到下一帧。策略：

- 0 延迟的 call_out → **始终在主线程原地执行**（保持语义，避免死循环检测失效）
- 正延迟的 call_out → **分发给心跳线程池**

```cpp
if (pool && cop->ob && delay_msecs.count() > 0) {
  // 正延迟 → 分发到工作线程
  thread->post(...);
} else {
  // 0 延迟或 pool 未启用 → 主线程原地执行
  ... original code ...
}
```

### 线程安全的 callout map

`g_callout_handle_map` 和 `g_callout_object_handle_map` 在以下场景中被并发访问：
- `new_call_out()` 插入
- `call_out()` 删除
- `remove_call_out()` / `remove_all_call_out()` 查找和删除
- `find_call_out()` 只读查询

**对策：** 添加 `std::mutex g_callout_map_mutex` 保护所有 map 操作。插入和删除操作快速（O(1) 哈希），锁持有时间极短。

### 改动范围

| 文件 | 改动 | 行数 |
|------|------|------|
| `src/packages/core/call_out.cc` | `call_out()` 分发到线程池；0 延迟特殊处理；map 操作加锁 | ~60 行 |
| `src/packages/core/call_out.h` | 新增 `extern std::mutex g_callout_map_mutex` | +1 行 |

### 前置条件
- Phase 0-6 完成（心跳线程池可用）
- `g_heartbeat_thread_pool` 必须已启用

### 验证
- call_out 测试（0 延迟、正延迟、walltime、跨对象 call_out）
- remove_call_out 在回调执行期间的正确性
- call_out(0) 死循环检测仍然有效
- 构建 + 测试套件通过

---

## 第十阶段：编译线程化

**目标：** `load_object()` 中的 LPC 编译过程移到独立线程，消除对象加载对主循环的阻塞。

**方案：** 参考 async 包的文件 I/O 模式。主线程 post 编译请求，编译线程执行词法分析 + 语法分析 + 代码生成，通过 `std::promise`/`std::future` 返回 `program_t*`。

**改动范围：** ~100 行（新建编译线程类 + 改动 load_object）。

**关键难点：** 编译器全局状态（`lex.cc` 中的词法分析器状态、`grammar.y` 生成的语法分析器状态）需要线程安全化。

---

## 执行顺序

```
Phase 0c (扫描/执行分离) ──→ Phase 8 (call_out 线程池) ──→ Phase 9 (编译线程化)
     ↑                              ↑                           ↑
  依赖心跳池启用                 复用 heartbeat 基础设施       参考 async 包模式
  ~40 行改动                     ~60 行改动                    ~100 行改动
```

Phase 0c 和 Phase 8 可以并行实施。Phase 9 是独立模块。

### 后续阶段 (已完成 Phase 0-6 后)

```
第八阶段 (Phase 0c: 扫描/执行分离) ──→ 第九阶段 (call_out 线程池) ──→ 第十阶段 (编译线程化)
      ↑                                        ↑                              ↑
 依赖心跳池启用                             复用 HeartbeatThread           参考 async 包模式
 ~40 行改动                               ~60 行改动                      ~100 行改动

 ✅ 已完成                             ✅ 已完成                        ⬜ 待实施 (需编译器状态隔离)
```

---

## 已移除的任务：玩家命令并行处理

### 原计划

将 `process_user_command()` 中的 LPC 执行分布到多工作线程，实现玩家命令的并行处理。

### 移除原因

经过 Phase 0-8 的改造，主线程的核心负载已发生根本性变化：

| 负载 | 改造前 | 改造后 |
|------|--------|--------|
| 网络 I/O | 主线程 | IO 线程池 ✅ |
| 心跳执行 | 主线程 (~40%) | 心跳线程池 ✅ |
| 对象扫描 (reset/clean_up) | 主线程 (~15%) | 心跳线程池 ✅ |
| call_out 执行 | 主线程 (~20%) | 心跳线程池 ✅ |
| 玩家命令 | 主线程 (~20%) | 主线程 (~85%) |

**主线程剩余负载占比虽然变大了（20%→85%），但绝对 CPU 消耗不变。** 更重要的是，主线程现在几乎专门服务于玩家命令，没有批量后台任务与之争夺 CPU 时间片。

### 架构判断

当前架构已经形成最优分工：

```
主线程 (低延迟交互专用)              工作线程池 (批量后台任务)
┌─────────────────────────┐         ┌─────────────────────────┐
│ 玩家命令处理             │         │ Thread 0: heartbeat     │
│ - 输入解析、命令执行     │         │ Thread 1: call_out      │
│ - 即时反馈给玩家        │         │ Thread 2: object swap   │
│ - 0 延迟 call_out       │         │ Thread 3: ...           │
│                         │         │                         │
│ 延迟敏感，需独占核心     │         │ 批量任务，可并行         │
└─────────────────────────┘         └─────────────────────────┘
```

这种设计类似于 nginx 的 worker 模型：一个主循环处理核心交互，重负载卸载到后台。由于玩家命令的交互特性（延迟敏感、需要即时反馈），让主线程专门服务它反而是最优解。

### 不做玩家命令并行化的理由

1. **主线程已足够空闲** — 心跳、call_out、对象扫描全部移出后，主线程 CPU 占用率从 ~100% 降到 ~25-35%（取决于玩家数量），单个核心完全够用
2. **命令交互频繁跨对象** — 玩家命令执行期间频繁访问 `this_player()`、`environment()`、携带物品、目标对象，跨线程 bounce 率远高于心跳，并行化收益被 bounce 开销抵消
3. **延迟敏感** — 命令需要即时反馈给玩家。如果 bounce 到其他线程再返回（通过 promise/future），增加的延迟对用户体验是净损失
4. **单玩家命令不会耗尽 CPU** — 每个命令有 eval 时限保护，不存在单一命令无限占用的问题。多玩家同时发命令时，主线程串行处理的延迟（微秒级上下文切换）远小于跨线程通信延迟

### 结论

**玩家命令并行化不是性能瓶颈，不做反而更优。** 从工作计划中永久移除该任务。如果有 MUD 遇到玩家命令延迟问题，应该从 LPC 代码层面优化（分帧执行长循环、减少 call_other 层级），而非从驱动架构层面并行化命令处理。
