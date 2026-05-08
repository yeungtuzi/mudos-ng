/*
 * heartbeat_worker.c — 压力测试工作对象（无 mudlib 依赖）
 *
 * 每个对象有心跳函数，执行真实负载：
 * - 数组操作 (filter, map, sort)
 * - 映射查询/修改
 * - 字符串处理
 */

// 对象的工作数据集
mapping data;

void create() {
    data = ([
        "counter" : 0,
        "values"  : allocate(20),
        "cache"   : ([]),
    ]);
    for (int i = 0; i < 20; i++) {
        data["values"][i] = random(10000);
    }
    set_heart_beat(1 + random(3));
}

// 数组处理
mixed array_work() {
    int *vals = data["values"];
    int *evens = filter(vals, (: $1 % 2 == 0 :));
    int *doubled = map(evens, (: $1 * 2 :));
    sort_array(doubled, 1);
    data["cache"]["array_count"] = sizeof(vals);
    data["cache"]["even_count"] = sizeof(evens);
    return doubled[0..(sizeof(doubled) > 5 ? 4 : sizeof(doubled) - 1)];
}

// 映射操作
void mapping_work() {
    mapping m = ([]);
    for (int i = 0; i < 10; i++) {
        m["key_" + i] = random(1000);
    }
    foreach (mixed key, mixed val in m) {
        m[key] = val + 1;
    }
    data["cache"]["mapping_size"] = sizeof(m);
}

// 字符串处理
string string_work() {
    string base = "stress_test_data_" + random(10000);
    string result = "";
    for (int i = 0; i < 5; i++) {
        result += base[i..(i + 3)];
    }
    return implode(explode(result, "_"), "-");
}

void heart_beat() {
    data["counter"]++;

    array_work();
    mapping_work();
    string_work();

    if (data["counter"] % 100 == 0) {
        data["cache"] = ([]);
    }
}

int query_counter() { return data["counter"]; }
