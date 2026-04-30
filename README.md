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

## Known Issues

A previous automated cleanup pass commented out unused local variable declarations across the codebase. This may inadvertently cause bugs where a variable was actually needed. If you encounter unexpected behavior, search for comments with specific dates and function descriptions (e.g., `// 2026-xx-xx unused`) and try uncommenting the variable declaration.

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