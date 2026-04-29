<!--
Copyright (c) 2026 [大河马/dahema@me.com]
SPDX-License-Identifier: MIT
-->

# MudOS-NG 单线程架构分析

本文档分析 MudOS-NG 的事件驱动架构,重点识别单线程模型中可能导致阻塞或卡住的瓶颈点。

## 整体架构:单线程事件驱动

MudOS-NG 基于 **libevent 事件循环**,主线程运行 `event_base_loop()` 处理所有工作:

```
┌─────────────────────────────────────────────────────┐
│                   Main Thread                        │
│  ┌──────────────────────────────────────────────┐   │
│  │          libevent event_base_loop()           │   │
│  │                                               │   │
│  │  ┌──────────┐  ┌──────────┐  ┌────────────┐  │   │
│  │  │Network I/O│  │Game Tick │  │Walltime    │  │   │
│  │  │(bufferevt)│  │(evtimer) │  │Events      │  │   │
│  │  └──────────┘  └──────────┘  └────────────┘  │   │
│  └──────────────────────────────────────────────┘   │
│                                                      │
│  ┌──────────────────────────────────────────────┐   │
│  │         Async Worker Threads                   │   │
│  │  (detached std::thread, file I/O, DB queries) │   │
│  └──────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────┘
```

### 控制流

网络输入到输出的完整路径:

```
libevent 检测到可读 socket
  → on_user_read() 回调
    → get_user_data() 从 bufferevent 读取
      → telnet 协商 / 输入处理
      → 若命令完整: evtimer_add(ev, zero_sec) 调度命令处理
        [返回事件循环]

下次事件循环迭代:
  → on_user_command() 回调
    → set_eval(max_eval_cost)  设置 eval 限制定时器
    → process_user_command()
      → get_user_command() 提取命令字符串
      → process_input()
        → safe_apply(APPLY_PROCESS_INPUT)  或 safe_parse_command()
          → user_parser()
            → 匹配 verb→action, 调用 apply()
              → apply_low() → eval_instruction()
                [VM 执行 LPC 字节码]
                → 调用 C efun 或其他 LPC 函数
                → 通过 add_message() 输出到 bufferevent
                → 每条指令检查 outoftime
      → maybe_schedule_user_command() 若还有命令待处理
```

同时,游戏刻系统并行运行:

```
on_game_tick() [libevent timer]
  → call_tick_events()
    → call_heart_beat() [自注册]
      → 对每个心跳对象: call_direct() → eval_instruction()
    → look_for_objects_to_swap() [每 5 分钟]
      → reset_object(), clean_up
    → reclaim_objects() [每 30 分钟]
```

## 核心组件

| 组件 | 文件 | 职责 |
|------|------|------|
| 事件循环 | `src/backend.cc:239` | `event_base_loop(base, 0)` — 阻塞式 libevent 主循环 |
| 游戏刻系统 | `src/backend.cc:79-151` | libevent `evtimer` 驱动虚拟时间, `call_tick_events()` 分发回调 |
| 网络 I/O | `src/comm.cc` | `bufferevent` + 非阻塞 socket, 运行时选择 kqueue/epoll |
| VM 解释器 | `src/vm/internal/base/interpret.cc:1976` | `eval_instruction()` — 同步字节码 `while(true)` + `switch` 调度 |
| 心跳 | `src/packages/core/heartbeat.cc:35` | 所有心跳对象**顺序**执行 `heart_beat()` |
| Call Out | `src/packages/core/call_out.cc` | 延迟函数执行,通过 gametick 或 walltime 事件驱动 |
| 异步 I/O | `src/packages/async/async.cc` | 分离线程执行文件/DB 操作,结果回调主循环 |
| 命令解析 | `src/packages/core/add_action.cc` | 动词→动作匹配, `user_parser()` |

## 线程模型

`init_backend()` (`src/backend.cc:60`) 调用 `evthread_use_pthreads()` 仅为 libevent 内部数据结构线程安全。所有 LPC 代码、网络 I/O、游戏刻处理运行在同一线程。

唯一的多线程组件:
- **异步 I/O 包** (`src/packages/async/async.cc:70-109`): 分离 `std::thread` 执行文件/DB 操作,结果通过 `add_walltime_event(0, check_reqs)` 回调主循环
- **POSIX 定时器** (`src/vm/internal/posix_timers.cc:19-23`): `SIGVTALRM` 信号处理器只设置 `outoftime = 1`,仅 Linux 有效

## 单线程阻塞点详细分析

### 第一类:LPC 执行 — 最根本的架构约束

`eval_instruction()` 是同步 `while(true)` 循环 (`src/vm/internal/base/interpret.cc:2008`):

```cpp
while (true) {
    instruction = EXTRACT_UCHAR(pc++);
    if (outoftime) {
        error("Too long evaluation. Execution aborted.\n");
    }
    switch (instruction) { ... }
}
```

**在任意一次 LPC 评估期间,整个事件循环被完全阻塞:**
- 不处理任何网络事件
- 不处理其他用户命令
- 游戏刻不会触发
- 不接受新连接

**致命问题**: 评估限制仅在 Linux 上生效 (`src/vm/internal/eval_limit.cc:9-15`):

```cpp
void init_eval() {
#ifdef __linux__
    init_posix_timers();
#else
    debug_message("WARNING: Platform doesn't support eval limit!\n");
#endif
}
```

macOS/Windows 上 LPC 死循环会永久挂死驱动。

即使 Linux 上,也是**协作式**而非抢占式——`SIGVTALRM` 信号处理器只设置 `outoftime` 标志,VM 只在**指令边界**检查。

### 第二类:心跳顺序执行

`call_heart_beat()` (`src/packages/core/heartbeat.cc:48-116`) 按 FIFO 队列顺序执行所有心跳:

```cpp
while (!heartbeats.empty()) {
    auto &hb = heartbeats.front();
    // 取出一个对象,执行其 heart_beat()
    call_direct(ob, ob->prog->heart_beat - 1, ORIGIN_DRIVER, 0);
    pop_stack();
}
```

如果某个对象的心跳较慢,所有后续对象的心跳都被延迟。没有公平调度,没有超时拆分。

### 第三类:命令处理序列化

每个命令在 `on_user_command()` (`src/comm.cc:82-108`) 中逐个处理:

```cpp
void on_user_command(...) {
    set_eval(max_eval_cost);
    process_user_command(user);
    maybe_schedule_user_command(user); // 调度下个命令(零延迟)
}
```

设计进行一次命令处理一个后为同用户的下一个命令调度零延迟事件,实现轮转。但**一个用户的慢命令会延迟所有其他用户**,因为零延迟事件排在事件循环的同一次迭代中。

### 第四类:call_out(0) 链式执行

`call_tick_events()` (`src/backend.cc:117-141`) 在 `while(true)` 中处理同刻事件:

```cpp
while (true) {
    // 提取当前刻所有事件
    all_events.clear();
    for (auto iter = iter_start; iter != iter_end; iter++) {
        all_events.push_back(iter->second);
    }
    g_tick_queue.erase(iter_start, iter_end);

    for (auto *event : all_events) {
        if (event->valid) {
            event->callback();  // 若执行了 call_out(0)...
        }
        delete event;
    }
    // callback 可能添加了新同刻事件,继续循环
}
```

`call_out.cc:89-101` 有限制,但达到限制前,所有网络 I/O 和其他游戏刻事件都被阻塞。

### 第五类:同步 I/O 阻塞

| 位置 | 描述 | 阻塞时间 |
|------|------|----------|
| `src/vm/internal/simulate.cc` `load_object()` | `stat()` + `open()` + 编译 LPC | 可能每次数十~数百ms |
| `src/comm.cc:651-666` `flush_message()` SSL | `SSL_write()` 可能触发完整 TLS 握手 | 可能秒级 |
| `read_file()` / `write_file()` | 同步文件系统调用 | 可能数十ms |
| `db_exec()` | 同步数据库查询 | 可能秒级 |
| `query_name_by_addr()` | 同步 DNS 查询 | 可能秒级 |
| `src/comm.cc:1249` `remove_interactive()` | 同步刷新 + 多个 `safe_apply()` | 可能数百ms |

SSL 路径尤其危险 (`src/comm.cc:651-662`):

```cpp
if (ip->ssl) {
    auto *data = evbuffer_pullup(output, len);
    auto wrote = SSL_write(ssl, data, len);  // 可能触发 renegotiation
    evbuffer_unfreeze(output, 1);
    // ...
}
```

### 第六类:5 分钟全量对象扫描

`look_for_objects_to_swap()` (`src/backend.cc:263-357`) 每 5 分钟遍历**所有对象**,对每个对象可能调用 `reset_object()` 和 `clean_up` (执行 LPC 代码):

```cpp
while (true) {
    while ((ob = (object_t *)next_ob)) {
        // ...
        if (...) {
            reset_object(ob);  // 执行 LPC reset()
        }
        // ...
        if (ready_for_clean_up && ...) {
            safe_apply(APPLY_CLEAN_UP, ob, 1, ORIGIN_DRIVER);  // 执行 LPC clean_up()
        }
    }
}
```

这是一个阻塞的同步扫描,运行时无网络事件处理。如果某个 `clean_up` 较慢,整批都被延迟。

### 第七类:异步 I/O 回调堆积

`check_reqs()` (`src/packages/async/async.cc:407-444`) 在主循环上运行,一次性排空整个 `finished_reqs`:

```cpp
void check_reqs() {
    std::lock_guard<std::mutex> const lock(finished_reqs_lock);
    while (!finished_reqs.empty()) {
        auto *req = finished_reqs.front();
        finished_reqs.pop_front();
        switch (req->type) {
            case AREAD: handle_read(req); break;  // 执行 LPC 回调
            case AWRITE: handle_write(req); break;
            // ...
        }
    }
}
```

`handle_read()` 等函数调用 LPC 回调,这可能在主线程上运行任意 LPC 代码。如果某个回调较慢,所有后续完成的 IO 请求回调都被延迟。

同时,`check_reqs()` 通过 `add_walltime_event(0, check_reqs)` 触发,无优先级保障。

### 第八类:shutdown 潜在死循环

`complete_all_asyncio()` (`src/packages/async/async.cc:446-455`):

```cpp
void complete_all_asyncio() {
    while (true) {
        std::lock_guard<std::mutex> const lock(reqs_lock);
        if (reqs.empty()) { break; }
    }
    check_reqs();
}
```

若工作线程因操作卡住而将请求推回队列(`async.cc:100-103`),此循环可能无限自旋。

## 风险排名

| 排名 | 问题 | 文件:行 | 影响 |
|------|------|---------|------|
| 🔴1 | 非 Linux 上无 eval limit | `posix_timers.cc`, `eval_limit.cc` | macOS/Windows 上 LPC 死循环永久挂死驱动 |
| 🔴2 | 心跳顺序执行 | `heartbeat.cc:35-117` | 一个慢心跳延迟所有其他心跳 |
| 🔴3 | 命令序列化 | `comm.cc:82-108` | 一个慢命令阻塞所有网络处理 |
| 🟠4 | call_tick_events() 内部循环 | `backend.cc:117-141` | call_out(0) 链阻塞网络和其他刻事件 |
| 🟠5 | SSL_write 在 flush_message | `comm.cc:651-666` | TLS 重协商阻塞 |
| 🟠6 | 5 分钟全量对象扫描 | `backend.cc:263-357` | 对所有对象同步执行 LPC |
| 🟠7 | complete_all_asyncio() 自旋 | `async.cc:446-455` | 工作线程卡住时 shutdown 可能死循环 |
| 🟠8 | 同步 DB/DB 操作 | `db.cc`, `file.cc` | 文件系统/数据库延迟 |
| ⚪9 | ed efun | `ed.cc` | 内置行编辑器模态阻塞 |
