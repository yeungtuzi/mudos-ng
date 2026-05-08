# Have Time #

## SSH Support ##
- [x] accept ssh connection at listening port
- [ ] make telnet port optional (default port is for SSH), with new config option "LegacyPort"

## network thread ##
- [x] Pure network I/O thread (Phase 1: IOThread/IOThreadPool — completed, 2 threads, round-robin connection distribution)

## multi-core CPU — 多线程改造 (详见 plan: .claude/plans/4-cpu-declarative-gem.md) ##

### 第〇阶段：对象扫描增量化 ###
- [x] 0a 容错遍历 — 删除 restart 逻辑，先存 next_all 再处理当前对象
- [x] 0b 增量扫描 — 每 tick 处理固定批量（__RC_SWAP_BATCH_SIZE__，默认 100），游标断点续扫
- [ ] 0c 扫描/执行分离 — 主线程只判标志位，LPC 执行 post 给心跳线程池 (依赖心跳池启用)

### 第一阶段：VM 状态线程本地化 ###
- [x] interpret.cc: ~35 个全局变量 → thread_local
- [x] interpret.h: extern → extern thread_local，end_of_stack/control_stack 运行时绑定
- [x] machine.h: current_object/command_giver/current_interactive → extern thread_local
- [x] simulate.cc: 全局变量 → thread_local
- [x] object.h/object.cc: previous_ob/cgsp → thread_local
- [x] heartbeat.cc/heartbeat.h: g_current_heartbeat_obj → thread_local
- [x] eval_limit.cc/eval_limit.h: outoftime → thread_local
- [x] 验证：构建 + 单元测试 + LPC 测试套件全部通过

### 第二阶段：共享数据结构线程安全化 ###
- [x] object_t: ref → std::atomic, flags → std::atomic
- [x] array_t/mapping_t/program_t/refed_t: ref → std::atomic
- [x] add_ref 宏改为 fetch_add；所有 ref 访问点适配 .load()
- [x] obj_list/obj_list_destruct 加 g_object_list_mutex
- [x] 共享字符串表加 g_string_table_mutex
- [x] 验证：构建 + 测试通过

### 第三阶段：每线程独立执行时限计时器 ###
- [x] posix_timers.h/cc: 新增 PerThreadTimer API
- [x] Linux: timer_create + SIGEV_THREAD_ID 每线程 API
- [x] macOS: dispatch_source_create 每线程 API
- [x] eval_limit.cc: 集成 init_thread_eval / cleanup_thread_eval / set_eval 每线程路由
- [x] 验证：构建 + 测试通过

### 第四阶段：HeartbeatThread 与 HeartbeatThreadPool ###
- [x] 新建 heartbeat_thread.h — HeartbeatThread / HeartbeatThreadPool 类
- [x] 新建 heartbeat_thread.cc — 完整实现（init_vm_state / process_heartbeats / event_loop / bounce）
- [x] HeartbeatThreadPool — 对象指针哈希分片
- [x] CMakeLists.txt 添加源文件
- [ ] 新建 test_heartbeat_thread.cc 单元测试 (后续)

### 第五阶段：心跳迁移 ###
- [x] heartbeat.cc: 重写为委托线程池（pool 不存在时退化为标志位操作）
- [x] mainlib.cc: 心跳线程池生命周期 (init → start → stop → delete)
- [x] rc.cc / runtime_config.h: 新增 heartbeat threads 配置（默认 0 = 禁用）
- [x] backend.cc: 最小改动（call_heart_beat 注册不变）
- [x] 验证：构建 + 测试通过

### 第六阶段：跨线程调用处理 ###
- [x] 6A: call_direct 跨线程检测 → bounce 回主线程 (g_current_heartbeat_thread + throw)
- [ ] 6B (未来): promise/future 阻塞式跨线程跳转 + 死锁防护

### 第七阶段：集成与性能测试 ###
- [x] 0-6 阶段全链路构建 + 单元测试通过
- [ ] TSan 构建验证（后续）
- [ ] 1/2/4/8 线程压力测试（1000+ 心跳对象）
- [ ] 性能基准：心跳吞吐量、CPU 利用率、主线程响应延迟
- [ ] 关闭安全性验证（心跳池→IO池→对象清理）

## Phase 0c: 扫描/执行分离（详见 plan） ##
- [ ] backend.cc: 主线程扫描只判标志位，LPC 执行 post 给心跳线程池
- [ ] 心跳池未启用时 fallback 内联执行

## call_out 线程池（详见 plan） ##
- [ ] call_out.cc: 正延迟 call_out 分发到心跳线程池
- [ ] 0 延迟 call_out 保留主线程原地执行
- [ ] g_callout_map_mutex 保护 callout map 并发访问

## 编译线程化（详见 plan） ##
- [ ] 新建编译线程类
- [ ] load_object() 改为异步编译

## packages ##
- [ ] For linux, deb or rpm or flatpak
- [ ] For Mac, homebrew
- [ ] Windows, TBD
