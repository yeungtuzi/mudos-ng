<!--
Copyright (c) 2026 [大河马/dahema@me.com]
SPDX-License-Identifier: MIT
-->

# Phase 1: I/O Thread Separation

## Overview

Phase 1 moves network I/O handling from the single VM/event-loop thread to dedicated I/O threads, eliminating direct blocking of LPC execution by network operations. Each I/O thread runs its own `event_base_loop`, managing bufferevent read/write, telnet negotiation, and connection lifecycle for a subset of users.

## Architecture

```
┌─────────────────────────┐       Lock-free Queue        ┌─────────────────────────┐
│   IO Thread Pool        │  ◄───────────────────────►   │    VM Thread            │
│   (2 threads)           │    user command (via         │                         │
│                         │    event_base_once)          │  ┌───────────────────┐  │
│  ┌───────────────────┐  │    output messages (via      │  │ libevent event_base│  │
│  │ IOThread #0       │  │    io_thread->post)          │  │ game loop (tick)   │  │
│  │ event_base        │  │                              │  │ heartbeat          │  │
│  │ kqueue/epoll      │  │                              │  │ call_out           │  │
│  │ - evconnlistener  │  │                              │  │ LPC eval           │  │
│  │ - bufferevent     │  │                              │  │ remove_interactive │  │
│  │ - telnet          │  │                              │  │ GC/reclaim         │  │
│  │ - TLS             │  │                              │  └───────────────────┘  │
│  └───────────────────┘  │                              └─────────────────────────┘
│  ┌───────────────────┐  │
│  │ IOThread #1       │  │
│  │ (same structure)  │  │
│  └───────────────────┘  │
└─────────────────────────┘
```

### Cross-thread Communication

- **I/O → VM (commands)**: `event_base_once(g_event_base, -1, EV_TIMEOUT, ...)` schedules a callback on the VM thread's event loop. Used for `on_user_command()` and `on_user_logon()`.
- **I/O → VM (user events)**: Same mechanism for `remove_interactive()` when triggered by connection errors/EOF.
- **VM → I/O (output)**: `IOThread::post()` pushes a task to the IO thread's MPSC task queue and wakes its event loop via socketpair write. Used by `add_message()` for actual socket writes.

## Files Changed

### New Files

| File | Purpose |
|------|---------|
| `src/base/internal/io_thread.h` | IOThread and IOThreadPool class declarations |
| `src/base/internal/io_thread.cc` | Full IOThread and IOThreadPool implementation |
| `src/tests/test_io_thread.cc` | 14 unit tests covering IOThread and IOThreadPool |

### Modified Files

| File | Change |
|------|--------|
| `src/backend.cc` | Added `evthread_make_base_notifiable(g_event_base)` after `event_base_new()` to enable cross-thread event injection |
| `src/comm.cc` | Split network I/O from LPC command processing; added I/O thread routing in `new_conn_handler()`, `on_user_command()`, `on_user_events()`, `add_message()` |
| `src/interactive.h` | Added `IOThread *io_thread{nullptr}` field to `interactive_t` |
| `src/mainlib.cc` | Added IO thread pool lifecycle (start before `backend()`, stop after) |
| `src/CMakeLists.txt` | Added `"base/internal/io_thread.cc"` to SRC list |
| `src/tests/CMakeLists.txt` | Added `io_thread_tests` executable and gtest discovery |

## IOThread Implementation

### IOThread Class

```
IOThread
├── id_                         : Thread index (0, 1, ...)
├── base_                       : Own libevent event_base
├── wakeup_fds_[2]              : Socketpair for cross-thread wakeup
│                                (portable: macOS lacks eventfd)
├── wakeup_event_               : EV_READ|EV_PERSIST on wakeup_fds_[0]
├── thread_                     : std::thread running event_loop()
├── running_                    : std::atomic<bool>
├── mutex_ + tasks_             : MPSC task queue (std::deque)
└── post(std::function<void()>) : Thread-safe, non-blocking task submission
```

### IOThreadPool Class

```
IOThreadPool
├── threads_        : std::vector<std::unique_ptr<IOThread>>
├── next_           : std::atomic<size_t> for lock-free round-robin
└── next_thread()   : Returns next IOThread* (fetch_add % size)
```

### Stop Mechanism (Reliability Fix)

The event loop uses `while (running_) { event_base_loop(base_, EVLOOP_ONCE); }` instead of relying on `event_base_loopbreak()`. This avoids a macOS/kqueue issue where `event_base_loopbreak()`'s internal notification can fail to wake a blocked kqueue. The wakeup socketpair write always triggers EVFILT_READ on the IO thread, `EVLOOP_ONCE` unwinds to check `running_`, and the loop exits cleanly.

## Network I/O Split Details

### Connection Flow (Before → After)

**Before Phase 1** (all on VM thread):
```
new_conn_handler()
  → new_user() creates ev_command on VM thread's base
  → new_user_event_listener(VM base)
  → on_user_logon() via event_base_once
  → on_user_command() on VM event loop
  → add_message() writes directly to bufferevent
```

**After Phase 1**:
```
new_conn_handler()
  → new_user() (ev_command deferred to IO thread)
  → g_io_thread_pool->next_thread() picks IOThread
  → IOThread::post() schedules on IO thread:
      → new_user_event_listener(IO thread's base)
      → evtimer_new(IO thread's base, on_user_command)
      → telnet init
      → on_user_logon() via event_base_once(g_event_base, ...) back to VM thread
  → on_user_command() on IO thread
      → event_base_once(g_event_base, ...) bounces to VM thread
      → process_user_command() runs on VM thread
  → add_message() on VM thread
      → ip->io_thread->post() to IO thread
      → output_to_user() writes to bufferevent on IO thread
```

### Cross-thread Safety Points

1. **remove_interactive()**: Bounced to VM thread via `event_base_once` when triggered from IO thread events (BEV_EVENT_ERROR/BEV_EVENT_EOF), because it calls LPC applies.

2. **Two-phase cleanup**: When a connection dies on the IO thread, the user is flagged `NET_DEAD` immediately. Actual `remove_interactive()` runs on the VM thread. The IO thread's bufferevent remains valid until the VM thread processes removal.

3. **add_message()** output path: Shadow/snoop LPC processing stays on the VM thread. The actual socket write is heap-allocated, copied, and posted to the IO thread.

## Unit Tests

`src/tests/test_io_thread.cc` contains 14 tests across 2 test suites:

### IOThreadTest (8 tests)
| Test | Description |
|------|-------------|
| CreateStartStop | Basic lifecycle: create, start, stop |
| PostSingleTask | Post one task, verify execution |
| TaskRunsOnIOThread | Verify task runs on a different thread than caller |
| TaskFifoOrdering | 10 tasks posted in sequence execute in FIFO order |
| ConcurrentPosting | 4 threads each post 100 tasks concurrently |
| MultipleStartStopCycles | Start/stop 5 times, verifying task execution each cycle |
| NestedPost | Post from within a task running on the IO thread |
| EventBaseOnceOnIOThread | `event_base_once` on IO thread's base fires on IO thread |

### IOThreadPoolTest (6 tests)
| Test | Description |
|------|-------------|
| CreateAndDestroy | Pool lifecycle with 2 threads |
| SingleThreadDistribution | Pool with 1 thread always returns same thread |
| MultipleThreadsRoundRobin | 3 threads across 12 calls verify round-robin pattern |
| ThreadIdAssignment | 4 threads have distinct IDs 0-3 |
| TasksRunOnCorrectThreads | Verify tasks land on distinct threads with correct IDs |
| ConcurrentRoundRobinWork | 4 posters x 50 tasks across 3 IO threads stress test |

## Build Integration

- `io_thread_tests` links against `${MUDOS_NG_LINK}` (the main driver library) and `GTest::GTest`, `GTest::Main`.
- Build and run: `cmake --build build -j$(nproc) && ./build/src/tests/io_thread_tests`
- All 14 tests pass reliably across consecutive runs (verified 10x).
- Existing test suites (lpc_tests, ofile_tests, LPC integration testsuite) remain unaffected.

## Configuration

IO thread pool size is hardcoded to 2 in `mainlib.cc`:
```cpp
g_io_thread_pool = new IOThreadPool(2);
```
This can be made configurable via the driver config file in a follow-up change.

## Known Limitations

- LPC-intensive scenarios (many concurrent heartbeats, long loops) can still block the VM thread. This is addressed by Phase 2 (fiber/coroutine cooperative scheduling).
- The IO thread pool size is currently hardcoded. Future work should expose it as a config option.
- DNS callbacks and WebSocket handshake completion still need routing to the VM thread for LPC-related processing.

## Verification

All tests pass:
- Unit tests: 14/14 io_thread_tests, 2/2 ofile_tests, 5/5 lpc_tests
- Integration: LPC testsuite ("all tests finished, shutting down")
