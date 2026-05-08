# MudOS-ng

**MudOS Next Generation** – a personal continuation of the classic MudOS v22 driver, based on the FluffOS codebase.

## About

This project is a personal fork and evolution of the original MudOS (v22) and FluffOS. I have deep affection for the MudOS project that started decades ago, and I want to keep its spirit alive with some modern tweaks and fixes.

MudOS-ng is **not an official release** from the original MudOS authors or the FluffOS team. It is my own side project, maintained purely for fun and nostalgia – no commercial ambitions, no roadmap, just code.

## License

This project is released under the **MIT License** (consistent with FluffOS). See the [LICENSE](./LICENSE) file for details.

- Files originally from FluffOS retain their MIT license and original copyright notices.
- New code written by me is also under MIT.

## Disclaimer

> **This is not an official MudOS or FluffOS product.**  
> The original MudOS project has been unmaintained for many years. This repository is a personal hobby fork. Use it at your own risk, and please do not expect any long-term support or stable release schedule.

## Acknowledgements

- The original MudOS team for creating the driver that powered countless MUDs.
- The FluffOS contributors for keeping the codebase alive and modernising it.

## Related Projects

- [纵横天下MUD](https://github.com/yeungtuzi/zhtx) — 基于 MudOS-NG 驱动的 MUD 游戏世界

## Major Upgrades (vs FluffOS upstream)

The following features and improvements have been added since forking from FluffOS:

### Networking & Protocols
- **SSH Server Support** — accept SSH connections via libssh, with password authentication. Telnet port can coexist or be disabled.
- **TLS/SSL Encryption** — TLS support for telnet ports with certificate and key configuration.
- **WebSocket Protocol** — native WebSocket support for web-based MUD clients, with an HTTP debug console.
- **Telnet Protocol Fixes** — re-offer SGA when linemode activates, thread-safety fixes for Mudlet compatibility.

### Concurrency & Performance (Multi-Threading Initiative)
- **IO Thread Pool** (Phase 1) — network I/O offloaded to a dedicated thread pool, eliminating I/O stalls on the main VM thread.
- **Incremental Object Scanning** — fault-tolerant linked-list traversal with batched per-tick processing, inspired by Linux kswapd. Eliminates periodic ~500ms freezes on large MUDs (10000+ objects).
- **Thread-Local VM State** — ~35 VM execution registers converted to `thread_local`, enabling each thread to independently execute LPC bytecode.
- **Atomic Reference Counting** — all shared data structures (`object_t`, `array_t`, `mapping_t`, `program_t`) use `std::atomic` refcounts with fine-grained mutex protection.
- **Per-Thread Eval Timer** — eval cost limit uses per-thread timers (Linux: `SIGEV_THREAD_ID`, macOS: `dispatch_source`) instead of a single process-wide signal.
- **Heartbeat Thread Pool** — configurable parallel heartbeat execution across multiple worker threads (default off, config option `heartbeat threads`).
- **Cross-Thread Call Bounce** — automatic detection and main-thread fallback for cross-object calls during parallel heartbeat execution.

### Language & Compiler
- **Nullish Coalescing** (`??`) and **Logical Assignment Operators** (`||=`, `&&=`, `??=`)
- **Simplified Function Pointer Syntax** — concise lambda-style function references.
- **Eval Limit on macOS** — `setitimer`-based execution timeout for non-Linux platforms.
- **Crash Backtrace** — libunwind integration for detailed crash stack traces on macOS/Linux.

### Infrastructure
- **CI/CD Pipeline** — GitHub Actions for Ubuntu, macOS, Windows, sanitizer, Docker, and CodeQL across multiple build configurations.
- **UTF-8 Support** — ICU-based Unicode handling throughout the driver.
- **Database Integration** — MySQL, PostgreSQL, and SQLite support with async database operations.
- **Performance Tracing** — OpenTelemetry-style trace spans and instruction-level profiling.
- **Static Build** — fully static linking support for Docker/Alpine deployment.

## Build & Usage

### Prerequisites

CMake 3.22+, C++17 compiler, and platform-specific dependencies:

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

**Alpine (Docker/Static)**

```bash
apk add --no-cache linux-headers gcc g++ clang-dev make cmake bash \
  mariadb-dev mariadb-static postgresql-dev sqlite-dev sqlite-static \
  openssl-dev openssl-libs-static zlib-dev zlib-static icu-dev icu-static \
  pcre-dev bison git musl-dev libelf-static elfutils-dev \
  zstd-static bzip2-static xz-static
```

### Build

```bash
mkdir build && cd build
cmake ..
make -j$(nproc) install
```

Key build options:

| Option | Default | Description |
|--------|---------|-------------|
| `CMAKE_BUILD_TYPE` | — | `Debug`, `Release`, or `RelWithDebInfo` |
| `MARCH_NATIVE` | `ON` | Optimize for current CPU |
| `STATIC` | `OFF` | Static linking (required for Docker) |
| `USE_JEMALLOC` | `ON` | Use jemalloc allocator |
| `PACKAGE_DB_SQLITE` | — | Enable SQLite (`2` = built-in) |
| `PACKAGE_DB_MYSQL` | — | Set to `""` to disable MySQL |
| `PACKAGE_CRYPTO` | `ON` | Enable crypto package |
| `ENABLE_SANITIZER` | `OFF` | Enable address sanitizer (Debug, Clang) |

**macOS (Apple Silicon)** — set environment variables for Homebrew paths:

```bash
OPENSSL_ROOT_DIR="/opt/homebrew/opt/openssl@3" \
ICU_ROOT="/opt/homebrew/opt/icu4c" \
cmake .. -DCMAKE_BUILD_TYPE=Release
```

**Windows (MSYS2/MINGW64 shell)** — disable crypto and MySQL:

```bash
cmake -G "MSYS Makefiles" -DCMAKE_BUILD_TYPE=Debug \
  -DMARCH_NATIVE=OFF -DPACKAGE_CRYPTO=OFF \
  -DPACKAGE_DB_MYSQL="" -DPACKAGE_DB_SQLITE=1 ..
```

**Static build (Docker/Alpine)**:

```bash
cmake .. -DMARCH_NATIVE=OFF -DSTATIC=ON
make install
ldd bin/driver  # Should show "not a dynamic executable"
```

### Run

```bash
./build/bin/driver etc/config.cfg
```

### Test

```bash
# Unit tests
cd build && make test

# LPC test suite
cd testsuite && ../build/bin/driver etc/config.test -ftest
```