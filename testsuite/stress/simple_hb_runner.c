#include <globals.h>
#define WORKER "/stress/simple_hb_test"
object *workers;
void do_tests() {
    int n = 1000;
    workers = ({});
    write(sprintf("START create %d simple workers\n", n));
    for (int i = 0; i < n; i++) {
        object ob = clone_object(WORKER);
        if (ob) workers += ({ob});
    }
    write(sprintf("DONE create %d workers, mem=%dKB\n", sizeof(workers), memory_info()/1024));
    set_heart_beat(1);
}
int ticks = 0;
void heart_beat() {
    ticks++;
    if (ticks <= 5 || ticks % 20 == 0) {
        int total = 0;
        for (int i = 0; i < sizeof(workers); i++) total += workers[i]->query_counter();
        write(sprintf("[TICK %d] total_hb=%d\n", ticks, total));
    }
    if (ticks >= 100) shutdown(0);
}
