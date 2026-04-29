---
layout: doc
title: MudOS Migration
---
<!--
Copyright (c) 2026 [大河马/dahema@me.com]
SPDX-License-Identifier: MIT
-->

# 旧 MudOS Mudlib 兼容性修复

## 背景

使用 MudOS-NG 加载旧版 MudOS v22 (ES2) mudlib 时,因 MudOS-NG 开启了 `SENSIBLE_MODIFIERS` 编译选项,移除了 MudOS 中的 `static` 关键字,导致大量 LPC 源文件编译失败。

## 快速诊断

运行 driver 后出现致命错误:

```
/adm/obj/master.c line 41: syntax error, unexpected L_BASIC_TYPE, expecting L_ASSIGN or ';' or '(' or ','
```

输出中的预定义宏确认了 `SENSIBLE_MODIFIERS` 已启用:

```c
#define __DEFAULT_PRAGMAS__ PRAGMA_WARNINGS + PRAGMA_SAVE_TYPES + PRAGMA_ERROR_CONTEXT + PRAGMA_OPTIMIZE
```

## 根因分析

### `static` 关键字

在 `src/compiler/internal/lex.cc` 中,`static` 只在 `SENSIBLE_MODIFIERS` 未定义时注册:

```c
// line 205-207
#ifndef SENSIBLE_MODIFIERS
    {"static", L_TYPE_MODIFIER, DECL_NOSAVE | DECL_PROTECTED},
#endif
```

启用 `SENSIBLE_MODIFIERS` 时,`static` 不是关键字,lexer 将其当作普通标识符(`L_IDENTIFIER`),导致解析器在函数/变量声明处报错。

确认该选项的定义:

```bash
$ grep 'SENSIBLE_MODIFIERS' src/local_options
#define SENSIBLE_MODIFIERS
```

### 替代方案

MudOS 中 `static` 有两种语义:

| 上下文 | MudOS static 含义 | SENSIBLE_MODIFIERS 替代 |
|--------|-------------------|------------------------|
| 变量 | 不保存到文件(nosave) | `nosave` |
| 函数 | 不对外部可见(protected) | `protected` |

替代关键字的定义:

| 关键字 | Token | 值 | 说明 |
|--------|-------|----|------|
| `nosave` | L_TYPE_MODIFIER | DECL_NOSAVE | 变量不保存 |
| `protected` | L_TYPE_MODIFIER | DECL_PROTECTED | 仅当前对象及继承者可见 |
| `public` | L_TYPE_MODIFIER | DECL_PUBLIC | 外部可见(默认) |
| `nomask` | L_TYPE_MODIFIER | DECL_NOMASK | 不允许重定义 |

## 修改文件清单

共修改 38 个文件,包含约 60+ 处 `static` 声明。

### 核心文件

| 文件 | 修改内容 |
|------|---------|
| `adm/obj/master.c` | `crash()` 和 `update_file()` 函数声明: `static` → `nosave protected` |

### Feature 文件

| 文件 | 修改 |
|------|------|
| `feature/dbase.c` | 变量声明 `static` → `nosave` |
| `feature/treemap.c` | 函数声明 `static nomask` → `nosave protected nomask` |
| `feature/move.c` | 变量声明 |
| `feature/command.c` | 变量声明 |
| `feature/more.c` | 变量声明 |
| `feature/team.c` | 变量声明 |
| `feature/attack.c` | 变量声明 |
| `feature/alias.c` | 变量声明 |
| `feature/name.c` | 变量声明 |
| `feature/trigger.c` | 变量声明 |
| `feature/action.c` | 变量声明 |
| `feature/message.c` | 变量声明 |

### Daemon 文件

| 文件 | 修改 |
|------|------|
| `adm/daemons/chinesed.c` | 变量声明 `static` → `nosave` |
| `adm/daemons/natured.c` | 变量声明 |
| `adm/daemons/natured_feng.c` | 变量声明 |
| `adm/daemons/na.c` | 变量声明 |
| `adm/daemons/network/dns_master.c` | `nosave` 变量 |
| `adm/daemons/network/ms.c` | 变量声明 |
| `adm/daemons/network/mail_serv.c` | 变量声明 |
| `adm/daemons/network/pingtcp.c` | 变量声明 |
| `adm/daemons/network/ftpd.c` | 变量声明 |
| `adm/daemons/network/cmwhod.c` | 变量声明 |
| `adm/daemons/network/pingd.c` | 变量声明 |

### Object 文件

| 文件 | 修改 |
|------|------|
| `obj/user.c` | 变量声明 |
| `obj/toy/dead_tiger.c` | 变量声明 |
| `obj/user/user.c` | 变量声明 |
| `obj/user/wiz.c` | 变量声明 |

### Network Daemon 修复

| 文件 | 修改 |
|------|------|
| `adm/daemons/network/dns_master.c` | 为 `PREF_TELL` 添加 UDP 分支 (`#elif`),消除空 `if` 体导致的 "Expression has no side effects" 编译警告 |

## 日志文件配置

`config.cfg` 中 `log directory` 路径不能以 `/` 开头。MudOS-NG 在启动时会在调用 `chdir()` 到 mudlib 目录之前就尝试打开日志文件,因此日志路径相对于启动 driver 时的当前工作目录(CWD)。

推荐做法:
- 从 mudlib 父目录启动: `cd /path/to/mudlib-parent && driver config.cfg`
- 或使用绝对路径(但注意 MudOS-NG 会去除路径开头的 `/` — 这是一个已知的行为)

## dns_master.c 表达式警告修复

### 问题

```
/adm/daemons/network/dns_master.c line 548: Warning: Expression has no side effects, and the value is unused
```

### 根因

`include/net/config.h` 中定义了:
```c
#define PREF_TELL  SVC_UDP   // SVC_UDP = 8
#define PREF_FINGER SVC_TCP  // SVC_TCP = 2
#define PREF_MAIL  SVC_TCP
```

在 `query_services()` 函数中,PREF_TELL 的协议检查:
```c
#if PREF_TELL & SVC_TCP   // 8 & 2 = 0 (false!)
```

因为 `SVC_UDP & SVC_TCP = 0`,预处理器移除了整个内层 `#if` 块,留下一个空的 `if` 体:
```c
if (!(mud_svc[mud]["tell"] & SVC_KNOWN)) {
    // 空的!
}
```

同时,PREF_FINGER 和 PREF_MAIL 都有完整的 UDP 回退分支,唯独 PREF_TELL 缺失。

### 修复

添加 UDP 协议分支,匹配 PREF_MAIL/PREF_FINGER 的已有模式:
```c
#if PREF_TELL & SVC_TCP
      if (tcp == TCP_SOME && !(mud_svc[mud]["tell"] & (SVC_TCP | SVC_NO_TCP)))
        SUPPORT_Q->send_support_q(address, port, "tcp", "tell");
#elif PREF_TELL & SVC_UDP
      if (!(mud_svc[mud]["tell"] & (SVC_UDP | SVC_NO_UDP)))
        SUPPORT_Q->send_support_q(address, port, "tell");
#endif
```

### 标准库文件

| 文件 | 修改 |
|------|------|
| `std/char.c` | 变量声明 |
| `std/room.c` | 变量声明 |
| `std/item/combined.c` | 变量声明 |

### 其他文件

| 文件 | 修改 |
|------|------|
| `class/guard/gtime.c` | 变量声明 |
| `class/guard/firstt.c` | 变量声明 |
| `class/guard/xtime.c` | 变量声明 |
| `class/guard/mytime.c` | 变量声明 |
| `class/guard/secondt.c` | 变量声明 |
| `class/guard/npc/firstt.c` | 变量声明 |
| `class/guard/npc/mytime.c` | 变量声明 |
| `class/scholar/sword_soul.c` | 变量声明 |
| `d/chengdu/room.c` | 变量声明 |
| `d/newb/huntarea.c` | 变量声明 |
| `d/newb/npc/teacher.c` | 变量声明 |
| `d/hua/obj/dead_tiger.c` | 变量声明 |

## 批量修复脚本

用于扫描整个 mudlib 并自动修复:

```python
#!/usr/bin/env python3
"""Convert MudOS 'static' to MudOS-NG SENSIBLE_MODIFIERS equivalents.

  Variable: static int x;       → nosave int x;
  Function: static void f() { } → protected void f() { }
           static nomask ...   → protected nomask ...
"""
import re
import os
import sys

MUDLIB = "/path/to/mudlib"

VAR_RE = re.compile(r'^static\s+')

def fix_line(line):
    stripped = line.lstrip()
    if not stripped.startswith("static"):
        return line

    indent = line[:len(line) - len(stripped)]
    content = stripped[6:].lstrip()

    # Heuristic: if '(' appears before ';', it's a function
    paren_pos = content.find('(')
    semi_pos = content.find(';')

    if paren_pos >= 0:
        # Check if the token before '(' looks like a function name
        before_paren = content[:paren_pos].strip().split()[-1] if content[:paren_pos].strip() else ""
        if before_paren and re.match(r'^[a-zA-Z_][a-zA-Z0-9_]*$', before_paren):
            # It's a function declaration
            if 'nomask' in content or 'nosave' in content:
                return indent + 'protected' + content
            return indent + 'protected ' + content

    # Variable declaration
    return indent + 'nosave' + content


def fix_file(filepath):
    with open(filepath, 'r', encoding='utf-8', errors='replace') as f:
        content = f.read()

    lines = content.split('\n')
    changed = False
    new_lines = []
    for line in lines:
        stripped = line.strip()
        if not stripped.startswith('static'):
            new_lines.append(line)
            continue
        fixed = fix_line(line)
        if fixed != line:
            changed = True
        new_lines.append(fixed)

    if changed:
        with open(filepath, 'w', encoding='utf-8') as f:
            f.write('\n'.join(new_lines))
        return True
    return False

if __name__ == '__main__':
    count = 0
    files_changed = 0
    for root, dirs, files in os.walk(MUDLIB):
        dirs[:] = [d for d in dirs if not d.startswith('.') and d != 'bak']
        for fname in files:
            if fname.endswith('.c'):
                fpath = os.path.join(root, fname)
                try:
                    if fix_file(fpath):
                        files_changed += 1
                        print(f"Fixed: {fpath[len(MUDLIB):]}")
                except Exception as e:
                    print(f"Error processing {fpath}: {e}", file=sys.stderr)

    print(f"\nDone. {files_changed} files changed.")
```

## 验证结果

修复后 driver 正常启动并接受连接:

```
New Debug log location: "log/debug.log".
...
Accepting telnet connections on 0.0.0.0:6688.
IO thread pool started with 2 threads.
Initializations complete.
```

无 `restore_object`、`static` 关键字或 `#pragma save_binary` 相关错误。

## 剩余非致命警告

| 警告 | 原因 | 状态 |
|------|------|------|
| `Illegal to declare nosave function` | 函数使用了 `nosave protected` | 已修复 |
| `Unknown #pragma, ignored.` (`#pragma save_binary`) | 12 个文件使用了 MudOS 弃用 pragma | 已删除 |
| `redefinition of #define` (ansi.h) | ansi.h 被重复包含 | 已加 `#ifndef` 守卫 |
| `Unable to open log file` | 日志路径相对于启动 CWD | 已修复(从父目录启动) |
| `Warning: Expression has no side effects` (dns_master.c:548) | `#if` 预处理器条件评估为假,空 `if` 体 | 已修复(添加 `#elif` UDP 分支) |
| `Warning: A negative constant as the second element of arr[x..y]` | MudOS-NG 数组切片语义改变 | `arr[x..-1]` → `arr[x..<1]` |
| `Unused local variable` | 变量声明但未使用 | 不影响运行,可忽略 |

## 数据迁移:GBK→UTF-8 Save 文件转换

### 背景

旧版 MudOS 环境下 `.o` save 文件中的中文字符串使用 GBK/GB2312 编码。MudOS-NG 默认要求所有字符串为有效 UTF-8,加载旧版 `.o` 文件时会导致:

```
*restore_object(): Invalid utf8 string while restoring dict.
*restore_object(): Invalid utf8 string while restoring emote.
```

### 问题分析

`.o` 文件是 `save_object()` / `restore_object()` 使用的 LPC 序列化格式,结构如下:

```
#/path/to/source.c
variable_name ([
  "key":"包含 GBK 中文的字符串",
  ...
])
```

文件为文本格式,混合了 ASCII 结构字符和 GBK 编码的中文文本。由于整个文件可以通过 GBK 编解码器解析(ASCII 是 GBK 的子集),可以直接对整个文件进行 GBK→UTF-8 转码。

### 批量转换脚本

```python
import os

def convert_o_files(data_dir):
    for root, dirs, files in os.walk(data_dir):
        for fname in files:
            if not fname.endswith('.o'):
                continue
            fpath = os.path.join(root, fname)
            with open(fpath, 'rb') as f:
                data = f.read()
            
            try:
                data.decode('utf-8')  # skip if already UTF-8
                continue
            except UnicodeDecodeError:
                pass
            
            # Backup original
            bak = fpath + '.gbk.bak'
            if not os.path.exists(bak):
                os.rename(fpath, bak)
            
            # GBK→UTF-8 conversion
            text = data.decode('gbk', errors='replace')
            with open(fpath, 'wb') as f:
                f.write(text.encode('utf-8'))

convert_o_files('/path/to/mudlib/data')
```

### 转换结果

- 扫描 `data/` 目录下所有 `.o` 文件
- 共转换 **6348 个文件**,其中 75 个已是有效 UTF-8(无需转换)
- 0 个文件转换失败
- 原始文件备份为 `.o.gbk.bak`

### 注意事项

- `errors='replace'` 处理极少数无法解析的 GBK 字节(约 14 字节/171KB),替换为 U+FFFD (�)
- 转换后文件体积略微增大(GBK 使用 2 字节/汉字,UTF-8 使用 3 字节/汉字)
- 如果未来需要回退,可以从 `.o.gbk.bak` 恢复

## LPC 中文名验证 UTF-8 修复

### 背景

MudOS-NG 的 LPC 字符串处理与 MudOS 有根本性差异:

| 操作 | MudOS 行为 | MudOS-NG 行为 |
|------|-----------|-------------|
| `str[i]` | 返回第 i 个**字节**的值(0-255) | 返回第 i 个**字符**的 Unicode 码点(如"大"=U+5927=22823) |
| `strlen()` | 返回**字节数** | 返回**字节数**(不变) |

原 `check_legal_name()` 函数按 GBK 2 字节/汉字的假设编写,使用 `i%2==0` 按字节位置判断中文字符,并且用 `strlen()` 返回的字节数做范围检查(4-8 字节=2-4 个 GBK 字符)。在 MudOS-NG 上这些全部失效:

- `strlen("大河马")` = 9 字节(UTF-8,每汉字 3 字节),超出范围检查 4-8 → 误判为"名字太长"
- `str[i]` 返回码点而非字节值,码点值(U+4E00=19968)远大于原代码中的比较值(160-255)
- `i%2==0` 的按步进检查不再适用(UTF-8 每汉字 3 字节)

### 根因

MudOS-NG 使用 ICU 实现 Unicode 感知的字符串操作。核心实现在:

`src/vm/internal/base/interpret.cc` (约 line 3467-3486):
```cpp
case T_STRING: {
    UChar32 res = u8_egc_index_as_single_codepoint(
        sp->u.string, SVALUE_STRLEN(sp), i);
    // res 是 Unicode 码点,不是字节值
    free_string_svalue(sp);
    (--sp)->u.number = res;
    break;
}
```

`u8_egc_index_as_single_codepoint()` 使用 `EGCSmartIterator` 将字符索引转换为字节偏移量,然后提取该位置的 Unicode 码点。

### 修复方案

#### 检查逻辑变更

所有 `check_legal_name()` 函数从字节迭代改为 Unicode 码点迭代:

**修复前** (MudOS GBK 方式):
```c
int check_legal_name(string name)
{
    int i;
    i = strlen(name);
    if( (strlen(name) < 4) || (strlen(name) > 8 ) ) {
        write("对不起，您的中文名字必须是 2 到 4 个中文字。\n");
        return 0;
    }
    while(i--) {
        if( name[i]<=' ' ) { ... }
        if( i%2==0 && !is_chinese(name[i..<0]) ) { ... }
    }
    return 1;
}
```

**修复后** (MudOS-NG Unicode 方式):
```c
int check_legal_name(string name)
{
    int cp, i;
    for (i = 0; ; i++) {
        cp = name[i];
        if (cp == 0) break;           // 字符串结束哨兵
        if (cp <= ' ') { ... }        // 控製字符
        if (cp < 13312 || cp > 40959) // CJK 范围: U+3400-U+9FFF
            return 0;
        if (i >= 4) { ... }           // 最多 4 个字符
    }
    if (i < 2) { ... }                // 最少 2 个字符
    return 1;
}
```

关键差异:
- 用 `for (i = 0; ; i++) { cp = name[i]; if (cp == 0) break; }` 按字符索引迭代
- CJK 检测使用码点范围 `cp >= 13312 && cp <= 40959` (U+3400-U+9FFF)
- 范围检查基于**字符计数**而非字节计数

#### `is_chinese()` 函数变更

```c
// 修复前: 按 GBK 字节值判断 (0xA1-0xFE)
int is_chinese(string str) {
    if( strlen(str)>=2 && str[0] > 160 && str[0] < 255 ) return 1;
    return 0;
}

// 修复后: 按 Unicode 码点判断 (U+3400-U+9FFF)
int is_chinese(string str) {
    if( strlen(str) < 1 ) return 0;
    int cp = str[0];
    if( cp >= 13312 && cp <= 40959 ) return 1;
    return 0;
}
```

### 修改文件清单

共修改 8 个 LPC 源文件:

| 文件 | 说明 |
|------|------|
| `adm/simul_efun/chinese.c` | `is_chinese()` 码点化 |
| `adm/daemons/logind.c` | 登录注册中文名验证 |
| `adm/npc/liu.c` | NPC 打造中文名验证 |
| `adm/npc/smith.c` | NPC 打造中文名验证 |
| `d/jingcheng/npc/liu.c` | 区域 NPC 中文名验证 |
| `d/jingcheng/npc/obj/liu.c` | 区域 NPC 中文名验证 |
| `u/m/masterall/npc/liu.c` | 用户 NPC 中文名验证 |
| `u/l/lucifer/npc/smith.c` | 用户 NPC 中文名验证 |

### 验证

```
3 chars (大河马): ACCEPTED
2 chars (大河):   ACCEPTED
4 chars (大河马河): ACCEPTED
1 char (大):      REJECT(count)
5 chars (大河马河大): REJECT(count)
ASCII (dahema):   REJECT(not Chinese)
mixed (大河a):    REJECT(not Chinese)
```

### 相关 MudOS-NG 源代码位置

- 关键字表: `src/compiler/internal/lex.cc` (约 line 160-220)
- 修饰符定义: `src/vm/internal/base/program.h` (约 line 89-110)
- 函数声明语法: `src/compiler/internal/grammar.y` (约 line 266-421)
- `SENSIBLE_MODIFIERS` 配置: `src/local_options` (line 35)
- UTF-8 字符串校验: `src/vm/internal/base/` (restore_object 实现)
