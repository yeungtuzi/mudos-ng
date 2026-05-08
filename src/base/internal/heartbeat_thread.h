/*
 * Copyright (c) 2026 [大河马/dahema@me.com]
 * SPDX-License-Identifier: MIT
 */

#ifndef HEARTBEAT_THREAD_H
#define HEARTBEAT_THREAD_H

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
struct object_t;

struct heart_beat_t {
  object_t *ob;
  short heart_beat_ticks;
  short time_to_heart_beat;
};

class HeartbeatThread {
 public:
  explicit HeartbeatThread(int id);
  ~HeartbeatThread();

  void start();
  void stop();
  void post(std::function<void()> task);

  // Queue operations — called via post() from main thread.
  void add_heartbeat(object_t *ob, int interval);
  void remove_heartbeat(object_t *ob);
  void modify_heartbeat(object_t *ob, int interval);

  // Process one heartbeat cycle on this thread.
  void process_heartbeats();

  event_base *base() const { return base_; }
  int id() const { return id_; }
  bool is_current_thread() const { return std::this_thread::get_id() == thread_id_; }

 private:
  void event_loop();
  void process_pending_tasks();
  void init_vm_state();
  void cleanup_vm_state();
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

  // Per-thread heartbeat queues (only accessed from this thread's event loop).
  std::deque<heart_beat_t> heartbeats_;
  std::deque<heart_beat_t> heartbeats_next_;

  struct PerThreadTimer *eval_timer_{nullptr};
};

class HeartbeatThreadPool {
 public:
  explicit HeartbeatThreadPool(size_t num_threads);
  ~HeartbeatThreadPool();
  void start();
  void stop();
  HeartbeatThread *thread_for_object(object_t *ob);
  HeartbeatThread *thread(int idx) { return threads_[idx].get(); }
  size_t size() const { return threads_.size(); }
 private:
  std::vector<std::unique_ptr<HeartbeatThread>> threads_;
};

extern HeartbeatThreadPool *g_heartbeat_thread_pool;
extern thread_local class HeartbeatThread *g_current_heartbeat_thread;
void bounce_heartbeat_to_main_thread(object_t *ob);

#endif
