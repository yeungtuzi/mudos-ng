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
  if (wakeup_fds_[1] != -1) { char c=1; while(write(wakeup_fds_[1],&c,1)<0&&errno==EINTR){} }
  if (thread_.joinable()) thread_.join();
  if (wakeup_event_) { event_del(wakeup_event_); event_free(wakeup_event_); wakeup_event_=nullptr; }
  if (wakeup_fds_[0]!=-1) { evutil_closesocket(wakeup_fds_[0]); wakeup_fds_[0]=-1; }
  if (wakeup_fds_[1]!=-1) { evutil_closesocket(wakeup_fds_[1]); wakeup_fds_[1]=-1; }
  if (base_) { event_base_free(base_); base_=nullptr; }
}

void HeartbeatThread::post(std::function<void()> task) {
  static std::atomic<int> pc{0};
  bool has_fd = (wakeup_fds_[1]!=-1);
  { std::lock_guard<std::mutex> lk(mutex_); tasks_.push_back(std::move(task)); }
  if (has_fd) { char c=1; while(write(wakeup_fds_[1],&c,1)<0&&errno==EINTR){} }
  else if (++pc <= 5) fprintf(stderr,"HB-T%d post DROPPED (fd=-1)\n",id_);
}

void HeartbeatThread::event_loop() { init_vm_state(); while(running_&&base_) event_base_loop(base_,EVLOOP_ONCE); cleanup_vm_state(); }

void HeartbeatThread::process_pending_tasks() {
  char buf[64];
  while(1){auto n=read(wakeup_fds_[0],buf,sizeof(buf)); if(n>0)continue; if(n<0&&errno==EINTR)continue; break;}
  std::deque<std::function<void()>> tasks;
  { std::lock_guard<std::mutex> lk(mutex_); tasks.swap(tasks_); }
  static std::atomic<int> pc{0}; int n=pc.fetch_add(1)+1;
  if(n<=5)fprintf(stderr,"HB-T%d process_pending #%d: %zu tasks\n",id_,n,tasks.size());
  for(auto& t:tasks) t();
}

void HeartbeatThread::wakeup_cb(evutil_socket_t,short,void*arg){reinterpret_cast<HeartbeatThread*>(arg)->process_pending_tasks();}

void HeartbeatThread::init_vm_state(){eval_timer_=per_thread_timer_create();init_thread_eval();reset_machine(1);}
void HeartbeatThread::cleanup_vm_state(){cleanup_thread_eval();if(eval_timer_){per_thread_timer_delete(eval_timer_);eval_timer_=nullptr;}}

// Queue operations — called on this thread via post().
void HeartbeatThread::add_heartbeat(object_t*ob,int interval){heartbeats_next_.push_back({ob,(short)interval,(short)interval});}
void HeartbeatThread::remove_heartbeat(object_t*ob){for(auto&hb:heartbeats_)if(hb.ob==ob)hb.ob=nullptr;for(auto&hb:heartbeats_next_)if(hb.ob==ob)hb.ob=nullptr;}
void HeartbeatThread::modify_heartbeat(object_t*ob,int interval){
  for(auto&hb:heartbeats_)if(hb.ob==ob){hb.time_to_heart_beat=hb.heart_beat_ticks=(short)interval;return;}
  for(auto&hb:heartbeats_next_)if(hb.ob==ob){hb.time_to_heart_beat=hb.heart_beat_ticks=(short)interval;return;}
  add_heartbeat(ob,interval);
  static std::atomic<int> mc{0};
  if (++mc <= 30) fprintf(stderr,"HB-T%d modify #%d: added ob=%p interval=%d\n",id_,mc.load(),(void*)ob,interval);
}

void HeartbeatThread::process_heartbeats() {
  // Merge new entries into active queue.
  heartbeats_.insert(heartbeats_.end(),heartbeats_next_.begin(),heartbeats_next_.end());
  heartbeats_next_.clear();
  static int call_count = 0;
  if (++call_count <= 5 || call_count % 100 == 0)
    fprintf(stderr, "HB-T%d: process_heartbeats #%d gate=%d queue=%zu\n",
            id_, call_count, g_heartbeat_gate.load(), heartbeats_.size());

  while(!heartbeats_.empty()){
    auto&hb=heartbeats_.front();
    if(hb.ob==nullptr||!(hb.ob->flags&O_HEART_BEAT)||(hb.ob->flags&O_DESTRUCTED)){heartbeats_.pop_front();continue;}
    heartbeats_next_.push_back(hb); heartbeats_.pop_front();
    auto*ch=&heartbeats_next_.back();
    if(--ch->heart_beat_ticks>0)continue;
    ch->heart_beat_ticks=ch->time_to_heart_beat;
    auto*ob=ch->ob;
    if(ob->prog->heart_beat==0)continue;

    // Gate check: skip LPC when gate is closed (creation phase).
    if (!g_heartbeat_gate.load(std::memory_order_relaxed)) continue;

    g_current_heartbeat_thread=this;
    g_current_heartbeat_obj=ob;
    object_t*ng=ob;
#ifndef NO_SHADOWS
    while(ng->shadowing)ng=ng->shadowing;
#endif
#ifndef NO_ADD_ACTION
    if(!(ng->flags&O_ENABLE_COMMANDS))ng=nullptr;
#endif
#ifdef PACKAGE_MUDLIB_STATS
    add_heart_beats(&ob->stats,1);
#endif
    save_command_giver(ng);
    current_interactive=nullptr; if(ob->interactive)current_interactive=ob;
    error_context_t econ; save_context(&econ);
    try{set_eval(max_eval_cost);call_direct(ob,ob->prog->heart_beat-1,0,0);pop_stack();}
    catch(const char*){restore_context(&econ);}
    pop_context(&econ);
    restore_command_giver(); current_interactive=nullptr;
    g_current_heartbeat_obj=nullptr; g_current_heartbeat_thread=nullptr; ch=nullptr;
  }
}

// Pool
HeartbeatThreadPool::HeartbeatThreadPool(size_t n){for(size_t i=0;i<n;i++)threads_.push_back(std::make_unique<HeartbeatThread>((int)i));}
HeartbeatThreadPool::~HeartbeatThreadPool(){stop();}
void HeartbeatThreadPool::start(){for(auto&t:threads_)t->start();}
void HeartbeatThreadPool::stop(){for(auto&t:threads_)t->stop();threads_.clear();}
HeartbeatThread*HeartbeatThreadPool::thread_for_object(object_t*ob){return threads_[(reinterpret_cast<uintptr_t>(ob)*2654435761ULL)%threads_.size()].get();}
HeartbeatThreadPool*g_heartbeat_thread_pool=nullptr;
thread_local HeartbeatThread*g_current_heartbeat_thread=nullptr;

void bounce_heartbeat_to_main_thread(object_t*ob){
  if(!g_event_base||!ob)return;
  event_base_once(g_event_base,-1,EV_TIMEOUT,[](evutil_socket_t,short,void*arg){
    auto*o=(object_t*)arg;
    if(!(o->flags&O_DESTRUCTED)&&o->prog->heart_beat!=0){error_context_t e;save_context(&e);try{set_eval(max_eval_cost);call_direct(o,o->prog->heart_beat-1,0,0);pop_stack();}catch(const char*){restore_context(&e);}pop_context(&e);}
  },ob,nullptr);
}
