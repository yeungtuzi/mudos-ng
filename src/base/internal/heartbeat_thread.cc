/*
 * Copyright (c) 2026 [大河马/dahema@me.com]
 * SPDX-License-Identifier: MIT
 */

#include "base/std.h"

#include "heartbeat_thread.h"

#include <event2/event.h>
#include <event2/thread.h>
#include <event2/util.h>
#include <unistd.h>
#include <cstring>

#include "backend.h"
#include "packages/core/heartbeat.h"
#include "vm/internal/base/machine.h"
#include "vm/internal/base/interpret.h"
#include "vm/internal/eval_limit.h"
#include "vm/internal/posix_timers.h"

// ---------------------------------------------------------------------------
// HeartbeatThread
// ---------------------------------------------------------------------------

HeartbeatThread::HeartbeatThread(int id) : id_(id) {}

HeartbeatThread::~HeartbeatThread() { stop(); }

void HeartbeatThread::start() {
  if (running_) {
    return;
  }

  base_ = event_base_new();
  if (!base_) {
    fatal("HeartbeatThread[%d]: failed to create event_base", id_);
  }

  int rv = evthread_make_base_notifiable(base_);
  if (rv != 0) {
    fatal("HeartbeatThread[%d]: evthread_make_base_notifiable failed", id_);
  }

  if (evutil_socketpair(AF_UNIX, SOCK_STREAM, 0, wakeup_fds_) == -1) {
    fatal("HeartbeatThread[%d]: failed to create wakeup socketpair", id_);
  }
  evutil_make_socket_nonblocking(wakeup_fds_[0]);
  evutil_make_socket_nonblocking(wakeup_fds_[1]);
  evutil_make_socket_closeonexec(wakeup_fds_[0]);
  evutil_make_socket_closeonexec(wakeup_fds_[1]);

  wakeup_event_ = event_new(base_, wakeup_fds_[0], EV_READ | EV_PERSIST,
                            HeartbeatThread::wakeup_cb, this);
  if (!wakeup_event_) {
    fatal("HeartbeatThread[%d]: failed to create wakeup event", id_);
  }
  if (event_add(wakeup_event_, nullptr) != 0) {
    fatal("HeartbeatThread[%d]: failed to add wakeup event", id_);
  }

  running_ = true;
  thread_ = std::thread([this]() { this->event_loop(); });
  thread_id_ = thread_.get_id();
}

void HeartbeatThread::stop() {
  if (!running_.exchange(false)) {
    return;
  }

  if (wakeup_fds_[1] != -1) {
    char c = 1;
    while (write(wakeup_fds_[1], &c, 1) < 0 && errno == EINTR) {
    }
  }

  if (thread_.joinable()) {
    thread_.join();
  }

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

void HeartbeatThread::post(std::function<void()> task) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    tasks_.push_back(std::move(task));
  }
  if (wakeup_fds_[1] != -1) {
    char c = 1;
    while (write(wakeup_fds_[1], &c, 1) < 0 && errno == EINTR) {
    }
  }
}

void HeartbeatThread::event_loop() {
  init_vm_state();
  while (running_ && base_) {
    event_base_loop(base_, EVLOOP_ONCE);
  }
  cleanup_vm_state();
}

void HeartbeatThread::process_pending_tasks() {
  char buf[64];
  while (true) {
    auto n = read(wakeup_fds_[0], buf, sizeof(buf));
    if (n > 0) { continue; }
    if (n < 0 && (errno == EINTR)) { continue; }
    break;
  }

  std::deque<std::function<void()>> tasks;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    tasks.swap(tasks_);
  }

  for (auto &task : tasks) {
    task();
  }
}

void HeartbeatThread::wakeup_cb(evutil_socket_t /*fd*/, short /*what*/, void *arg) {
  auto *self = reinterpret_cast<HeartbeatThread *>(arg);
  self->process_pending_tasks();
}

// ---------------------------------------------------------------------------
// VM state lifecycle
// ---------------------------------------------------------------------------

void HeartbeatThread::init_vm_state() {
  // Create per-thread eval timer.
  eval_timer_ = per_thread_timer_create();
  init_thread_eval();

  // Initialize thread-local VM stacks.
  reset_machine(1);
}

void HeartbeatThread::cleanup_vm_state() {
  cleanup_thread_eval();
  if (eval_timer_) {
    per_thread_timer_delete(eval_timer_);
    eval_timer_ = nullptr;
  }
}

// ---------------------------------------------------------------------------
// Heartbeat queue operations (called on this thread's event loop)
// ---------------------------------------------------------------------------

void HeartbeatThread::add_heartbeat(object_t *ob, int interval) {
  heartbeats_next_.push_back({ob, static_cast<short>(interval), static_cast<short>(interval)});
}

void HeartbeatThread::remove_heartbeat(object_t *ob) {
  for (auto &hb : heartbeats_) {
    if (hb.ob == ob) { hb.ob = nullptr; }
  }
  for (auto &hb : heartbeats_next_) {
    if (hb.ob == ob) { hb.ob = nullptr; }
  }
}

void HeartbeatThread::modify_heartbeat(object_t *ob, int interval) {
  // Search existing entries.
  for (auto &hb : heartbeats_) {
    if (hb.ob == ob) {
      hb.time_to_heart_beat = static_cast<short>(interval);
      hb.heart_beat_ticks = static_cast<short>(interval);
      return;
    }
  }
  for (auto &hb : heartbeats_next_) {
    if (hb.ob == ob) {
      hb.time_to_heart_beat = static_cast<short>(interval);
      hb.heart_beat_ticks = static_cast<short>(interval);
      return;
    }
  }
  // Not found — add a new entry.
  add_heartbeat(ob, interval);
}

int HeartbeatThread::query_heartbeat(object_t *ob) {
  for (auto &hb : heartbeats_) {
    if (hb.ob == ob) { return hb.time_to_heart_beat; }
  }
  for (auto &hb : heartbeats_next_) {
    if (hb.ob == ob) { return hb.time_to_heart_beat; }
  }
  return 0;
}

void HeartbeatThread::process_heartbeats() {
  // Merge new heartbeats into the main queue.
  heartbeats_.insert(heartbeats_.end(), heartbeats_next_.begin(), heartbeats_next_.end());
  heartbeats_next_.clear();

  while (!heartbeats_.empty()) {
    auto &hb = heartbeats_.front();

    // Skip invalid entries.
    if (hb.ob == nullptr || !(hb.ob->flags & O_HEART_BEAT) || (hb.ob->flags & O_DESTRUCTED)) {
      heartbeats_.pop_front();
      continue;
    }

    // Move to next queue while we execute.
    heartbeats_next_.push_back(hb);
    heartbeats_.pop_front();
    auto *curr_hb = &heartbeats_next_.back();

    if (--curr_hb->heart_beat_ticks > 0) {
      continue;
    }
    curr_hb->heart_beat_ticks = curr_hb->time_to_heart_beat;

    auto *ob = curr_hb->ob;
    if (ob->prog->heart_beat == 0) {
      continue;
    }

    object_t *new_command_giver = ob;
#ifndef NO_SHADOWS
    while (new_command_giver->shadowing) {
      new_command_giver = new_command_giver->shadowing;
    }
#endif
#ifndef NO_ADD_ACTION
    if (!(new_command_giver->flags & O_ENABLE_COMMANDS)) {
      new_command_giver = nullptr;
    }
#endif
#ifdef PACKAGE_MUDLIB_STATS
    add_heart_beats(&ob->stats, 1);
#endif
    save_command_giver(new_command_giver);

    current_interactive = nullptr;
    if (ob->interactive) {
      current_interactive = ob;
    }

    g_current_heartbeat_obj = ob;
    g_current_heartbeat_thread = this;

    error_context_t econ;
    save_context(&econ);
    try {
      set_eval(max_eval_cost);
      call_direct(ob, ob->prog->heart_beat - 1, 0 /*origin=driver*/, 0);
      pop_stack();
    } catch (const char *) {
      restore_context(&econ);
    }
    pop_context(&econ);

    restore_command_giver();
    current_interactive = nullptr;
    g_current_heartbeat_obj = nullptr;
    g_current_heartbeat_thread = nullptr;
    curr_hb = nullptr;
  }
}

// ---------------------------------------------------------------------------
// HeartbeatThreadPool
// ---------------------------------------------------------------------------

HeartbeatThreadPool::HeartbeatThreadPool(size_t num_threads) {
  for (size_t i = 0; i < num_threads; i++) {
    threads_.push_back(std::make_unique<HeartbeatThread>(static_cast<int>(i)));
  }
}

HeartbeatThreadPool::~HeartbeatThreadPool() { stop(); }

void HeartbeatThreadPool::start() {
  for (auto &t : threads_) {
    t->start();
  }
}

void HeartbeatThreadPool::stop() {
  for (auto &t : threads_) {
    t->stop();
  }
  threads_.clear();
}

HeartbeatThread *HeartbeatThreadPool::thread_for_object(object_t *ob) {
  auto hash = reinterpret_cast<uintptr_t>(ob) >> 3;
  return threads_[hash % threads_.size()].get();
}

HeartbeatThreadPool *g_heartbeat_thread_pool = nullptr;
thread_local HeartbeatThread *g_current_heartbeat_thread = nullptr;

// Bounce a full heartbeat re-execution to the main event loop
// when a cross-thread call is detected.
void bounce_heartbeat_to_main_thread(object_t *ob) {
  if (!g_event_base || !ob) {
    return;
  }
  // Schedule the heartbeat to re-run on the main thread's next event cycle.
  // The object ref is safe because the heartbeat queue owns a reference
  // through the heart_beat_t entry.
  event_base_once(g_event_base, -1, EV_TIMEOUT,
    [](evutil_socket_t, short, void *arg) {
      auto *obj = reinterpret_cast<object_t *>(arg);
      if (!(obj->flags & O_DESTRUCTED) && obj->prog->heart_beat != 0) {
        error_context_t econ;
        save_context(&econ);
        try {
          set_eval(max_eval_cost);
          call_direct(obj, obj->prog->heart_beat - 1, 0, 0);
          pop_stack();
        } catch (const char *) {
          restore_context(&econ);
        }
        pop_context(&econ);
      }
    },
    ob, nullptr);
}
