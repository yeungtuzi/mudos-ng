/*
 * heavy_controller.c — 高负载压力测试控制器
 *
 * 创建 N 个 heavy_worker 克隆，每心跳执行大量 CPU 操作。
 * 用法: driver config.test -fstress_heavy:N
 */

#define WORKER_FILE "/stress/heavy_worker"

int total_workers;
int start_time;
object *workers;
int batch_size;
int created;
int report_interval;

void create() {
    workers = ({});
    created = 0;
    batch_size = 500;     // 每批 500 个
    report_interval = 15; // 每 15 秒报告
}

void create_batch() {
    for (int i = 0; i < batch_size && created < total_workers; i++) {
        object ob = clone_object(WORKER_FILE);
        if (ob) {
            ob->create(created);
            workers += ({ ob });
            created++;
        }
    }

    if (created < total_workers) {
        // 用正延迟 call_out 让游戏 tick 穿插执行，心跳才能分发到工作线程
        call_out("create_batch", 1);
    } else {
        write(sprintf("创建完成: %d 个工作对象\n", created));
        call_out("run_test", 2);
    }
}

void run_test() {
    start_time = time();
    write(sprintf("高负载压力测试开始: %d workers, 目标 600 秒\n", total_workers));
    write("每个心跳: 100元素数组排序/filter/map + 50键映射 + 字符串处理\n");
    call_out("periodic_report", report_interval);
    call_out("finish_test", 600);
}

void periodic_report() {
    int elapsed = time() - start_time;
    int obj_count = sizeof(objects());
    int mem = memory_info();

    // 采样
    int sample = (sizeof(workers) > 100) ? 100 : sizeof(workers);
    int min_hb = 99999999, max_hb = 0;
    float total_hb = 0;

    for (int i = 0; i < sample; i++) {
        int c = workers[i]->query_counter();
        if (c < min_hb) min_hb = c;
        if (c > max_hb) max_hb = c;
        total_hb += to_float(c);
    }
    int avg_hb = to_int(total_hb / sample);

    write(sprintf("[%4ds] objs=%d workers=%d mem=%dKB "
                  "hb_avg=%d hb_min=%d hb_max=%d\n",
                  elapsed, obj_count, sizeof(workers),
                  mem / 1024, avg_hb, min_hb, max_hb));

    write_file("/stress_report.csv",
               sprintf("%d,%d,%d,%d,%d,%d,%d\n",
                       elapsed, obj_count, sizeof(workers),
                       mem / 1024, avg_hb, min_hb, max_hb));

    if (elapsed < 600) {
        call_out("periodic_report", report_interval);
    }
}

void finish_test() {
    int elapsed = time() - start_time;
    int obj_count = sizeof(objects());
    int mem_kb = memory_info() / 1024;

    write(sprintf("\n===== 压力测试完成 =====\n"));
    write(sprintf("持续时间: %d 秒\n", elapsed));
    write(sprintf("总对象数: %d\n", obj_count));
    write(sprintf("工作对象: %d\n", sizeof(workers)));
    write(sprintf("内存:     %d KB\n", mem_kb));

    // 心跳统计
    int total_hb = 0;
    int max_hb = 0;
    for (int i = 0; i < sizeof(workers); i++) {
        int c = workers[i]->query_counter();
        total_hb += c;
        if (c > max_hb) max_hb = c;
    }
    write(sprintf("总心跳数: %d\n", total_hb));
    if (elapsed > 0) {
        write(sprintf("吞吐量:   %d 心跳/秒\n", total_hb / elapsed));
        write(sprintf("每对象平均: %d 次/秒\n", (total_hb / sizeof(workers)) / elapsed));
    }

    write_file("/stress_report.csv",
               sprintf("FINISH,%d,%d,%d,%d,%d,%d\n",
                       elapsed, obj_count, sizeof(workers), mem_kb,
                       total_hb, max_hb));

    write("压力测试结束\n");
    shutdown(0);
}

void go(int target) {
    total_workers = target;
    write(sprintf("高负载压力测试: 目标 %d 个工作对象\n", total_workers));
    create_batch();
}
