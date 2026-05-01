# Phase 1: 修复 telnet->z (MCCP 压缩流) 的线程安全

## 问题诊断

### 崩溃现象

两次相关崩溃，共同点都是 `_send()` → `deflate(telnet->z)` → `deflateCopy` 崩溃：

**第一次**（IO 线程）：
```
#8  _pthread_start
#7  IOThread::start()::$_0
#6  event_base_loop
#5  event_process_active_single_queue
#4  IOThread::wakeup_cb
#3  process_user_command(interactive_t*)::$_0   ← GA lambda
#2  _send
#1  libz.1.dylib, deflate
#0  libz.1.dylib, deflateCopy
```
Segmentation fault — GA lambda 在 IO 线程上调用 `telnet_send_ga` → `_send` → `deflate` 崩溃。

**第二次**（VM 线程，revert GA lambda 后）：
```
#10 main → driver_main → backend → event_base_loop
#5  event_once_cb
#4  on_user_command::$_0
#3  process_user_command(interactive_t*)         ← 直接调用
#2  _send
#1  libz.1.dylib, deflate
#0  libz.1.dylib, deflateCopy
```
Bus error — `process_user_command` 在 VM 线程上调用 `set_linemode` → `telnet_negotiate` → `_send` → `deflate` 崩溃。

### 根因

libtelnet 的 `_send()` 函数在调用线程上直接使用 `telnet->z`（zlib 压缩流），但 **zlib 的 `deflate()` 不是线程安全的**。两个线程同时通过 `_send()` 访问同一个 `telnet->z`：

| 线程 | 触发路径 |
|------|---------|
| VM | `process_user_command` → `set_linemode`/`set_charmode` → `telnet_negotiate` → `_send` |
| VM | `process_user_command` → `telnet_send_ga` → `_send` |
| VM | LPC efuns (`telnet_nop`, `telnet_ga`, `send_gmcp`, `send_zmp`, 等) → `_send` |
| IO | `output_to_user` → `telnet_send_text` → `_send` |
| IO | `telnet_recv` → 协商响应 → `_send` |

并发访问 `telnet->z` → 内部状态损坏 → `deflateCopy` 崩溃。

这个 race condition 在 IO 线程引入时就已存在。Phase 0 的 linemode 修复（`\n` 追加、FORWARDMASK 接受）增加了 `process_user_command` 的调用频率，使 race 窗口增大到足以频繁触发。

### 关键约束

1. `_send()` 在调用线程上执行压缩，**然后**才触发 TELNET_EV_SEND 事件回调。事件回调中的跨线程分发（copy + post）无法保护压缩操作本身。
2. libtelnet 的 `telnet_t` 结构体定义在 `.c` 文件内部，不能从外部访问 `telnet->z`。
3. 不需要锁的设计是可能的：只要所有 `_send()` 调用发生在同一个线程，就不存在并发。

## 方案

**方案 B — 统一路由到 IO 线程**

将所有 libtelnet 发送操作的调用统一到 IO 线程执行。

- 无需修改 libtelnet 核心逻辑
- 无锁设计，无性能损失
- 架构一致性：网络操作都在 IO 线程
- IO 线程的任务队列天然 FIFO，保证操作顺序

实现方式：为 libtelnet 增加 `telnet_get_userdata()` 公共 API，使 mudos 层的包装函数能获取 `interactive_t *`，从而判断是否需要 post 到 IO 线程。

## 实际修改清单

### 1. libtelnet — 新增公共 API

**`src/thirdparty/libtelnet/libtelnet.h`** — 声明：
```c
extern void *telnet_get_userdata(telnet_t *telnet);
```

**`src/thirdparty/libtelnet/libtelnet.c`** — 实现（3 行）：
```c
void *telnet_get_userdata(telnet_t *telnet) {
    return telnet->ud;
}
```

### 2. `src/net/telnet.cc` — 包装函数

所有以 `struct telnet_t *` 为参数的包装函数，改为通过 `telnet_get_userdata()` 获取 `ip`，非 IO 线程时 post 到 IO 线程：

| 函数 | 改前 | 改后 |
|------|------|------|
| `telnet_send_ga` | 直接 `telnet_iac` | post 到 IO 线程 |
| `telnet_send_nop` | 直接 `telnet_iac` | post 到 IO 线程 |
| `telnet_do_naws` | 直接 `telnet_negotiate` | post 到 IO 线程 |
| `telnet_dont_naws` | 直接 `telnet_negotiate` | post 到 IO 线程 |
| `telnet_start_request_ttype` | 直接 `telnet_negotiate` | post 到 IO 线程 |
| `telnet_request_ttype` | 直接 `telnet_begin_sb` | post 到 IO 线程 |

```cpp
// 统一模式
void telnet_send_ga(struct telnet_t *telnet) {
    if (!telnet) return;
    auto *ip = static_cast<interactive_t *>(telnet_get_userdata(telnet));
    if (ip->io_thread && !ip->io_thread->is_current_thread()) {
        ip->io_thread->post([telnet]() { telnet_iac(telnet, TELNET_GA); });
    } else {
        telnet_iac(telnet, TELNET_GA);
    }
}
```

### 3. `src/net/telnet.cc` — set_linemode / set_charmode / set_localecho

将 telnet 操作和 `flush_message` 合并为一个 lambda post 到 IO 线程。

**关键细节**：`SUPPRESS_GA` 标志位必须在 VM 线程上立即设置（`process_user_command` 后续检查此标志），不能延迟到 IO 线程。所以 `need_sga` 在 VM 线程计算并设置标志，仅 telnet 协商延迟到 IO：

```cpp
void set_linemode(interactive_t *ip, bool flush) {
    if (!ip->telnet) return;
    if (ip->iflags & USING_LINEMODE) {
        bool need_sga = !(ip->iflags & SUPPRESS_GA);
        if (need_sga) ip->iflags |= SUPPRESS_GA;  // 立即在 VM 线程设置

        if (ip->io_thread && !ip->io_thread->is_current_thread()) {
            ip->io_thread->post([ip, flush, need_sga]() {
                telnet_negotiate(ip->telnet, TELNET_DO, TELNET_TELOPT_LINEMODE);
                telnet_subnegotiation(ip->telnet, ...);  // MODE_EDIT | MODE_TRAPSIG
                if (need_sga) telnet_negotiate(ip->telnet, TELNET_WILL, TELNET_TELOPT_SGA);
                if (flush) flush_message(ip);
            });
        } else { /* 原有逻辑 */ }
    } else { /* WONT SGA 路径，同样 post */ }
}
```

### 4. `src/net/msp.cc` — MSP 发送

- `telnet_send_msp_oob`：复制 msg 数据，post telnet_subnegotiation 到 IO 线程
- `on_telnet_dont_msp`：post telnet_negotiate 到 IO 线程；标志位 `USING_MSP` 在 VM 线程立即清除

### 5. `src/packages/core/telnet_ext.cc` — LPC efuns

所有调用 libtelnet 发送函数的 efuns，分两类处理：

**简单 efuns**（调用的包装函数已自动路由）：`f_telnet_nop`、`f_telnet_ga`、`f_request_term_type`、`f_start_request_term_type`、`f_request_term_size`、`f_act_mxp`、`f_telnet_msp_oob`

这些 efuns 调用的 mudos 包装函数（`telnet_send_ga`、`telnet_do_naws` 等）已在步骤 2 中自动路由。只需额外处理 `flush_message` 也 post 到 IO 线程。

**复杂 efuns**（直接调用 libtelnet 函数）：`f_send_gmcp`、`f_send_msdp_variable`、`f_send_zmp`

这些 efuns 直接在代码中调用 `telnet_subnegotiation`、`telnet_send`、`telnet_begin_zmp` 等 libtelnet 函数。需要先在 VM 线程上提取 LPC 栈数据，再 post 到 IO 线程执行 telnet 操作：

- `f_send_gmcp`：提取转码后的字符串，post 单个 `telnet_subnegotiation` + `flush_message`
- `f_send_zmp`：提取 cmd 和 args 到 `std::vector<std::string>`，post 整个 `begin_zmp → loop(arg) → finish_zmp → flush`
- `f_send_msdp_variable`：提取 var_name 和所有可能类型的值（string/number/real/buffer），post `begin_sb → switch(type) → finish_sb → flush`

### 6. `src/comm.cc` — process_user_command

GA 发送恢复为直接调用 `telnet_send_ga(ip->telnet)`。IO 线程路由由 `telnet_send_ga()` 内部处理。

### 不需要修改的地方（已在 IO 线程）

- `output_to_user()` → `telnet_send_text()` — 已通过 `add_message()` post 到 IO 线程
- telnet 事件回调链 — 由 `telnet_recv()` 在 IO 线程触发
- `send_initial_telnet_negotiations()` — 在 `io->post()` 回调中调用
- `on_telnet_connect()` 中的各种 `telnet_negotiate` — 同样在 IO 线程回调中

## 线程模型总结

修改后的线程分工：

```
VM 线程（g_event_base）:
  └─ 执行 LPC 代码
  └─ process_user_command
  └─ 标志位操作（SUPPRESS_GA, USING_MSP 等）
  └─ 数据提取（从 LPC 栈读取字符串/数值）
  └─ io_thread->post(lambda)  ← 调度 telnet 操作

IO 线程（io_thread_pool）:
  └─ telnet_recv → _process → 协商响应 → _send → deflate(telnet->z)
  └─ output_to_user → telnet_send_text → _send → deflate(telnet->z)
  └─ 执行 post 的 lambda:
       ├─ telnet_negotiate → _send → deflate(telnet->z)
       ├─ telnet_subnegotiation → _send → deflate(telnet->z)
       ├─ telnet_send → _send → deflate(telnet->z)
       └─ flush_message → bufferevent_flush
```

所有 `telnet->z` 访问仅发生在 IO 线程，不存在并发。

## 编译验证

Release 模式构建通过：
```bash
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(sysctl -n hw.ncpu) install
```

Debug 模式存在预存问题（`F_BREAK_POINT` 未定义，与本次修改无关）。
