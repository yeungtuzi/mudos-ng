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

HeartbeatThread::HeartbeatThread(int id) : id_(id) {}
HeartbeatThread::~HeartbeatThread() { stop(); }

void HeartbeatThread::start() {
  if (running_) return;
  base_ = event_base_new();
  if (!base_) fatal("HeartbeatThread[%d]: failed to create event_base", id_);
  if (evthread_make_base_notifiable(base_) != 0)
    fatal("HeartbeatThread[%d]: evthread_make_base_notifiable failed", id_);
  if (evutil_socketpair(AF_UNIX, SOCK_STREAM, 0, wakeup_fds_) == -1)
    fatal("HeartbeatThread[%d]: failed to create wakeup socketpair", id_);
  evutil_make_socket_nonblocking(wakeup_fds_[0]);
  evutil_make_socket_nonblocking(wakeup_fds_[1]);
  evutil_make_socket_closeonexec(wakeup_fds_[0]);
  evutil_make_socket_closeonexec(wakeup_fds_[1]);
  wakeup_event_ = event_new(base_, wakeup_fds_[0], EV_READ | EV_PERSIST,
                            HeartbeatThread::wakeup_cb, this);
  if (!wakeup_event_) fatal("HeartbeatThread[%d]: failed to create wakeup event", id_);
  if (event_add(wakeup_event_, nullptr) != 0)
    fatal("HeartbeatThread[%d]: failed to add wakeup event", id_);
  running_ = true;
  thread_ = std::thread([this]() { this->event_loop(); });
  thread_id_ = thread_.get_id();
}

void HeartbeatThread::stop() {
  if (!running_.exchange(false)) return;
  if (wakeup_fds_[1] != -1) {
    char c = 1;
    while (write(wakeup_fds_[1], &c, 1) < 0 && errno == EINTR) {}
  }
  if (thread_.joinable()) thread_.join();
  if (wakeup_event_) { event_del(wakeup_event_); event_free(wakeup_event_); wakeup_event_ = nullptr; }
  if (wakeup_fds_[0] != -1) { evutil_closesocket(wakeup_fds_[0]); wakeup_fds_[0] = -1; }
  if (wakeup_fds_[1] != -1) { evutil_closesocket(wakeup_fds_[1]); wakeup_fds_[1] = -1; }
  if (base_) { event_base_free(base_); base_ = nullptr; }
}

void HeartbeatThread::post(std::function<void()> task) {
  { std::lock_guard<std::mutex> lock(mutex_); tasks_.push_back(std::move(task)); }
  if (wakeup_fds_[1] != -1) {
    char c = 1;
    while (write(wakeup_fds_[1], &c, 1) < 0 && errno == EINTR) {}
  }
}

void HeartbeatThread::event_loop() {
  init_vm_state();
  while (running_ && base_) event_base_loop(base_, EVLOOP_ONCE);
  cleanup_vm_state();
}

void HeartbeatThread::process_pending_tasks() {
  char buf[64];
  while (true) {
    auto n = read(wakeup_fds_[0], buf, sizeof(buf));
    if (n > 0) continue;
    if (n < 0 && (errno == EINTR)) continue;
    break;
  }
  std::deque<std::function<void()>> tasks;
  { std::lock_guard<std::mutex> lock(mutex_); tasks.swap(tasks_); }
  for (auto &task : tasks) task();
}

void HeartbeatThread::wakeup_cb(evutil_socket_t, short, void *arg) {
  reinterpret_cast<HeartbeatThread *>(arg)->process_pending_tasks();
}

void HeartbeatThread::init_vm_state() {
  eval_timer_ = per_thread_timer_create();
  init_thread_eval();
  reset_machine(1);
}

void HeartbeatThread::cleanup_vm_state() {
  cleanup_thread_eval();
  if (eval_timer_) { per_thread_timer_delete(eval_timer_); eval_timer_ = nullptr; }
}

void HeartbeatThread::execute_heartbeat(object_t *ob) {
  if (!ob || (ob->flags & O_DESTRUCTED) || ob->prog->heart_beat == 0) return;

  g_current_heartbeat_thread = this;
  g_current_heartbeat_obj = ob;

  object_t *new_command_giver = ob;
#ifndef NO_SHADOWS
  while (new_command_giver->shadowing) new_command_giver = new_command_giver->shadowing;
#endif
#ifndef NO_ADD_ACTION
  if (!(new_command_giver->flags & O_ENABLE_COMMANDS)) new_command_giver = nullptr;
#endif
#ifdef PACKAGE_MUDLIB_STATS
  add_heart_beats(&ob->stats, 1);
#endif
  save_command_giver(new_command_giver);
  current_interactive = nullptr;
  if (ob->interactive) current_interactive = ob;

  error_context_t econ;
  save_context(&econ);
  try {
    set_eval(max_eval_cost);
    call_direct(ob, ob->prog->heart_beat - 1, 0, 0);
    pop_stack();
  } catch (const char *) {
    restore_context(&econ);
  }
  pop_context(&econ);

  restore_command_giver();
  current_interactive = nullptr;
  g_current_heartbeat_obj = nullptr;
  g_current_heartbeat_thread = nullptr;
}

// Pool
HeartbeatThreadPool::HeartbeatThreadPool(size_t num_threads) {
  for (size_t i = 0; i < num_threads; i++)
    threads_.push_back(std::make_unique<HeartbeatThread>(static_cast<int>(i)));
}
HeartbeatThreadPool::~HeartbeatThreadPool() { stop(); }
void HeartbeatThreadPool::start() { for (auto &t : threads_) t->start(); }
void HeartbeatThreadPool::stop() { for (auto &t : threads_) t->stop(); threads_.clear(); }
HeartbeatThread *HeartbeatThreadPool::thread_for_object(object_t *ob) {
  return threads_[(reinterpret_cast<uintptr_t>(ob) * 2654435761ULL) % threads_.size()].get();
}

HeartbeatThreadPool *g_heartbeat_thread_pool = nullptr;
thread_local HeartbeatThread *g_current_heartbeat_thread = nullptr;

void bounce_heartbeat_to_main_thread(object_t *ob) {
  if (!g_event_base || !ob) return;
  event_base_once(g_event_base, -1, EV_TIMEOUT,
    [](evutil_socket_t, short, void *arg) {
      auto *obj = reinterpret_cast<object_t *>(arg);
      if (!(obj->flags & O_DESTRUCTED) && obj->prog->heart_beat != 0) {
        error_context_t econ;
        save_context(&econ);
        try { set_eval(max_eval_cost); call_direct(obj, obj->prog->heart_beat - 1, 0, 0); pop_stack(); }
        catch (const char *) { restore_context(&econ); }
        pop_context(&econ);
      }
    }, ob, nullptr);
}
