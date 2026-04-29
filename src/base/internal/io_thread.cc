/*
 * Copyright (c) 2026 [大河马/dahema@me.com]
 * SPDX-License-Identifier: MIT
 */

#include "base/std.h"

#include "io_thread.h"

#include <event2/event.h>
#include <event2/thread.h>
#include <event2/util.h>
#include <unistd.h>
#include <cstring>

IOThread::IOThread(int id) : id_(id) {}

IOThread::~IOThread() { stop(); }

void IOThread::start() {
  if (running_) {
    return;
  }

  // Create the event base for this IO thread
  base_ = event_base_new();
  if (!base_) {
    fatal("IOThread[%d]: failed to create event_base", id_);
  }

  // Ensure libevent threading is initialized
  // (already done in init_backend, but be safe)
  int rv = evthread_make_base_notifiable(base_);
  if (rv != 0) {
    fatal("IOThread[%d]: evthread_make_base_notifiable failed", id_);
  }

  // Create a socket pair for cross-thread wakeup.
  // We use socketpair instead of eventfd for cross-platform compatibility
  // (macOS does not have eventfd).
  if (evutil_socketpair(AF_UNIX, SOCK_STREAM, 0, wakeup_fds_) == -1) {
    fatal("IOThread[%d]: failed to create wakeup socketpair", id_);
  }

  // Make both ends non-blocking
  evutil_make_socket_nonblocking(wakeup_fds_[0]);
  evutil_make_socket_nonblocking(wakeup_fds_[1]);
  evutil_make_socket_closeonexec(wakeup_fds_[0]);
  evutil_make_socket_closeonexec(wakeup_fds_[1]);

  // Register the wakeup fd as a persistent read event
  wakeup_event_ = event_new(base_, wakeup_fds_[0], EV_READ | EV_PERSIST,
                            IOThread::wakeup_cb, this);
  if (!wakeup_event_) {
    fatal("IOThread[%d]: failed to create wakeup event", id_);
  }

  struct timeval no_timeout = {0, 0};
  if (event_add(wakeup_event_, nullptr) != 0) {
    fatal("IOThread[%d]: failed to add wakeup event", id_);
  }

  running_ = true;
  thread_ = std::thread([this]() { this->event_loop(); });
  thread_id_ = thread_.get_id();
}

void IOThread::stop() {
  if (!running_.exchange(false)) {
    return;
  }

  // Wake the IO thread so event_loop's while(running_) check takes effect.
  // The wakeup socket write guarantees kqueue (macOS) or epoll (Linux)
  // returns, event_base_loop(EVLOOP_ONCE) unwinds, and the while condition
  // sees running_ == false.
  if (wakeup_fds_[1] != -1) {
    char c = 1;
    while (write(wakeup_fds_[1], &c, 1) < 0 && errno == EINTR) {
    }
  }

  if (thread_.joinable()) {
    thread_.join();
  }

  // Cleanup
  if (wakeup_event_) {
    event_del(wakeup_event_);
    event_free(wakeup_event_);
    wakeup_event_ = nullptr;
  }
  if (wakeup_fds_[0] != -1) {
    evutil_closesocket(wakeup_fds_[0]);
    wakeup_fds_[0] = -1;
  }
  if (wakeup_fds_[1] != -1) {
    evutil_closesocket(wakeup_fds_[1]);
    wakeup_fds_[1] = -1;
  }
  if (base_) {
    event_base_free(base_);
    base_ = nullptr;
  }
}

void IOThread::post(std::function<void()> task) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    tasks_.push_back(std::move(task));
  }

  // Wake up the IO thread's event loop so it processes the task
  if (wakeup_fds_[1] != -1) {
    char c = 1;
    while (write(wakeup_fds_[1], &c, 1) < 0 && errno == EINTR) {
    }
  }
}

void IOThread::event_loop() {
  // Process events one batch at a time, checking running_ in between.
  // This is more robust than event_base_loopbreak across platforms
  // (macOS kqueue can be unreliable about waking from loopbreak).
  while (running_ && base_) {
    event_base_loop(base_, EVLOOP_ONCE);
  }
}

void IOThread::process_pending_tasks() {
  // Drain the wakeup socket (read all available bytes)
  char buf[64];
  while (true) {
    auto n = read(wakeup_fds_[0], buf, sizeof(buf));
    if (n > 0) {
      continue;
    }
    if (n < 0 && (errno == EINTR)) {
      continue;
    }
    break;
  }

  // Swap and execute all pending tasks
  std::deque<std::function<void()>> tasks;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    tasks.swap(tasks_);
  }

  for (auto &task : tasks) {
    task();
  }
}

void IOThread::wakeup_cb(evutil_socket_t /*fd*/, short /*what*/, void *arg) {
  auto *self = reinterpret_cast<IOThread *>(arg);
  self->process_pending_tasks();
}

// --- IOThreadPool ---

IOThreadPool::IOThreadPool(size_t num_threads) {
  for (size_t i = 0; i < num_threads; i++) {
    threads_.push_back(std::make_unique<IOThread>(static_cast<int>(i)));
  }
}

IOThreadPool::~IOThreadPool() { stop(); }

void IOThreadPool::start() {
  for (auto &t : threads_) {
    t->start();
  }
}

void IOThreadPool::stop() {
  for (auto &t : threads_) {
    t->stop();
  }
  threads_.clear();
}

IOThread *IOThreadPool::next_thread() {
  if (threads_.empty()) {
    return nullptr;
  }
  auto idx = next_.fetch_add(1) % threads_.size();
  #if defined(__has_feature)
  #if __has_feature(thread_sanitizer)
  // suppress tsan warning for benign race on modulus
  #endif
  #endif
  return threads_[idx].get();
}

IOThreadPool *g_io_thread_pool = nullptr;
