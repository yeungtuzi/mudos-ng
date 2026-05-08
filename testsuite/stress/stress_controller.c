/*
 * stress_controller.c — 压力测试控制器
 *
 * 用法: driver config.test -fstress_controller -f'stress_controller->go(N)'
 */

#define WORKER_FILE "/stress/heartbeat_worker"

int total_workers;
int start_time;
int last_report_time;
object *workers;
int batch_size;
int created;

void create() {
    workers = ({});
    created = 0;
    batch_size = 50;
}

void create_batch() {
    for (int i = 0; i < batch_size && created < total_workers; i++) {
        object ob = clone_object(WORKER_FILE);
        if (ob) {
            workers += ({ ob });
            created++;
        }
    }

    if (created < total_workers) {
        call_out("create_batch", 1);
    } else {
        write(sprintf("创建完成: %d 个工作对象\n", created));
        call_out("run_test", 2);
    }
}

void run_test() {
    start_time = time();
    last_report_time = start_time;
    write("压力测试开始 (目标 600 秒)\n");
    call_out("periodic_report", 10);
    call_out("finish_test", 600);
}

void periodic_report() {
    int elapsed = time() - start_time;
    int obj_count = sizeof(objects());
    int mem = memory_info();

    int min_hb = 999999, max_hb = 0, total_hb = 0;
    int sample_size = (sizeof(workers) > 50) ? 50 : sizeof(workers);
    for (int i = 0; i < sample_size; i++) {
        int c = workers[i]->query_counter();
        if (c < min_hb) min_hb = c;
        if (c > max_hb) max_hb = c;
        total_hb += c;
    }
    int avg_hb = (sample_size > 0) ? total_hb / sample_size : 0;

    write(sprintf("[%4ds] objs=%d workers=%d mem=%dKB "
                  "hb(avg=%d min=%d max=%d)\n",
                  elapsed, obj_count, sizeof(workers),
                  mem / 1024, avg_hb, min_hb, max_hb));

    // Write CSV data for report generation.
    write_file("/stress_report.csv",
               sprintf("%d,%d,%d,%d,%d,%d,%d\n",
                       elapsed, obj_count, sizeof(workers),
                       mem / 1024, avg_hb, min_hb, max_hb));

    if (elapsed < 600) {
        call_out("periodic_report", 10);
    }
}

void finish_test() {
    int elapsed = time() - start_time;
    int obj_count = sizeof(objects());

    write(sprintf("\n===== 压力测试完成 =====\n"));
    write(sprintf("持续时间: %d 秒\n", elapsed));
    write(sprintf("总对象数: %d\n", obj_count));
    write(sprintf("心跳工作对象: %d\n", sizeof(workers)));
    write(sprintf("内存: %d KB\n", memory_info() / 1024));

    // 采样最终心跳计数
    int total_hb = 0;
    for (int i = 0; i < sizeof(workers); i++) {
        total_hb += workers[i]->query_counter();
    }
    write(sprintf("总心跳执行: %d 次\n", total_hb));
    if (elapsed > 0) {
        write(sprintf("吞吐量: %d 心跳/秒\n", total_hb / elapsed));
    }

    write("压力测试结束\n");
    shutdown(0);
}

// 入口: go(N) 启动 N 个工作对象
void go(int target) {
    total_workers = target;
    write(sprintf("压力测试: 目标 %d 个工作对象\n", total_workers));
    create_batch();
}
