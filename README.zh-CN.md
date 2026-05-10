# MudOS-ng

**MudOS Next Generation** – 基于 MudOS-NG 代码库的个人延续版，致敬经典的 MudOS v22 驱动。

## 关于本项目

本项目是原 MudOS (v22) 和 MudOS-NG 的个人衍生版本。我对 MudOS 有着很深的情怀，希望在保留其精神的同时，加入一些现代的小修小补。

MudOS-ng **不是**原 MudOS 作者或 MudOS-NG 团队的官方发布版。这只是我个人的兴趣项目，纯粹为了好玩和怀念 – 没有商业目标，没有路线图，只有代码。

## 许可证

本项目使用 **MIT 许可证**（与 MudOS-NG 保持一致）。详见 [LICENSE](./LICENSE) 文件。

- 源自 MudOS-NG 的文件保留 MIT 许可证及原始版权声明。
- 我原创的代码同样采用 MIT 许可证。

## 免责声明

> **本项目并非 MudOS 或 MudOS-NG 官方产品。**  
> 原 MudOS 项目已停止维护多年。本仓库仅为个人兴趣分支。使用风险自负，请勿期待长期支持或稳定版本周期。

## 致谢

- 感谢原 MudOS 团队创造了驱动无数 MUD 游戏的基石。
- 感谢 MudOS-NG 贡献者们让代码库得以存活并现代化。

## 相关项目

- [纵横天下MUD](https://github.com/yeungtuzi/zhtx) — 基于 MudOS-NG 驱动的 MUD 游戏世界

## 主要功能升级（相对 FluffOS 上游）

自 FluffOS 分叉以来新增的主要功能和改进：

### 网络与协议
- **SSH 服务器支持** — 通过 libssh 接受 SSH 连接，支持密码认证。telnet 端口可共存或禁用。
- **TLS/SSL 加密** — telnet 端口支持 TLS 加密，可配置证书和密钥文件。
- **WebSocket 协议** — 原生 WebSocket 支持，面向 Web 端 MUD 客户端，附带 HTTP 调试终端。
- **Telnet 协议修复** — 行模式激活时重新协商 SGA，修复 Mudlet 兼容性等线程安全问题。

### 并发与性能（多线程改造计划）
- **IO 线程池**（第一阶段）— 网络 I/O 卸载到专用线程池，消除 I/O 对主 VM 线程的阻塞。
- **增量对象扫描** — 借鉴 Linux kswapd 思想，容错链表遍历 + 每 tick 批量处理，消除大型 MUD（10000+ 对象）每 5 分钟 ~500ms 的周期性卡顿。
- **VM 状态线程本地化** — 约 35 个 VM 执行寄存器改为 `thread_local`，使各线程可独立执行 LPC 字节码。
- **原子引用计数** — 所有共享数据结构（`object_t`、`array_t`、`mapping_t`、`program_t`）使用 `std::atomic` 引用计数，配以细粒度互斥锁保护。
- **每线程独立执行时限** — eval 时限使用每线程计时器（Linux: `SIGEV_THREAD_ID`，macOS: `dispatch_source`），替代进程级单一信号。
- **心跳线程池** — 可配置的并行心跳执行，支持多工作线程（默认关闭，通过 `heartbeat threads` 配置项启用）。
- **跨线程调用跳转** — 并行心跳执行时自动检测跨对象调用并回退至主线程执行。
- **心跳多线程改造总结** — [详细复盘文档](docs/multithread-heartbeat-postmortem.md)，记录了为期 2.5 天的心跳多线程化全过程：9 个并发 bug 的发现与修复（double-free、死锁、竞态条件、字符串安全性），架构设计思路，锁顺序原则，调试技巧和经验教训。

### 语言与编译器
- **空值合并运算符**（`??`）和**逻辑赋值运算符**（`||=`、`&&=`、`??=`）
- **简化的函数指针语法** — 简洁的 lambda 风格函数引用。
- **macOS 执行时限** — 基于 `setitimer` 的非 Linux 平台执行超时支持。
- **崩溃回溯** — 集成 libunwind，支持 macOS/Linux 的详细崩溃调用栈。

### 基础设施
- **CI/CD 流水线** — GitHub Actions 覆盖 Ubuntu、macOS、Windows、消毒器、Docker、CodeQL 等多种构建配置。
- **UTF-8 支持** — 基于 ICU 的 Unicode 处理全驱动覆盖。
- **数据库集成** — MySQL、PostgreSQL、SQLite 支持，含异步数据库操作。
- **性能追踪** — OpenTelemetry 风格 trace span 和指令级性能分析。
- **静态编译** — 完全静态链接，支持 Docker/Alpine 部署。

## 编译与使用

### 依赖安装

需要 CMake 3.22+、C++17 编译器，以及各平台的依赖库：

**Ubuntu/Debian**

```bash
sudo apt install build-essential bison libmysqlclient-dev libpcre3-dev \
  libpq-dev libsqlite3-dev libssl-dev libz-dev libjemalloc-dev libicu-dev \
  libgtest-dev
```

**macOS (Homebrew)**

```bash
brew install cmake pkg-config mysql pcre libgcrypt openssl jemalloc icu4c \
  sqlite3 googletest
```

**Windows (MSYS2/MINGW64)**

```bash
pacman --noconfirm -S --needed \
  mingw-w64-x86_64-toolchain mingw-w64-x86_64-cmake \
  mingw-w64-x86_64-zlib mingw-w64-x86_64-pcre \
  mingw-w64-x86_64-icu mingw-w64-x86_64-sqlite3 \
  mingw-w64-x86_64-jemalloc mingw-w64-x86_64-gtest bison make
```

**Alpine (Docker/静态编译)**

```bash
apk add --no-cache linux-headers gcc g++ clang-dev make cmake bash \
  mariadb-dev mariadb-static postgresql-dev sqlite-dev sqlite-static \
  openssl-dev openssl-libs-static zlib-dev zlib-static icu-dev icu-static \
  pcre-dev bison git musl-dev libelf-static elfutils-dev \
  zstd-static bzip2-static xz-static
```

### 编译

```bash
mkdir build && cd build
cmake ..
make -j$(nproc) install
```

主要编译选项：

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `CMAKE_BUILD_TYPE` | — | 构建类型：`Debug` / `Release` / `RelWithDebInfo` |
| `MARCH_NATIVE` | `ON` | 针对当前 CPU 优化 |
| `STATIC` | `OFF` | 静态链接（Docker 部署需启用） |
| `USE_JEMALLOC` | `ON` | 使用 jemalloc 内存分配器 |
| `PACKAGE_DB_SQLITE` | — | 启用 SQLite（设为 `2` 使用内置版本） |
| `PACKAGE_DB_MYSQL` | — | 设为 `""` 禁用 MySQL |
| `PACKAGE_CRYPTO` | `ON` | 启用加密包 |
| `ENABLE_SANITIZER` | `OFF` | 启用地址消毒器（需 Debug + Clang） |

**macOS (Apple Silicon)** — 设置 Homebrew 环境变量：

```bash
OPENSSL_ROOT_DIR="/opt/homebrew/opt/openssl@3" \
ICU_ROOT="/opt/homebrew/opt/icu4c" \
cmake .. -DCMAKE_BUILD_TYPE=Release
```

**Windows (MSYS2/MINGW64 终端)** — 禁用加密和 MySQL：

```bash
cmake -G "MSYS Makefiles" -DCMAKE_BUILD_TYPE=Debug \
  -DMARCH_NATIVE=OFF -DPACKAGE_CRYPTO=OFF \
  -DPACKAGE_DB_MYSQL="" -DPACKAGE_DB_SQLITE=1 ..
```

**静态编译 (Docker/Alpine)**：

```bash
cmake .. -DMARCH_NATIVE=OFF -DSTATIC=ON
make install
ldd bin/driver  # 应显示 "not a dynamic executable"
```

### 运行

```bash
./build/bin/driver etc/config.cfg
```

### 测试

```bash
# 单元测试
cd build && make test

# LPC 测试套件
cd testsuite && ../build/bin/driver etc/config.test -ftest
```