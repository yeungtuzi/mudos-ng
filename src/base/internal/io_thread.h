/*
 * Copyright (c) 2026 [大河马/dahema@me.com]
 * SPDX-License-Identifier: MIT
 */

#ifndef IO_THREAD_H
#define IO_THREAD_H

#include <atomic>
#include <deque>
#include <event2/util.h>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

struct event_base;
struct event;

class IOThread {
 public:
  explicit IOThread(int id);
  ~IOThread();

  IOThread(const IOThread &) = delete;
  IOThread &operator=(const IOThread &) = delete;
  IOThread(IOThread &&) = delete;
  IOThread &operator=(IOThread &&) = delete;

  void start();
  void stop();

  // Post a task to be executed on this IO thread's event loop.
  // Thread-safe, non-blocking.
  void post(std::function<void()> task);

  event_base *base() const { return base_; }
  int id() const { return id_; }

  // Returns true if the calling thread is this IO thread.
  bool is_current_thread() const { return std::this_thread::get_id() == thread_id_; }

 private:
  void event_loop();
  void process_pending_tasks();

  static void wakeup_cb(evutil_socket_t fd, short what, void *arg);

  int id_;
  std::thread::id thread_id_;
  event_base *base_{nullptr};
  evutil_socket_t wakeup_fds_[2] = {-1, -1};
  event *wakeup_event_{nullptr};
  std::thread thread_;
  std::atomic<bool> running_{false};

  std::mutex mutex_;
  std::deque<std::function<void()>> tasks_;
};

class IOThreadPool {
 public:
  explicit IOThreadPool(size_t num_threads);
  ~IOThreadPool();

  IOThreadPool(const IOThreadPool &) = delete;
  IOThreadPool &operator=(const IOThreadPool &) = delete;

  void start();
  void stop();

  // Round-robin selection of next IO thread.
  IOThread *next_thread();
  size_t size() const { return threads_.size(); }

 private:
  std::vector<std::unique_ptr<IOThread>> threads_;
  std::atomic<size_t> next_{0};
};

// Global IO thread pool pointer, initialized in mainlib
extern IOThreadPool *g_io_thread_pool;

#endif  // IO_THREAD_H
