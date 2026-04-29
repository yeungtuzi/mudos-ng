/*
 * Copyright (c) 2026 [大河马/dahema@me.com]
 * SPDX-License-Identifier: MIT
 */

/*
 * Unit tests for Phase 1 I/O thread infrastructure.
 *
 * Tests cover:
 *   - IOThread lifecycle (create, start, stop)
 *   - Task posting and FIFO ordering
 *   - Task execution on the correct thread
 *   - Concurrent posting from multiple threads
 *   - Nested posting (post from within IO thread)
 *   - IOThreadPool round-robin distribution
 *   - Multiple pool sizes
 *   - event_base_once on IO thread's event_base
 */

#include "base/internal/io_thread.h"

#include <event2/event.h>
#include <event2/thread.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

// Per-test-suite setup: enable libevent threading (required for
// evthread_make_base_notifiable inside IOThread::start()).
class IOThreadEnvironment : public ::testing::Environment {
 public:
  void SetUp() override {
    evthread_use_pthreads();
  }
};
static auto g_env = ::testing::AddGlobalTestEnvironment(new IOThreadEnvironment);

// ============================================================================
// IOThread unit tests
// ============================================================================

TEST(IOThreadTest, CreateStartStop) {
  IOThread thread(0);
  thread.start();
  thread.stop();
}

TEST(IOThreadTest, PostSingleTask) {
  IOThread thread(0);
  thread.start();

  std::atomic<bool> executed{false};
  thread.post([&]() { executed = true; });

  // Wait for the task to execute (max ~1s)
  for (int i = 0; i < 100 && !executed; i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  EXPECT_TRUE(executed);
  thread.stop();
}

TEST(IOThreadTest, TaskRunsOnIOThread) {
  IOThread thread(0);
  thread.start();

  auto caller_id = std::this_thread::get_id();
  std::atomic<std::thread::id> task_thread_id{caller_id};

  thread.post([&]() { task_thread_id = std::this_thread::get_id(); });

  for (int i = 0; i < 100 && task_thread_id == caller_id; i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  EXPECT_NE(task_thread_id, caller_id);
  thread.stop();
}

TEST(IOThreadTest, TaskFifoOrdering) {
  IOThread thread(0);
  thread.start();

  std::vector<int> order;
  std::mutex order_mutex;

  for (int i = 0; i < 10; i++) {
    thread.post(
        [i, &order, &order_mutex]() {
          std::lock_guard<std::mutex> lock(order_mutex);
          order.push_back(i);
        });
  }

  // Sentinel task – when it completes, all prior tasks are done.
  std::atomic<bool> done{false};
  thread.post([&]() { done = true; });
  for (int i = 0; i < 100 && !done; i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  ASSERT_EQ(order.size(), 10);
  for (int i = 0; i < 10; i++) {
    EXPECT_EQ(order[i], i) << "Task at position " << i << " out of order";
  }
  thread.stop();
}

TEST(IOThreadTest, ConcurrentPosting) {
  constexpr int kNumPosters = 4;
  constexpr int kTasksPerPoster = 100;

  IOThread thread(0);
  thread.start();

  std::atomic<int> counter{0};

  std::vector<std::thread> posters;
  for (int p = 0; p < kNumPosters; p++) {
    posters.emplace_back([&]() {
      for (int i = 0; i < kTasksPerPoster; i++) {
        thread.post([&]() { counter.fetch_add(1, std::memory_order_relaxed); });
      }
    });
  }
  for (auto &t : posters) {
    t.join();
  }

  std::atomic<bool> done{false};
  thread.post([&]() { done = true; });
  for (int i = 0; i < 200 && !done; i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  EXPECT_EQ(counter.load(), kNumPosters * kTasksPerPoster);
  thread.stop();
}

TEST(IOThreadTest, MultipleStartStopCycles) {
  IOThread thread(0);
  for (int cycle = 0; cycle < 5; cycle++) {
    thread.start();
    std::atomic<bool> executed{false};
    thread.post([&]() { executed = true; });
    for (int i = 0; i < 100 && !executed; i++) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_TRUE(executed);
    thread.stop();
  }
}

// Post from inside a task running on the IO thread itself.
TEST(IOThreadTest, NestedPost) {
  IOThread thread(0);
  thread.start();

  std::atomic<int> counter{0};
  std::atomic<bool> inner_done{false};

  thread.post([&]() {
    counter.store(1);
    thread.post([&]() {
      counter.store(2);
      inner_done = true;
    });
  });

  for (int i = 0; i < 200 && !inner_done; i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  EXPECT_EQ(counter.load(), 2);
  EXPECT_TRUE(inner_done);
  thread.stop();
}

// event_base_once on the IO thread's event_base fires on the IO thread.
TEST(IOThreadTest, EventBaseOnceOnIOThread) {
  IOThread thread(0);
  thread.start();

  auto caller_id = std::this_thread::get_id();
  auto *task_id = new std::atomic<std::thread::id>(caller_id);

  event_base_once(
      thread.base(), -1, EV_TIMEOUT,
      [](evutil_socket_t /*fd*/, short /*what*/, void *arg) {
        *static_cast<std::atomic<std::thread::id> *>(arg) =
            std::this_thread::get_id();
      },
      task_id, nullptr);

  for (int i = 0; i < 100 && *task_id == caller_id; i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  EXPECT_NE(*task_id, caller_id);
  delete task_id;
  thread.stop();
}

// ============================================================================
// IOThreadPool unit tests
// ============================================================================

TEST(IOThreadPoolTest, CreateAndDestroy) {
  IOThreadPool pool(2);
  pool.start();
  pool.stop();
}

TEST(IOThreadPoolTest, SingleThreadDistribution) {
  IOThreadPool pool(1);
  pool.start();

  auto *t1 = pool.next_thread();
  auto *t2 = pool.next_thread();
  auto *t3 = pool.next_thread();
  EXPECT_EQ(t1, t2);
  EXPECT_EQ(t1, t3);

  std::atomic<bool> executed{false};
  t1->post([&]() { executed = true; });
  for (int i = 0; i < 100 && !executed; i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_TRUE(executed);
  pool.stop();
}

TEST(IOThreadPoolTest, MultipleThreadsRoundRobin) {
  IOThreadPool pool(3);
  pool.start();

  // Collect threads returned by 12 consecutive next_thread() calls.
  std::vector<IOThread *> picked;
  for (int i = 0; i < 12; i++) {
    picked.push_back(pool.next_thread());
  }

  // Verify all 3 distinct threads are seen.
  std::set<IOThread *> distinct(picked.begin(), picked.end());
  EXPECT_EQ(distinct.size(), 3);

  // Verify round-robin pattern.
  for (int i = 0; i < 9; i++) {
    EXPECT_EQ(picked[i + 3], picked[i])
        << "Round-robin cycle broken at index " << i;
  }

  pool.stop();
}

TEST(IOThreadPoolTest, ThreadIdAssignment) {
  IOThreadPool pool(4);
  pool.start();

  std::set<int> ids;
  for (int i = 0; i < 4; i++) {
    auto *t = pool.next_thread();
    ids.insert(t->id());
  }

  EXPECT_EQ(ids.size(), 4);
  EXPECT_TRUE(ids.find(0) != ids.end());
  EXPECT_TRUE(ids.find(1) != ids.end());
  EXPECT_TRUE(ids.find(2) != ids.end());
  EXPECT_TRUE(ids.find(3) != ids.end());

  pool.stop();
}

TEST(IOThreadPoolTest, TasksRunOnCorrectThreads) {
  IOThreadPool pool(2);
  pool.start();

  auto *t0 = pool.next_thread();
  auto *t1 = pool.next_thread();
  ASSERT_NE(t0, t1);

  std::atomic<std::thread::id> id0{std::thread::id()};
  std::atomic<std::thread::id> id1{std::thread::id()};

  t0->post([&]() { id0 = std::this_thread::get_id(); });
  t1->post([&]() { id1 = std::this_thread::get_id(); });

  for (int i = 0; i < 100 && (id0 == std::thread::id() || id1 == std::thread::id()); i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  EXPECT_NE(id0, std::thread::id());
  EXPECT_NE(id1, std::thread::id());
  EXPECT_NE(id0, id1);  // different IO threads

  // Tasks on each thread should consistently run on that thread.
  std::atomic<int> count0{0};
  std::atomic<int> count1{0};

  std::atomic<bool> done0{false};
  std::atomic<bool> done1{false};

  t0->post([&]() {
    count0 = 1;
    done0 = true;
  });
  t1->post([&]() {
    count1 = 1;
    done1 = true;
  });

  for (int i = 0; i < 100 && (!done0 || !done1); i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  EXPECT_EQ(count0.load(), 1);
  EXPECT_EQ(count1.load(), 1);

  pool.stop();
}

// Stress test: N threads each post M tasks, all distributed round-robin.
TEST(IOThreadPoolTest, ConcurrentRoundRobinWork) {
  constexpr int kNumPosters = 4;
  constexpr int kTasksPerPoster = 50;

  IOThreadPool pool(3);
  pool.start();

  std::atomic<int> total{0};
  std::atomic<bool> done{false};

  std::vector<std::thread> posters;
  for (int p = 0; p < kNumPosters; p++) {
    posters.emplace_back([&]() {
      for (int i = 0; i < kTasksPerPoster; i++) {
        auto *t = pool.next_thread();
        t->post([&]() { total.fetch_add(1, std::memory_order_relaxed); });
      }
    });
  }
  for (auto &t : posters) {
    t.join();
  }

  // Drain all tasks
  int expected = kNumPosters * kTasksPerPoster;
  for (int i = 0; i < 200 && total.load() < expected; i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  EXPECT_EQ(total.load(), expected);
  pool.stop();
}
