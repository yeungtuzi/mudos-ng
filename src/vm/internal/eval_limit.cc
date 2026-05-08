#include "base/std.h"

#include "eval_limit.h"
#include "posix_timers.h"

thread_local volatile int outoftime = 0;
uint64_t max_eval_cost;

// Per-thread timer pointer.  Created by heartbeat worker threads
// (or other worker threads that run LPC).  nullptr on the main thread
// which uses the per-process timer instead.
static thread_local PerThreadTimer *g_per_thread_timer = nullptr;

void init_eval() {
#ifndef _WIN32
  init_posix_timers();
#else
  debug_message("WARNING: Platform doesn't support eval limit!\n");
#endif
}

// Called by worker threads to set up their own eval timer.
void init_thread_eval() {
  g_per_thread_timer = per_thread_timer_create();
}

// Called by worker threads at shutdown.
void cleanup_thread_eval() {
  if (g_per_thread_timer) {
    per_thread_timer_delete(g_per_thread_timer);
    g_per_thread_timer = nullptr;
  }
}

void set_eval(uint64_t etime) {
  outoftime = 0;
#ifndef _WIN32
  if (g_per_thread_timer) {
    per_thread_timer_set(g_per_thread_timer, etime);
  } else {
    posix_eval_timer_set(etime);
  }
#endif
}

int64_t get_eval() {
#ifndef _WIN32
  if (g_per_thread_timer) {
    return per_thread_timer_get(g_per_thread_timer);
  }
  return posix_eval_timer_get();
#else
  return max_eval_cost;
#endif
}
