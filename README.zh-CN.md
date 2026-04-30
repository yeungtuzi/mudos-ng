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