# Major Updates #

## SSH Support ##
- [x] accept ssh connection at listening port

## network thread ##
- [x] Pure network I/O thread (Phase 1: IOThread/IOThreadPool — completed, 2 threads, round-robin connection distribution)

## multi-core CPU — 多线程改造 (详见 plan: docs/multi-threading-plan.md) ##

### 第〇阶段：对象扫描增量化 ###
- [x] 0a 容错遍历 — 删除 restart 逻辑，先存 next_all 再处理当前对象
- [x] 0b 增量扫描 — 每 tick 处理固定批量（__RC_SWAP_BATCH_SIZE__，默认 100），游标断点续扫
- [x] 0c 扫描/执行分离 — 主线程只判标志位，LPC 执行 post 给心跳线程池

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

### 第五阶段：心跳迁移 ###
- [x] heartbeat.cc: 重写为委托线程池（pool 不存在时退化为标志位操作）
- [x] mainlib.cc: 心跳线程池生命周期，默认 auto-detect CPU 核心数
- [x] rc.cc / runtime_config.h: 新增 heartbeat threads / swap batch size 配置
- [x] 验证：构建 + 测试通过

### 第六阶段：跨线程调用处理 ###
- [x] 6A: call_direct 跨线程检测 → bounce 回主线程 (g_current_heartbeat_thread + throw)

### 第八阶段：call_out 线程池 ###
- [x] call_out.cc: 正延迟 call_out 分发到心跳线程池
- [x] 0 延迟 call_out 保留主线程原地执行
- [x] g_callout_map_mutex 保护 callout map 并发访问

### 已移除：玩家命令并行处理 ###
**永久移除。** 心跳/call_out/对象扫描全部移出后，主线程 CPU 从 ~100% 降至 ~25-35%，已是玩家命令专用线程。命令是延迟敏感交互型负载，独占主线程优于跨线程并行。

---

## 待完成工作 ##

### 短期 ###
- [x] 心跳队列为空问题修复（ready flag + socket write 竞态 + thread_for_object 哈希修正）
- [x] 30000 对象创建 + 心跳队列分发验证（对象均匀分布到 8 线程，每线程 3000-4400 对象）
- [x] test_heartbeat_thread.cc 单元测试 (14 tests) + test_io_thread.cc (14 tests)
- [x] 线程安全修复：funptr_hdr_t::ref 原子化、program_t::func_ref 原子化、apply_ret_value → thread_local
- [x] debugmalloc 多线程安全：md_refjournal mutex、MDmalloc/MDfree mutex、DEBUGMALLOC_EXTENSIONS 临时关闭
- [ ] 30000 对象心跳执行稳定性 — 进度: push_shared_string/timer/svalue_to_int/int_free_string 已修复，当前 crash 在 extend_string→debugrealloc→MDfree, 待定位最后一个 bug
- [ ] TSan 构建验证
- [ ] 性能基准：心跳吞吐量、CPU 利用率、主线程响应延迟
- [ ] 关闭安全性验证（心跳池→IO池→对象清理）
- [ ] 压力测试脚本化（1K/5K/10K/30K 对象梯度）

### 中期 ###
- [ ] 6B (未来): promise/future 阻塞式跨线程跳转 + 死锁防护

### 长期 / 非驱动层 ###
- [ ] 驱动层外围完善
