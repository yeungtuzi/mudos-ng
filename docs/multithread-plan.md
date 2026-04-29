<!--
Copyright (c) 2026 [大河马/dahema@me.com]
SPDX-License-Identifier: MIT
-->

# MudOS-NG 多线程异步架构改造计划

## 背景

MudOS-NG 当前是纯单线程架构(基于 libevent `event_base_loop`),所有网络 I/O、LPC 执行、心跳、call_out、垃圾回收全部在同一线程上运行。当任何 LPC 代码执行时(包括命令处理、心跳、call_out),整个事件循环被完全阻塞,无法响应网络事件或其他操作。

目标:改造为非阻塞多线程异步架构,让心跳、call_out 等操作可以异步执行,同时保留游戏刻的时序语义。

## 设计约束(来自代码分析)

### 必须保护的全局状态

约 40+ 个全局变量需要变为线程安全:

| 类别 | 变量示例 | 方案 |
|------|---------|------|
| VM 寄存器 | `pc`, `sp`, `fp`, `csp` | thread_local |
| 执行上下文 | `current_object`, `command_giver`, `current_interactive` | thread_local |
| 执行栈 | `_stack[]`, `_control_stack[]` | thread_local(每线程独立栈) |
| 错误处理 | `current_error_context`, `catch_value` | thread_local |
| 引用栈 | `command_giver_stack[]`, `cgsp` | thread_local |
| 对象列表 | `obj_list`, `obj_list_destruct` | 互斥锁(std::shared_mutex) |
| 特殊对象 | `master_ob`, `simul_efun_ob` | 读写锁(读多写少) |
| 调度队列 | `g_tick_queue`, `g_callout_handle_map` | 互斥锁 |
| 心跳 | `heartbeats`, `heartbeats_next` | 互斥锁 |
| 引用计数 | `object_t.ref`, `program_t.ref`, 容器 ref | std::atomic |
| 对象标志 | `object_t.flags` | std::atomic |
| 评估限制 | `outoftime`, `max_eval_cost` | std::atomic + 每线程定时器 |

### 关键约束

1. **游戏刻时序**:心跳、call_out、清理等事件在游戏刻边界触发,call_out(0) 允许同刻链式调用
2. **两阶段销毁**:O_DESTRUCTED 标志(阶段1)与实际内存释放(阶段2)分离
3. **全局 save/restore 模式**:command_giver 等上下文在每次 LPC 调用时 save 和 restore
4. **错误处理**:C++ exception 跨 LPC 调用帧抛出,被 safe_apply/safe_call_function_pointer 捕获
5. **eval limit**:Linux 上用 SIGVTALRM(进程级),不支持多线程

## 内存占用基线(当前,64位)

了解当前内存占用对分布式设计的规模估算至关重要:

| 结构 | 大小 | 100M 对象的内存占用 |
|------|------|--------------------|
| `object_t` 固定开销 | ~240 字节 | 24 GB |
| 每个变量(svalue_t) | 16 字节 | ~24 GB(典型 30 变量对象) |
| `program_t`(典型) | 10-50 KB | 被克隆共享,不按对象计数 |
| `heart_beat_t` | 16 字节 | 1.6 GB |
| `interactive_t` | ~1 MB | 每个在线玩家 1 MB |
| `sentence_t` | 48 字节 | 每个房间绑定数 KB |
| `pending_call_t` | ~72 字节 | 视 call_out 数量 |
| `svalue_t`(栈) | 16 字节 | 每栈 ~256 KB |

**关键结论:100M 对象单机需要 50GB+ 以上 RAM,必须分布式。**

## 四阶段改造计划

### 第一阶段:VM 线程 + I/O 线程(3-4个月,推荐优先实施)

将网络 I/O 移至专用 I/O 线程,VM/游戏逻辑保持单线程。这是**风险最低、收益最快的方案**。

#### 架构图

```
┌─────────────────────┐    Lock-free Queue    ┌─────────────────────┐
│   I/O Thread(s)     │ ◄────────────────── ► │    VM Thread        │
│                     │    user command req   │                     │
│  ┌───────────────┐  │    output messages    │  ┌───────────────┐  │
│  │ libevent loop  │  │                      │  │ libevent loop  │  │
│  │ bufferevent    │  │                      │  │ game tick      │  │
│  │ evconnlistener │  │                      │  │ heartbeat      │  │
│  │ TLS/WebSocket  │  │                      │  │ call_out       │  │
│  └───────────────┘  │                      │  │ LPC eval       │  │
│                     │                      │  │ GC/reclaim     │  │
└─────────────────────┘                      │  └───────────────┘  │
                                             └─────────────────────┘
```

#### 具体步骤

**1.1 基础设施**
- `src/backend.cc`:调用 `evthread_make_base_notifiable(g_event_base)`,启用跨线程事件注入
- 新增 `src/base/internal/io_thread.h/.cc`:IOThread 类,自带 event_base 和无锁队列
- 新增依赖:`moodycamel::ConcurrentQueue` 或 `boost::lockfree::queue`

**1.2 I/O 线程实现**
- IOThread 管理一个 libevent event_base、无锁 SPSC 任务队列、eventfd 唤醒机制
- 支持 `post(std::function<void()>)` — 跨线程安全非阻塞
- 支持通过 `num_io_threads` 配置(默认 2)创建线程池
- 新连接通过 `io_thread->post()` 路由到 I/O 线程的 event_base

**1.3 用户连接拆分**
- `interactive_t::ev_buffer` 和 `ev_command` 移到 I/O 线程的 event_base
- `new_conn_handler()`接受连接后,通过 I/O 线程创建 bufferevent
- `on_user_read()`/`on_user_write()`/`on_user_events()`在 I/O 线程上触发

**1.4 命令队列(I/O→VM)**
- 当用户有完整命令时,通过无锁 MPSC 队列 `user_command_queue` 发送到 VM 线程
- VM 线程在每次游戏刻迭代或专用 event 中排空队列,批量处理用户命令
- 每个命令仍然在 VM 线程上顺序执行,保持 `command_giver` save/restore 模式不变

**1.5 输出队列(VM→I/O)**
- `add_message()`改为消息入队,而非直接写 bufferevent
- I/O 线程在每次事件循环迭代排空所有用户的输出队列

**1.6 特殊处理**
- `remove_interactive()`:两阶段跨线程清理(VM 标记+ I/O 释放)
- `flush_message()`:通过 I/O 线程 post 触发
- DNS/WebSocket 回调:通过 event_base_once 路由回 VM 线程

#### 涉及文件

| 文件 | 修改 |
|------|------|
| `src/backend.cc`, `src/backend.h` | 添加跨线程通信、命令队列接口 |
| `src/comm.cc`, `src/comm.h` | 拆分连接接收和 I/O 处理,消息队列 |
| `src/interactive.h` | 添加输出队列字段,线程归属标记 |
| `src/user.cc` | 线程安全 users 列表操作 |
| `src/mainlib.cc` | 启动 I/O 线程池 |
| `src/packages/core/dns.cc` | 回调路由到 VM 线程 |
| 新增: `src/base/internal/io_thread.h/.cc` | I/O 线程池实现 |

#### 第一阶段成果

- ✅ 网络 I/O 延迟不再阻塞 LPC 执行
- ✅ 用户断开连接等事件不再影响当前命令
- ✅ 游戏刻时序语义完全不变
- ❌ 心跳、call_out 仍单线程顺序执行
- ❌ LPC 密集型场景(大量心跳)仍可能卡住

---

### 第二阶段:纤程协作式调度(6-12个月)

在第一阶段基础上,引入纤程(fiber)将 LPC 执行协作式多任务化。心跳、call_out、用户命令各自运行在独立的纤程中,在 LPC 函数调用边界主动让出。

#### 架构图

```
VM Thread
  ├── libevent 事件循环
  ├── I/O 通信(从第一阶段)
  │
  └── Fiber Scheduler
        ├── Fiber 1: 用户 A 的命令链
        ├── Fiber 2: 用户 B 的命令链
        ├── Fiber 3: 心跳(按对象分纤程)
        ├── Fiber 4: call_out 执行
        ├── Fiber 5: 对象 swap/cleanup
        └── ...
```

#### 具体步骤

**2.1 纤程库集成**
- 选择:Boost.Coroutine2 或轻量级 libco
- 每个纤程 256KB 栈+自己的 VM 寄存器副本
- `Fiber`类:entry_point、栈空间、上下文、SavedVMState

**2.2 VM 寄存器纤程化**
- 约 40+ 全局变量 → `VMContext` 结构体
- `Fiber::current->ctx->sp` 代替全局 `sp`
- 纤程切换时保存/恢复整个 VMContext
- 通过宏简化替换:`#define sp (Fiber::current->ctx->sp)`

**2.3 让出点(Yield Point)**
- `call_direct()`/`call_function_pointer()`:LPC 调用另一个 LPC 函数时让出
- `eval_instruction()`循环:每次指令后检查是否需要让出
- `safe_apply()`/`safe_call_function_pointer()`:天然边界
- 每个心跳对象执行完毕后让出
- 每个 call_out 执行完毕后让出

**2.4 调度器**
- `FiberScheduler`:ready_queue(就绪队列)、blocked_queue(阻塞队列)
- 时间片轮转调度
- 支持优先级(用户命令 > 心跳 > call_out > 清理)

**2.5 游戏刻集成**
- `call_heart_beat()`为每个心跳对象创建纤程,不阻塞立即返回
- `call_tick_events()`中的事件以纤程方式执行
- 调度器在每次纤程让出后重新调度

#### 第二阶段成果

- ✅ 长时间 LPC 不再阻塞整个驱动
- ✅ 心跳、call_out 真正"异步"执行
- ✅ 响应性显著提升(一个慢命令不再卡住所有用户)
- ❌ 仍然是单线程,不能利用多核
- ⚠️ 需要仔细处理 call_out(0) 同刻链式调用的时序

---

### 第三阶段:对象分片实现多核并行(12-24个月)

在第二阶段纤程化的基础上,将 VM 扩展为多个 Worker 线程,每个线程维护一个**对象分片(Shard)**。每个分片拥有自己的纤程调度器、对象子集、游戏刻时钟和 event_base。

#### 核心架构:分区对象模型

```
┌───────────┐  ┌───────────┐  ┌───────────┐  ┌───────────┐
│ Shard 0   │  │ Shard 1   │  │ Shard 2   │  │ Shard 3   │
│ CPU 0     │  │ CPU 1     │  │ CPU 2     │  │ CPU 3     │
│           │  │           │  │           │  │           │
│ obj_list  │  │ obj_list  │  │ obj_list  │  │ obj_list  │
│ heartbts  │  │ heartbts  │  │ heartbts  │  │ heartbts  │
│ call_outs │  │ call_outs │  │ call_outs │  │ call_outs │
│ TLS regs  │  │ TLS regs  │  │ TLS regs  │  │ TLS regs  │
│ fiber sched│  │ fiber sched│  │ fiber sched│  │ fiber sched│
└─────┬─────┘  └─────┬─────┘  └─────┬─────┘  └─────┬─────┘
      │               │               │               │
      └───────────────┴───────┬───────┴───────────────┘
                              │
                     [Cross-Shard Message Bus]
                     (shared memory, lock-free queues)
                              │
                     [ I/O Thread Pool ]
                              │
                     [ Network Layer ]
```

#### 分区策略:房间分组

MUD 世界天然以"房间(room)"为基本单元。分片规则:

```
分区键 = hash(区域名称) % 分片数
```

**归属规则(同一分片必须在一起的):**
- 房间 + 该房间内的所有对象(玩家、NPC、物品)
- Shadow 链(base + shadowers)
- Interactive 会话 + 其绑定的玩家对象
- 同一玩家对象的所有 call_out

**跨分片引用:通过远程对象代理:**

```cpp
// 统一对象引用(兼容本地和远程)
union object_ref {
    object_t *local;        // 同一分片内,直接指针
    struct {                 // 跨分片引用
        uint16_t shard_id;  // 目标分片
        uint64_t oid;       // 目标分片上的对象 ID
        // 缓存本地代理对象指针(若已解析)
        object_t *proxy;    // 远程对象的本地代理
    } remote;
    bool is_local() const;
};
```

#### 跨分片消息总线

```
struct ShardMessage {
    enum Type {
        CALL_FUNCTION,     // 跨分片调用函数
        DESTRUCT_OBJECT,   // 对象销毁通知
        MOVE_TO_SHARD,     // 对象迁移到本分片
        UPDATE_REFCOUNT,   // 引用计数变更
        LIVING_LOOKUP,     // 跨分片 living 查询
        HEARTBEAT_DONE,    // 心跳完成通知
        GAMETICK_SYNC,     // 游戏刻同步
    };
    Type type;
    uint64_t target_oid;
    uint32_t target_shard;
    std::vector<svalue_t> args;  // 序列化参数
    std::function<void(svalue_t*)> callback;  // 异步结果回调
};
```

每对分片之间有两组无锁 MPSC 队列(双向)。跨分片调用变成异步消息:

1. Shard A 打包消息 → 发送到 Shard B 的 inbox 队列
2. Shard B 在其空闲时处理消息 → 执行 LPC 调用
3. 结果通过 callback/promise 返回 Shard A

#### 游戏刻同步

每个分片有自己的 `g_current_gametick`,但通过 NTP 风格同步协议保持松散一致:

```
1. Leader 分片(分片 0)每 N 个 tick 广播同步信号
2. 所有分片在同步点对齐 tick 计数
3. 跨分片 call_out(0) 使用 sender 的 tick + round-trip delay
```

#### 涉及文件(第三阶段)

| 文件 | 修改 |
|------|------|
| `src/vm/internal/base/object.h` | 对象引用改为 union(本地/远程) |
| `src/vm/internal/simulate.cc` | obj_list → 每个分片独立 |
| `src/vm/internal/otable.cc` | ObjectTable → 分片本地 + 全局注册 |
| `src/packages/core/heartbeat.cc` | 心跳队列分片化 |
| `src/packages/core/call_out.cc` | 跨分片 call_out 路由 |
| `src/packages/core/add_action.cc` | hashed_living 分片化 |
| `src/backend.cc` | 跨分片消息总线、游戏刻同步 |
| 新增: `src/base/internal/shard.h/.cc` | 分片管理器 |
| 新增: `src/base/internal/shard_message.h/.cc` | 跨分片消息定义、序列化 |
| 新增: `src/base/internal/cross_shard_rpc.h/.cc` | 跨分片 RPC 实现 |

#### 第三阶段成果

- ✅ 多核并行:每个核心独立执行 LPC
- ✅ 心跳、call_out、命令处理并行化
- ✅ 水平扩展:增加分片数 = 增加吞吐量
- ❌ 单机规模限制(内存、核心数)
- ❌ 跨分片 RPC 增加延迟(同机抖动 < 1μs,但仍不可忽略)

---

### 第四阶段:分布式集群(12-24个月,与第三阶段可并行推进)

在第三阶段单机多分片的基础上,扩展到多台物理机。每台机器运行一个 MudOS-NG 实例(节点),每个节点管理多个分片。节点之间通过网络进行通信。

#### 核心架构:联邦集群

```
                        ┌─────────────────────┐
                        │   Global Registry    │
                        │  (Service Discovery) │
                        │  node → zone mapping │
                        │  player location DB  │
                        └──────────┬──────────┘
                                   │
       ┌───────────────────────────┼───────────────────────────┐
       │                           │                           │
┌──────┴──────┐           ┌──────┴──────┐           ┌──────┴──────┐
│  Node A     │           │  Node B     │           │  Node C     │
│  CPU 0-7   │           │  CPU 0-7   │           │  CPU 0-7   │
│            │           │            │           │            │
│ ┌────────┐ │           │ ┌────────┐ │           │ ┌────────┐ │
│ │Shard 0 │ │  TCP/gRPC │ │Shard 2 │ │           │ │Shard 4 │ │
│ │ 房间 1-100│◄├─────────┤ │ 房间201-│ │           │ │ 房间401-│ │
│ │        │ │           │ │ 400    │ │           │ │ 600    │ │
│ ├────────┤ │           │ ├────────┤ │           │ ├────────┤ │
│ │Shard 1 │ │           │ │Shard 3 │ │           │ │Shard 5 │ │
│ │ 房间 101│ │           │ │ 房间 301│ │           │ │ 房间 501│ │
│ │ -200   │ │           │ │ -400   │ │           │ │ -600   │ │
│ └────────┘ │           │ └────────┘ │           │ └────────┘ │
└────────────┘           └────────────┘           └────────────┘
```

#### 全局对象寻址系统

每个对象分配一个**全局唯一 128 位 OID**:

```
┌──────────┬──────────────┬──────────────────────────────────┐
│ node_id  │  shard_id    │          local_object_id         │
│ (16 bit) │  (16 bit)    │          (96 bit)                │
└──────────┴──────────────┴──────────────────────────────────┘
```

- `node_id`:节点 ID,启动时从全局注册中心分配
- `shard_id`:节点内部分片 ID
- `local_object_id`:分片内自增 ID(96 位足够宇宙永不回绕)

**全局注册中心**:独立的轻量级服务,维护:
- 节点在线状态和负载
- Object ID → 节点/分片映射(缓存)
- 玩家当前位置(节点+房间)
- 区域所有权(哪个节点负责哪些区域)

#### 跨节点对象引用

```cpp
// 全局对象引用(网络透明)
class GlobalObjectRef {
    uint128_t oid;       // 全局唯一 ID
    // 本地缓存(快速路径)
    object_t *cached_local;  // 如果对象在本节点
    // 远程代理(如果对象在另一节点)
    RemoteProxy *proxy;      // 包含节点地址、序列化方法
};
```

**远程代理对象(RemoteProxy):**
- 对 LPC 透明:实现了 `object_t` 接口的子集
- 任何方法调用 → 序列化为网络 RPC 请求
- 缓存常用属性(如 obname、flags)减少 RPC
- 引用计数通过引用计数消息同步(批量化)

#### 跨节点 RPC 协议

```
Protocol: TCP (长连接,连接池) 或 QUIC (实验)
Serialization: Protocol Buffers / FlatBuffers (高效二进制)
```

消息类型:

| 类型 | 方向 | 用途 |
|------|------|------|
| `CALL_FUNCTION` | 双向 | 跨节点 LPC 函数调用 |
| `MOVE_PLAYER` | 请求/应答 | 玩家移动到另一个节点的房间 |
| `OBJECT_LOOKUP` | 请求/应答 | 通过 OID 查找对象位置 |
| `REFCOUNT_UPDATE` | 单向 | 远程对象引用计数变更 |
| `HEARTBEAT_QUOTA` | 请求/应答 | 心跳对象迁移协商 |
| `GAMETICK_SYNC` | 广播 | 游戏刻同步 |
| `HANDSHAKE` | 双向 | 玩家节点间无缝切换 |
| `BROADCAST` | 广播 | 全局广播(如 shout) |
| `ZONE_TRANSFER` | 请求/应答 | 区域所有权转移(动态负载均衡) |

#### 玩家跨节点移动协议

```
[Player in Node A, Room 100]
                  │
玩家 say "east"   │
                  │
[Node A 检查 Room 101 属于 Node B]
                  │
1. Node A → Node B: MOVE_PLAYER 请求
   (携带玩家对象序列化数据 + 会话上下文)
                  │
2. Node B 在本地创建玩家的 RemoteProxy
   (或创建完整本地对象副本 + 从 A 迁移交互会话)
                  │
3. Node B → Node A: MOVE_PLAYER 确认
   (携带 Node B 上的新对象引用)
                  │
4. Node A:
   - 输出重定向到 Node B
   - 玩家本地对象降级为 RemoteProxy
   - 将玩家交互会话迁移到 Node B
                  │
5. Node B 接管玩家的命令处理
```

#### 亿级心跳调度系统

100M 对象有心跳是巨大的挑战。即使每个心跳耗时 1μs,单线程也需要 **100 秒**才能完成一轮。需要多级调度:

```
心跳分类:
  High-Frequency (快速):  每 1-10 ticks    (战斗 NPC)
  Normal (普通):          每 5-60 ticks    (常规 NPC、玩家)
  Low-Frequency (低频):   每 60-600 ticks  (房间效果、环境)
  Event-Driven (事件):    仅事件触发        (地图装饰、等待玩家)

调度:
  每 tick: High-Freq → 最多 1000 个心跳
  每 5 ticks: Normal → 当前分片的 N% 对象
  每 60 ticks: Low-Freq → 批量扫描
  Event: 由消息队列驱动,不占 tick 预算
```

**动态心跳预算:**

```
每个分片每 tick 的心跳处理时间: ~5ms (目标: < 10ms)
如果心跳队列过长:
  → High-Freq 优先保证
  → Normal 均匀分布在多个 tick
  → Low-Freq 延后
  → 触发负载告警,建议迁移对象到新节点
```

**非活跃对象降级:**

```cpp
// 对象经过 N 个 tick 无任何操作后:
状态: Active → Idle → Swapped

Idle: 对象在内存,但不参与心跳队列(无 heart_beat_set)
      如果被外部访问,恢复 Active

Swapped: 对象序列化到外部存储(Redis/SSD)
         持有 metadata: obname、位置、最后活动时间
         被访问时: 发消息到所在节点请求加载
```

#### 全局服务集群

```
┌─────────────────────────────────────────────────────────────┐
│                    Global Services                           │
│                                                             │
│  ┌────────────┐  ┌────────────┐  ┌────────────┐            │
│  │ Registry   │  │ Chat       │  │ Economy    │            │
│  │ 节点发现   │  │ 跨节点聊天  │  │ 全局经济    │            │
│  │ 对象定位   │  │ 频道/私信  │  │ 拍卖行/交易 │            │
│  └────────────┘  └────────────┘  └────────────┘            │
│                                                             │
│  ┌────────────┐  ┌────────────┐  ┌────────────┐            │
│  │ Persistence│  │ Auth       │  │ Analytics  │            │
│  │ 玩家数据存储│  │ 登录认证   │  │ 实时监控    │            │
│  │ 存档/恢复  │  │ 账户管理   │  │ 负载统计    │            │
│  └────────────┘  └────────────┘  └────────────┘            │
└─────────────────────────────────────────────────────────────┘
```

这些全局服务可以:
- 用独立的轻量级服务器实现(C++/Go/Rust)
- 通过 gRPC/Thrift 与游戏节点通信
- 水平扩展无状态服务(如 Chat 可加节点)
- 使用一致性哈希做状态服务分区(如 Registry)

#### 数据持久化

```cpp
// 对象持久化格式(Protobuf)
message SerializedObject {
    uint128_t oid = 1;
    string obname = 2;           // 蓝图路径
    string zone = 3;             // 所属区域
    uint128_t location = 4;      // 所在房间 OID
    repeated Variable variables = 5;  // LPC 变量
    repeated Sentence sentences = 6;  // 命令绑定
    HeartbeatConfig heartbeat = 7;    // 心跳配置
    uint64_t last_active_tick = 8;    // 最后活动 tick
    bytes program_bytecode = 9;       // 仅自定义对象
}
```

**存储后端选择:**
- **活跃对象**:Redis/Memcached(内存,快速访问)
- **非活跃对象**:SSD/RocksDB(本地持久化)
- **存档**:S3/分布式文件系统(长期存储)
- **关系数据**:PostgreSQL(玩家账户、经济)

#### 涉及文件(第四阶段)

| 组件 | 实现 |
|------|------|
| 全局注册中心 | 新服务: `cluster/registry_server.cc` |
| 跨节点 RPC | `src/base/internal/cluster_rpc.h/.cc` |
| 对象序列化 | `src/base/internal/object_serializer.h/.cc` |
| 远程代理 | `src/vm/internal/base/remote_proxy.h/.cc` |
| 持久化客户端 | `src/base/internal/persistence.h/.cc` |
| 动态心跳调度 | `src/packages/core/heartbeat_scheduler.h/.cc` |
| 对象降级引擎 | `src/vm/internal/object_swapper.h/.cc` |
| 区域负载均衡器 | `cluster/load_balancer.cc` |

#### 第四阶段成果

- ✅ 理论上无限水平扩展
- ✅ 100M+ 心跳对象的可调度性
- ✅ 地理分布减少延迟
- ❌ 极大的工程复杂度
- ❌ 跨节点 RPC 延迟(网络往返)

---

## 规模估算:100M 对象分布式部署

### 硬件需求

| 配置 | 单节点能力 | 需要节点数 |
|------|-----------|-----------|
| 64GB RAM, 16 核 | 500 万对象 + 8 分片 | 20 台 |
| 256GB RAM, 64 核 | 2000 万对象 + 32 分片 | 5 台 |
| 云集群(8 台 64 核) | 全部 1 亿对象 | 8 台 |

### 心跳带宽

假设心跳分级调度,每心跳 10μs LPC 开销:

```
100M 对象,其中:
  1% High-Freq (1秒间隔):       1M 对象, 10秒/tick → 100核并行
  10% Normal (5秒间隔):        10M 对象, 100秒/tick → 分布到 100 核
  30% Low-Freq (60秒间隔):     30M 对象, 300秒/tick → 分布到 300 核
  59% Event-Driven (无 tick):  59M 对象, 不占用 tick
```

### 网络带宽

```
1000 在线玩家,每人每秒 1KB 进出:
  上行: 8 Mbps   下行: 8 Mbps   → 非常低

跨节点 RPC (假设每秒 10000 次调用,每次 1KB):
  10000 × 1KB × 2 = 160 Mbps

总网络:约 200 Mbps — 单台 1GbE 足够承载节点间流量
```

---

## 全周期风险矩阵

| 阶段 | 风险 | 概率 | 影响 | 缓解 |
|------|------|------|------|------|
| 1 | I/O 线程与 VM 线程竞争 cache | 中 | 性能下降 | 线程绑核(cpu pinning) |
| 2 | fiber 栈内存爆炸 | 低 | OOM | 栈池复用,合理上限 |
| 2 | C++ exception 跨 fiber | 中 | 未定义行为 | 仅在 `while(true)` 头部让出 |
| 3 | 跨分片 RPC 死锁 | 中 | 全线卡住 | 超时 + 熔断 + 因果序检查 |
| 3 | 对象迁移状态不一致 | 高 | 数据损坏 | 两阶段提交 + 幂等性设计 |
| 4 | 网络分区导致双写 | 高 | 数据不一致 | 基于注册中心的租约机制 |
| 4 | 远程 RPC 延迟波动 | 高 | 玩家体验差 | 预测性预加载 + 客户端缓冲 |
| 4 | 分布式调试复杂度 | 高 | 开发效率 | 全链路追踪(opentelemetry) |

## 跨阶段公共基础设施

### 1. 对象标识系统(所有阶段共用)

```cpp
// 从单机指针逐步演进到全局 128 位 OID
// 第一阶段: object_t* (不变)
// 第二阶段: object_t* + uint64_t local_id
// 第三阶段: object_t* + ShardRef{shard_id, oid}
// 第四阶段: GlobalOID{node_id, shard_id, local_id}
```

### 2. 序列化框架

从第二阶段就开始引入,逐步完善的序列化能力:

```
阶段1: 无序列化需求
阶段2: 纤程切换时 VMContext 序列化/反序列化
阶段3: 跨分片消息序列化(svalue_t、object_ref)
阶段4: 跨节点消息序列化(Protobuf/FlatBuffers)
阶段5: 对象持久化(完整对象状态 -> 存储)
```

### 3. 遥测和可观测性

从第一阶段就要加入:

```cpp
// 每个 shard/节点 暴露指标:
- ShardLoad: 当前 tick 的处理时间
- HeartbeatQueueDepth: 待处理心跳数
- CrossShardRPCLatency: 跨分片 RPC 延迟
- CommandLatency: 命令处理延迟 P50/P95/P99
- ObjectCount: 对象数统计
- MemoryUsage: 内存使用
```

通过 Prometheus + Grafana 或 OpenTelemetry 收集,用于自动负载均衡和容量规划。

## 推荐实施路径

```
当前
 │
 ├── 0-3 个月: 第一阶段 — I/O 线程分离
 │   (解决网络阻塞,低风险高收益)
 │
 ├── 4-12 个月: 第二阶段 + 基础设施
 │   ├── 纤程化核心 VM(git merge 到 master)
 │   ├── 原子化引用计数
 │   ├── 可观测性基础设施
 │   └── 并行执行 1 万个心跳(单线程验证)
 │
 ├── 12-24 个月: 第三阶段 — 多核并行
 │   ├── 对象分片实现
 │   ├── 跨分片消息总线
 │   ├── 游戏刻同步
 │   └── 8 核单机承载 1000 万对象
 │
 └── 18-36 个月: 第四阶段 — 分布式集群
     ├── 全局注册中心服务
     ├── 跨节点 RPC + 代理对象
     ├── 动态心跳分级调度
     ├── 玩家跨节点无缝迁移
     ├── 对象持久化 + 加载/卸载
     └── 8 节点承载 1 亿对象
```

## 关键文件清单

### 现有文件(需要修改)

| 文件 | 阶段 | 修改内容 |
|------|------|----------|
| `src/vm/internal/base/interpret.cc` | 2,3,4 | VM 寄存器纤程化 + 分片化 |
| `src/vm/internal/base/machine.h` | 2,3,4 | 全局变量 thread_local |
| `src/vm/internal/simulate.cc` | 1,2,3,4 | 对象列表、对象查找、交互上下文 |
| `src/backend.cc` | 1,2,3,4 | I/O 线程、纤程调度、分片消息、游戏刻 |
| `src/comm.cc` | 1 | I/O 线程通信 |
| `src/interactive.h` | 1,4 | I/O 队列、远程会话 |
| `src/vm/internal/base/object.h` | 3,4 | 对象引用 union + OID |
| `src/vm/internal/base/object.cc` | 2,3,4 | 引用计数原子化、分片归属 |
| `src/packages/core/heartbeat.cc` | 2,3,4 | 纤程化、分级调度、分片化 |
| `src/packages/core/call_out.cc` | 2,3,4 | 跨分片/节点路由 |
| `src/packages/core/add_action.cc` | 3,4 | living 表分片化 |
| `src/vm/internal/otable.cc` | 3,4 | 跨节点对象查找 |
| `src/vm/internal/eval_limit.cc` | 1,2 | 多线程 eval limit |
| `src/vm/internal/posix_timers.cc` | 1,2,3 | 每线程定时器 |
| `src/mainlib.cc` | 1,3,4 | I/O 线程、分片、集群启动 |

### 新文件

| 文件 | 阶段 | 用途 |
|------|------|------|
| `src/base/internal/io_thread.h/.cc` | 1 | I/O 线程池 |
| `src/base/internal/fiber.h/.cc` | 2 | 纤程实现 |
| `src/base/internal/fiber_scheduler.h/.cc` | 2 | 纤程调度器 |
| `src/base/internal/vm_context.h` | 2 | VMContext 结构体 |
| `src/base/internal/shard.h/.cc` | 3 | 分片管理器 |
| `src/base/internal/shard_message.h/.cc` | 3 | 跨分片消息 |
| `src/base/internal/cross_shard_rpc.h/.cc` | 3 | 跨分片 RPC |
| `src/base/internal/global_oid.h` | 4 | 128 位 OID |
| `src/base/internal/remote_proxy.h/.cc` | 4 | 远程对象代理 |
| `src/base/internal/object_serializer.h/.cc` | 4 | 对象序列化 |
| `src/base/internal/heartbeat_scheduler.h/.cc` | 4 | 分级心跳调度 |
| `src/base/internal/object_swapper.h/.cc` | 4 | 对象降级/加载 |
| `src/base/internal/persistence.h/.cc` | 4 | 持久化客户端 |
| `src/base/internal/cluster_rpc.h/.cc` | 4 | 跨节点 RPC |
| `cluster/registry_server.cc` | 4 | 全局注册中心 |
| `cluster/load_balancer.cc` | 4 | 负载均衡器 |

## 总结

本计划将 MudOS-NG 从单线程单机架构逐步演进为支持亿级对象的分布式集群:

```
单线程单机 → 多线程 I/O → 纤程协作并发 → 多核对象分片 → 多机联邦集群
  (现状)      (0-3月)     (4-12月)       (12-24月)      (18-36月)
```

每个阶段都是前一阶段的超集,且保留向后的 API 兼容性。核心设计原则:

1. **渐进可验证**:每阶段都可单独测试和部署
2. **LPC 透明**:mudlib 代码在不同阶段无需重写
3. **水平扩展**:从单机到集群无需架构重设计,只需增加节点
4. **对象局部性**:利用 MUD 世界的天然分区(房间、区域)最小化跨边界通信
