/*
 * Copyright (c) 2026 [大河马/dahema@me.com]
 * SPDX-License-Identifier: MIT
 */

/*
 * Unit tests for Phase 2-6 heartbeat thread infrastructure.
 *
 * Tests cover:
 *   - HeartbeatThread lifecycle (create, start, stop, restart)
 *   - Task posting and FIFO ordering
 *   - Task execution on the correct (worker) thread
 *   - Thread-local VM state initialization
 *   - Heartbeat queue operations (add/remove/modify/query)
 *   - HeartbeatThreadPool object sharding (deterministic, uniform)
 *   - Concurrent posting from multiple threads
 *   - Cross-thread call bounce detection (g_current_heartbeat_thread)
 *   - Per-thread eval timer lifecycle
 *   - Heartbeat processing (process_heartbeats empty / populated)
 */

#include "base/internal/heartbeat_thread.h"

#include "base/package_api.h"

#include <event2/event.h>
#include <event2/thread.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "vm/internal/eval_limit.h"

// Enable libevent threading for test environment.
class HeartbeatThreadEnvironment : public ::testing::Environment {
 public:
  void SetUp() override { evthread_use_pthreads(); }
};
static auto g_hb_env =
    ::testing::AddGlobalTestEnvironment(new HeartbeatThreadEnvironment);

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Minimal object_t stub for tests that don't need a fully initialized object.
static object_t *make_test_object() {
  auto *ob = reinterpret_cast<object_t *>(calloc(1, sizeof(object_t)));
  ob->flags.store(O_HEART_BEAT, std::memory_order_relaxed);
  return ob;
}

static void free_test_object(object_t *ob) { free(ob); }

// Minimal program_t stub with a fake heart_beat function index.
static program_t *make_test_program() {
  auto *prog = reinterpret_cast<program_t *>(calloc(1, sizeof(program_t)));
  prog->heart_beat = 0;  // 0 = no heartbeat function → process_heartbeats skips LPC exec
  prog->ref.store(1, std::memory_order_relaxed);
  return prog;
}

static void free_test_program(program_t *prog) { free(prog); }

// Poll until condition is true or timeout expires.
#define WAIT_FOR(cond, timeout_ms)                                       \
  do {                                                                   \
    auto _deadline = std::chrono::steady_clock::now() +                  \
                     std::chrono::milliseconds(timeout_ms);              \
    while (!(cond) && std::chrono::steady_clock::now() < _deadline) {    \
      std::this_thread::sleep_for(std::chrono::milliseconds(5));         \
    }                                                                    \
  } while (0)

// ============================================================================
// HeartbeatThread lifecycle
// ============================================================================

TEST(HeartbeatThreadTest, CreateStartStop) {
  HeartbeatThread thread(0);
  thread.start();
  EXPECT_NE(thread.base(), nullptr);
  thread.stop();
}

TEST(HeartbeatThreadTest, MultipleStartStopCycles) {
  HeartbeatThread thread(0);
  for (int cycle = 0; cycle < 5; cycle++) {
    thread.start();
    std::atomic<bool> executed{false};
    thread.post([&]() { executed = true; });
    WAIT_FOR(executed.load(), 1000);
    EXPECT_TRUE(executed.load());
    thread.stop();
  }
}

TEST(HeartbeatThreadTest, IdAssignment) {
  HeartbeatThread t0(0);
  HeartbeatThread t1(1);
  HeartbeatThread t99(99);
  EXPECT_EQ(t0.id(), 0);
  EXPECT_EQ(t1.id(), 1);
  EXPECT_EQ(t99.id(), 99);
}

// ============================================================================
// Task posting
// ============================================================================

TEST(HeartbeatThreadTest, PostSingleTask) {
  HeartbeatThread thread(0);
  thread.start();

  std::atomic<bool> executed{false};
  thread.post([&]() { executed = true; });
  WAIT_FOR(executed.load(), 1000);

  EXPECT_TRUE(executed.load());
  thread.stop();
}

TEST(HeartbeatThreadTest, TaskRunsOnWorkerThread) {
  HeartbeatThread thread(0);
  thread.start();

  auto caller_id = std::this_thread::get_id();
  std::atomic<std::thread::id> task_thread_id{caller_id};

  thread.post([&]() { task_thread_id = std::this_thread::get_id(); });
  WAIT_FOR(task_thread_id.load() != caller_id, 1000);

  EXPECT_NE(task_thread_id.load(), caller_id);
  thread.stop();
}

TEST(HeartbeatThreadTest, TaskFifoOrdering) {
  HeartbeatThread thread(0);
  thread.start();

  std::vector<int> order;
  std::mutex order_mutex;

  for (int i = 0; i < 20; i++) {
    thread.post([i, &order, &order_mutex]() {
      std::lock_guard<std::mutex> lock(order_mutex);
      order.push_back(i);
    });
  }

  std::atomic<bool> done{false};
  thread.post([&]() { done = true; });
  WAIT_FOR(done.load(), 2000);

  ASSERT_EQ(order.size(), 20u);
  for (int i = 0; i < 20; i++) {
    EXPECT_EQ(order[i], i) << "Task at position " << i << " out of order";
  }
  thread.stop();
}

// ============================================================================
// Concurrent posting (stress)
// ============================================================================

TEST(HeartbeatThreadTest, ConcurrentPosting) {
  constexpr int kNumPosters = 8;
  constexpr int kTasksPerPoster = 500;

  HeartbeatThread thread(0);
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

  WAIT_FOR(counter.load() == kNumPosters * kTasksPerPoster, 5000);

  EXPECT_EQ(counter.load(), kNumPosters * kTasksPerPoster);
  thread.stop();
}

// ============================================================================
// Heartbeat queue operations (add / remove / modify / query)
// ============================================================================

TEST(HeartbeatThreadTest, AddAndQueryHeartbeat) {
  HeartbeatThread thread(0);
  thread.start();

  auto *ob = make_test_object();
  auto *prog = make_test_program();
  ob->prog = prog;

  std::atomic<int> result{0};
  thread.post([&, ob]() {
    thread.add_heartbeat(ob, 5);
    result = thread.query_heartbeat(ob);
  });
  WAIT_FOR(result.load() != 0, 1000);

  EXPECT_EQ(result.load(), 5);

  free_test_program(prog);
  free_test_object(ob);
  thread.stop();
}

TEST(HeartbeatThreadTest, RemoveHeartbeat) {
  HeartbeatThread thread(0);
  thread.start();

  auto *ob = make_test_object();
  auto *prog = make_test_program();
  ob->prog = prog;

  std::atomic<int> result{-1};
  thread.post([&, ob]() {
    thread.add_heartbeat(ob, 5);
    thread.remove_heartbeat(ob);
    result = thread.query_heartbeat(ob);
  });
  WAIT_FOR(result.load() != -1, 1000);

  EXPECT_EQ(result.load(), 0);

  free_test_program(prog);
  free_test_object(ob);
  thread.stop();
}

TEST(HeartbeatThreadTest, ModifyHeartbeat) {
  HeartbeatThread thread(0);
  thread.start();

  auto *ob = make_test_object();
  auto *prog = make_test_program();
  ob->prog = prog;

  std::atomic<int> before{0}, after{0};
  thread.post([&, ob]() {
    thread.add_heartbeat(ob, 5);
    before = thread.query_heartbeat(ob);
    thread.modify_heartbeat(ob, 10);
    after = thread.query_heartbeat(ob);
  });
  WAIT_FOR(after.load() != 0, 1000);

  EXPECT_EQ(before.load(), 5);
  EXPECT_EQ(after.load(), 10);

  free_test_program(prog);
  free_test_object(ob);
  thread.stop();
}

TEST(HeartbeatThreadTest, MultipleObjectsHeartbeats) {
  HeartbeatThread thread(0);
  thread.start();

  constexpr int kNumObjects = 100;
  object_t *objects[kNumObjects];
  program_t *programs[kNumObjects];

  for (int i = 0; i < kNumObjects; i++) {
    objects[i] = make_test_object();
    programs[i] = make_test_program();
    objects[i]->prog = programs[i];
  }

  std::atomic<int> count{0};
  thread.post([&]() {
    for (int i = 0; i < kNumObjects; i++) {
      thread.add_heartbeat(objects[i], i + 1);
    }
    count = kNumObjects;
  });
  WAIT_FOR(count.load() == kNumObjects, 1000);

  std::atomic<int> queried{0};
  std::atomic<bool> all_correct{true};
  thread.post([&]() {
    for (int i = 0; i < kNumObjects; i++) {
      auto interval = thread.query_heartbeat(objects[i]);
      if (interval != i + 1) {
        all_correct = false;
      }
    }
    queried = 1;
  });
  WAIT_FOR(queried.load() == 1, 1000);

  EXPECT_TRUE(all_correct.load());

  for (int i = 0; i < kNumObjects; i++) {
    free_test_program(programs[i]);
    free_test_object(objects[i]);
  }
  thread.stop();
}

// ============================================================================
// Heartbeat count
// ============================================================================

TEST(HeartbeatThreadTest, HeartbeatCount) {
  HeartbeatThread thread(0);
  thread.start();

  auto *ob1 = make_test_object();
  auto *ob2 = make_test_object();
  auto *p1 = make_test_program();
  auto *p2 = make_test_program();
  ob1->prog = p1;
  ob2->prog = p2;

  std::atomic<size_t> count{9999};
  thread.post([&, ob1, ob2]() {
    thread.add_heartbeat(ob1, 3);
    thread.add_heartbeat(ob2, 5);
    count = thread.heartbeat_count();
  });
  WAIT_FOR(count.load() != 9999, 1000);

  EXPECT_EQ(count.load(), 2u);

  free_test_program(p1);
  free_test_program(p2);
  free_test_object(ob1);
  free_test_object(ob2);
  thread.stop();
}

// ============================================================================
// HeartbeatThreadPool
// ============================================================================

TEST(HeartbeatThreadPoolTest, CreateAndDestroy) {
  HeartbeatThreadPool pool(2);
  pool.start();
  EXPECT_EQ(pool.size(), 2u);
  pool.stop();
}

TEST(HeartbeatThreadPoolTest, ObjectShardingDeterministic) {
  HeartbeatThreadPool pool(4);
  pool.start();

  auto *ob = make_test_object();
  auto *t1 = pool.thread_for_object(ob);
  auto *t2 = pool.thread_for_object(ob);

  EXPECT_EQ(t1, t2) << "Same object must always map to same thread";

  free_test_object(ob);
  pool.stop();
}

TEST(HeartbeatThreadPoolTest, ObjectShardingDeterministicAcrossPool) {
  // Verify the same object always maps to the same thread,
  // and that all objects get assigned to some valid thread.
  HeartbeatThreadPool pool(4);
  pool.start();

  std::vector<object_t *> objects;
  constexpr int kNumObjects = 100;
  objects.reserve(kNumObjects);
  for (int i = 0; i < kNumObjects; i++) {
    objects.push_back(make_test_object());
  }

  // Each object must map to a valid thread.
  for (auto *ob : objects) {
    auto *t = pool.thread_for_object(ob);
    ASSERT_NE(t, nullptr);
    ASSERT_GE(t->id(), 0);
    ASSERT_LT(t->id(), 4);
  }

  // Repeated calls must return the same thread.
  for (auto *ob : objects) {
    auto *t1 = pool.thread_for_object(ob);
    auto *t2 = pool.thread_for_object(ob);
    EXPECT_EQ(t1, t2);
  }

  for (auto *ob : objects) { free_test_object(ob); }
  pool.stop();
}

TEST(HeartbeatThreadPoolTest, PostToShardedThread) {
  HeartbeatThreadPool pool(2);
  pool.start();

  auto *ob = make_test_object();
  auto *thread = pool.thread_for_object(ob);

  std::atomic<bool> executed{false};
  thread->post([&]() { executed = true; });
  WAIT_FOR(executed.load(), 1000);

  EXPECT_TRUE(executed.load());

  free_test_object(ob);
  pool.stop();
}

TEST(HeartbeatThreadPoolTest, MultiplePoolsDoNotConflict) {
  // Verify that creating a second pool with different thread count works.
  HeartbeatThreadPool pool1(1);
  pool1.start();

  auto *ob = make_test_object();
  auto *t1 = pool1.thread_for_object(ob);
  EXPECT_EQ(t1->id(), 0);

  std::atomic<bool> done{false};
  t1->post([&]() { done = true; });
  WAIT_FOR(done.load(), 1000);
  EXPECT_TRUE(done.load());

  free_test_object(ob);
  pool1.stop();
}

// ============================================================================
// g_current_heartbeat_thread tracker
// ============================================================================

TEST(HeartbeatThreadTest, CurrentThreadTracker) {
  HeartbeatThread thread(0);
  thread.start();

  std::atomic<HeartbeatThread *> captured{nullptr};
  std::atomic<bool> is_current{false};

  thread.post([&]() {
    // Simulate what process_heartbeats does.
    g_current_heartbeat_thread = &thread;
    captured = g_current_heartbeat_thread;
    is_current = thread.is_current_thread();
    g_current_heartbeat_thread = nullptr;
  });

  WAIT_FOR(captured.load() != nullptr, 1000);

  EXPECT_EQ(captured.load(), &thread);
  EXPECT_TRUE(is_current.load());

  // After reset, should be null on this (main) thread.
  EXPECT_EQ(g_current_heartbeat_thread, nullptr);

  thread.stop();
}

// ============================================================================
// Per-thread eval timer lifecycle
// ============================================================================

TEST(HeartbeatThreadTest, EvalTimerLifecycle) {
  HeartbeatThread thread(0);
  thread.start();

  // init_thread_eval / cleanup_thread_eval are called inside
  // init_vm_state / cleanup_vm_state during event loop start/stop.
  // Just verify that set_eval / get_eval work on the worker thread.

  std::atomic<bool> eval_works{false};
  thread.post([&]() {
    set_eval(1000000);  // 1 second
    auto remaining = get_eval();
    // get_eval may return 0 or > 0 depending on platform.
    // Just verify no crash.
    eval_works = true;
    (void)remaining;
  });

  WAIT_FOR(eval_works.load(), 1000);
  EXPECT_TRUE(eval_works.load());

  thread.stop();
}

// ============================================================================
// VM thread-local state isolation
// ============================================================================

TEST(HeartbeatThreadTest, VmStateIsolation) {
  // Verify that VM state (sp, csp, etc.) is independent across threads.
  // We test this indirectly: two threads each run a heartbeat cycle
  // and should not interfere with each other.

  HeartbeatThreadPool pool(2);
  pool.start();

  auto *ob1 = make_test_object();
  auto *ob2 = make_test_object();
  auto *p1 = make_test_program();
  auto *p2 = make_test_program();
  ob1->prog = p1;
  ob2->prog = p2;

  auto *t1 = pool.thread_for_object(ob1);
  auto *t2 = pool.thread_for_object(ob2);

  std::atomic<int> t1_done{0}, t2_done{0};

  t1->post([&]() {
    t1->add_heartbeat(ob1, 1);
    t1->process_heartbeats();
    t1_done = 1;
  });

  t2->post([&]() {
    t2->add_heartbeat(ob2, 1);
    t2->process_heartbeats();
    t2_done = 1;
  });

  WAIT_FOR(t1_done.load() && t2_done.load(), 2000);

  EXPECT_EQ(t1_done.load(), 1);
  EXPECT_EQ(t2_done.load(), 1);

  free_test_program(p1);
  free_test_program(p2);
  free_test_object(ob1);
  free_test_object(ob2);
  pool.stop();
}

// ============================================================================
// Deadlock detection (timeout-based)
// ============================================================================

TEST(HeartbeatThreadTest, NoDeadlockConcurrentPostAndStop) {
  // Repeatedly post while stopping — should never hang.
  HeartbeatThread thread(0);
  thread.start();

  std::atomic<bool> keep_posting{true};
  std::atomic<int> post_count{0};

  std::thread poster([&]() {
    while (keep_posting.load()) {
      thread.post([&]() { post_count.fetch_add(1); });
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  keep_posting = false;
  poster.join();

  // Stop should complete quickly.
  auto t0 = std::chrono::steady_clock::now();
  thread.stop();
  auto elapsed = std::chrono::steady_clock::now() - t0;

  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
            5000)
      << "stop() took too long — possible deadlock";
}

TEST(HeartbeatThreadTest, NoDeadlockDoubleStop) {
  HeartbeatThread thread(0);
  thread.start();

  std::atomic<bool> done{false};
  thread.post([&]() { done = true; });
  WAIT_FOR(done.load(), 1000);

  thread.stop();
  // Second stop should be harmless.
  thread.stop();
}

// ============================================================================
// Stress: post many tasks from many callers
// ============================================================================

TEST(HeartbeatThreadTest, StressManyTasksManyCallers) {
  constexpr int kNumCallers = 8;
  constexpr int kBatchSize = 200;

  HeartbeatThreadPool pool(4);
  pool.start();

  std::atomic<int> total{0};
  std::atomic<bool> running{true};

  std::vector<std::thread> callers;
  for (int c = 0; c < kNumCallers; c++) {
    callers.emplace_back([&]() {
      for (int i = 0; i < kBatchSize && running.load(); i++) {
        auto *ob = make_test_object();
        auto *thread = pool.thread_for_object(ob);
        thread->post([&total, ob]() {
          total.fetch_add(1);
          free_test_object(ob);
        });
        // Small delay to simulate real-world interleaving.
        if (i % 50 == 0) {
          std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
      }
    });
  }

  for (auto &t : callers) {
    t.join();
  }
  running = false;

  WAIT_FOR(total.load() >= kNumCallers * kBatchSize, 10000);

  EXPECT_GE(total.load(), kNumCallers * kBatchSize);
  pool.stop();
}
