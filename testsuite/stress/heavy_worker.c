/*
 * heavy_worker.c — 高负载压力测试工作对象
 *
 * 每个心跳执行大量 CPU 密集型操作，旨在饱和所有 CPU 核心。
 * 负载：大数组排序 + 深层映射操作 + 字符串处理 + 浮点计算
 */

mapping data;
int id;

void create(int my_id) {
    id = my_id;
    data = ([
        "counter"  : 0,
        "values"   : allocate(100),   // 100 元素数组
        "cache"    : ([]),
        "history"  : allocate(50),    // 历史记录
    ]);

    // 初始化随机数据
    for (int i = 0; i < 100; i++) {
        data["values"][i] = random(100000);
    }
    for (int i = 0; i < 50; i++) {
        data["history"][i] = random(50000);
    }

    // 心跳间隔 = 1 tick，最大化压力
    set_heart_beat(1);
}

// 重量级数组处理
void heavy_array_work() {
    int *vals = data["values"] + ({});

    // 排序
    int *sorted = sort_array(vals, 1);

    // filter + map + 再次排序
    int *filtered = filter(sorted, (: $1 > 5000 && $1 < 95000 :));
    int *mapped = map(filtered, (: $1 * 3 + random(10) :));
    int *mapped2 = map(mapped, (: $1 * $1 / ($1 + 1) :));
    sort_array(mapped2, -1);

    // 统计
    int sum = 0;
    for (int i = 0; i < sizeof(mapped2); i++) {
        sum += mapped2[i];
    }

    data["cache"]["array_sum"] = sum;
    data["cache"]["array_size"] = sizeof(mapped2);
}

// 重量级映射操作
void heavy_mapping_work() {
    mapping m = ([]);
    // 构造 50 键映射
    for (int i = 0; i < 50; i++) {
        m["key_" + sprintf("%04d", i)] = random(10000);
    }

    // 遍历修改
    int sum = 0;
    foreach (mixed key, mixed val in m) {
        m[key] = val * 2 + 1;
        sum += m[key];
    }

    // keys/values 操作
    mixed *keys_arr = keys(m);
    sort_array(keys_arr, 1);

    // 子映射
    mapping sub = ([]);
    for (int i = 0; i < 20 && i < sizeof(keys_arr); i++) {
        sub[keys_arr[i]] = m[keys_arr[i]];
    }

    data["cache"]["map_sum"] = sum;
    data["cache"]["map_keys"] = sizeof(keys_arr);
    data["cache"]["sub_map"] = sizeof(sub);
}

// 重量级字符串处理
void heavy_string_work() {
    string base = "";
    // 构造长字符串
    for (int i = 0; i < 20; i++) {
        base += sprintf("data_%05d_%05d_", id, random(100000));
    }

    // explode/implode
    string *parts = explode(base, "_");
    string rebuilt = implode(parts[0..(sizeof(parts) > 10 ? 10 : sizeof(parts) - 1)], "-");

    // 字符操作
    string reversed = "";
    int len = strlen(rebuilt);
    for (int i = len - 1; i >= 0 && i > len - 50; i--) {
        reversed += rebuilt[i..i];
    }

    data["cache"]["str_len"] = len;
    data["cache"]["rev_len"] = strlen(reversed);
}

void heart_beat() {
    data["counter"]++;

    heavy_array_work();
    heavy_mapping_work();
    heavy_string_work();

    // 周期性清理
    if (data["counter"] % 50 == 0) {
        data["cache"] = ([]);
        data["values"] = map(data["values"], (: random(100000) :));
    }
}

int query_counter() { return data["counter"]; }
int query_id() { return id; }
