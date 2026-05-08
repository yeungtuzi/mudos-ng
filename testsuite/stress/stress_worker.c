/*
 * stress_worker.c — 压力测试工作对象
 * 每个心跳执行数组+映射+字符串操作，模拟真实负载
 */
mapping data;

void create() {
    data = (["counter":0, "values":allocate(50), "cache":([])]);
    for (int i = 0; i < 50; i++) data["values"][i] = random(10000);
    set_heart_beat(1);  // 每 tick 执行
}

void heart_beat() {
    data["counter"]++;

    // 数组操作
    int *vals = data["values"];
    int *filtered = filter(vals, (: $1 > 1000 :));
    int *doubled = map(filtered, (: $1 * 3 :));
    int *sorted = sort_array(doubled, -1);

    // 映射操作
    mapping m = ([]);
    for (int i = 0; i < 20; i++) m["k" + i] = i * i;
    foreach (mixed k, mixed v in m) m[k] = v + 1;
    data["cache"]["map_size"] = sizeof(keys(m));

    // 字符串操作
    string s = "";
    for (int i = 0; i < 5; i++) s += sprintf("x%d", random(1000));
    data["cache"]["str"] = strlen(s);
    data["cache"]["arr"] = sizeof(sorted);
}

int query_counter() { return data["counter"]; }
