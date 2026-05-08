/*
 * run_stress.c — 渐进式日志压力测试
 * 每 1000 个对象输出进度和内存, 监测重编译和泄漏
 */
#define WORKER "/stress/stress_worker"

object *workers;
int start_time;

void log(string msg) {
    write(sprintf("[%d] %s\n", time(), msg));
}

void do_tests() {
    int n = 30000;
    workers = ({});
    log(sprintf("START create %d objects, mem=%dKB", n, memory_info()/1024));

    int t0 = time();
    int last_mem = memory_info()/1024;
    int last_t = t0;

    for (int i = 0; i < n; i++) {
        object ob = clone_object(WORKER);
        if (ob) workers += ({ob});

        if ((i+1) % 1000 == 0) {
            int now = time();
            int mem = memory_info()/1024;
            int batch_time = now - last_t;
            float per_obj = (float)batch_time / 1000.0;

            log(sprintf("  progress %d/%d: +%ds (%.2fs/obj) mem=%dKB (+%dKB) total_objs=%d",
                        i+1, n, batch_time, per_obj, mem, mem-last_mem,
                        sizeof(objects())));
            last_mem = mem;
            last_t = now;
        }
    }

    int t1 = time();
    log(sprintf("DONE create: %d objects in %ds, mem=%dKB, total_objs=%d",
                sizeof(workers), t1-t0, memory_info()/1024, sizeof(objects())));

    call_out("begin_monitor", 3);
}

void begin_monitor() {
    start_time = time();
    log(sprintf("HB-MONITOR start 30s, mem=%dKB total_objs=%d",
                memory_info()/1024, sizeof(objects())));
    log("  Using heart_beat for reports (call_out unreliable under load)");
    set_heart_beat(1);  // fire every tick for reports
    call_out("finish_report", 30);
}

int hb_ticks = 0;
void heart_beat() {
    hb_ticks++;
    // Log every tick for first 5, then every 100
    if (hb_ticks <= 5 || hb_ticks % 100 == 0) {
        int sum = 0, minv = 999999, maxv = 0;
        for (int i = 0; i < 50; i++) {
            int c = workers[i]->query_counter();
            if (c < minv) minv = c; if (c > maxv) maxv = c; sum += c;
        }
        log(sprintf("HB +%ds (tick %d): avg=%d min=%d max=%d mem=%dKB",
                    time()-start_time, hb_ticks, sum/50, minv, maxv, memory_info()/1024));
    }
}

void finish_report() {
    set_heart_beat(0);  // stop reporting
    int elap = time() - start_time;
    int total = sizeof(workers);
    int min_hb = 999999, max_hb = 0, sum_hb = 0;
    for (int i = 0; i < 100; i++) {
        int c = workers[i]->query_counter();
        if (c < min_hb) min_hb = c; if (c > max_hb) max_hb = c; sum_hb += c;
    }
    log(sprintf("REPORT ==== %ds result ====", elap));
    log(sprintf("  objects=%d mem=%dKB total_objs=%d", total, memory_info()/1024, sizeof(objects())));
    log(sprintf("  hb avg=%d min=%d max=%d", sum_hb/100, min_hb, max_hb));
    if (max_hb > 0)
        log(sprintf("  throughput ~%d/sec", max_hb*total/elap));
    else
        log("  WARNING: no heartbeats!");
    log("  Done. Check Activity Monitor for P/E core load");
    shutdown(0);
}
