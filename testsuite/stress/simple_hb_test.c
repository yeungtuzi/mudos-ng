/*
 * simple_hb_test.c — 简单心跳测试，验证心跳线程池工作
 */
int hb_count = 0;

void heart_beat() {
    hb_count++;
    write(sprintf("[%d] heartbeat #%d on object\n", time(), hb_count));
}

void do_tests() {
    write("Setting heartbeat...\n");
    set_heart_beat(1);
    write(sprintf("query_heart_beat: %d\n", query_heart_beat(this_object())));
}
