# Have Time #

## SSH Support ##
- [x] accept ssh connection at listening port
- [ ] make telnet port optional (default port is for SSH), with new config option "LegacyPort"

## network thread ##
- [x] Pure network I/O thread (Phase 1: IOThread/IOThreadPool — completed, 2 threads, round-robin connection distribution)

## multi-core CPU — 多线程改造 (详见 plan: .claude/plans/4-cpu-declarative-gem.md) ##

### 第〇阶段：对象扫描增量化（零依赖） ###
- [ ] 0a 容错遍历 — 删除 restart 逻辑，先存 next_all 再处理当前对象
- [ ] 0b 增量扫描 — 每 tick 处理固定批量（__RC_SWAP_BATCH_SIZE__，默认 50），游标断点续扫
- [ ] 0c 扫描/执行分离 — 主线程只判标志位，LPC 执行 post 给心跳线程池 (依赖 Phase 2)

### 第一阶段：VM 状态线程本地化 ###
- [ ] interpret.cc: ~35 个全局变量 → thread_local
- [ ] interpret.h: extern → extern thread_local，end_of_stack/control_stack 改为访问器函数
- [ ] machine.h: current_object/command_giver/current_interactive → extern thread_local
- [ ] simulate.cc: 全局变量 → thread_local
- [ ] object.h/object.cc: previous_ob/cgsp → thread_local
- [ ] heartbeat.cc/heartbeat.h: g_current_heartbeat_obj → thread_local
- [ ] eval_limit.cc/eval_limit.h: outoftime → thread_local
- [ ] 验证：构建 + 单元测试 + LPC 测试套件全部通过

### 第二阶段：共享数据结构线程安全化 ###
- [ ] object_t: ref → atomic, flags → atomic
- [ ] array_t/mapping_t/program_t/refed_t: ref → atomic
- [ ] add_ref 宏改为 fetch_add
- [ ] 所有 ref-- 改为 fetch_sub
- [ ] obj_list/obj_list_destruct 加 g_object_list_mutex
- [ ] 共享字符串表加 mutex
- [ ] 验证：TSan 构建无数据竞争

### 第三阶段：每线程独立执行时限计时器 ###
- [ ] Linux: timer_create + SIGEV_THREAD_ID 每线程 API
- [ ] macOS: dispatch_source_create 每线程 API
- [ ] eval_limit.cc: 每线程计时器集成
- [ ] 验证：主线程和心跳线程 eval limit 各自工作

### 第四阶段：HeartbeatThread 与 HeartbeatThreadPool ###
- [ ] 新建 heartbeat_thread.h — HeartbeatThread 类
- [ ] 新建 heartbeat_thread.cc — 实现（init_vm_state / process_heartbeats / event_loop）
- [ ] HeartbeatThreadPool — 对象指针哈希分片
- [ ] CMakeLists.txt 添加源文件
- [ ] 新建 test_heartbeat_thread.cc 单元测试

### 第五阶段：心跳迁移 ###
- [ ] heartbeat.cc: 删除全局 deques，call_heart_beat/set_heart_beat/query_heart_beat 委托线程池
- [ ] mainlib.cc: 心跳线程池生命周期 (init → start → stop → delete)
- [ ] rc.cc / runtime_config.h: 新增 __RC_HEARTBEAT_THREADS__ 配置
- [ ] backend.cc: 最小改动（call_heart_beat 注册不变）
- [ ] 验证：1/2/4 线程 LPC 测试套件通过

### 第六阶段：跨线程调用处理 ###
- [ ] 6A: call_direct 跨线程检测 → bounce 回主线程 (heartbeat_bounce_exception)
- [ ] object_t 新增 heart_beat_thread_id 字段
- [ ] 6B (未来): promise/future 阻塞式跨线程跳转 + 死锁防护

### 第七阶段：集成与性能测试 ###
- [ ] TSan 构建验证完整测试套件
- [ ] 1/2/4/8 线程压力测试（1000+ 心跳对象）
- [ ] 性能基准：心跳吞吐量、CPU 利用率、主线程响应延迟
- [ ] 关闭安全性验证（心跳池→IO池→对象清理）

## packages ##
- [ ] For linux, deb or rpm or flatpak
- [ ] For Mac, homebrew
- [ ] Windows, TBD
