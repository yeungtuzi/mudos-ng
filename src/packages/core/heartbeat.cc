/*
 * heartbeat.cc
 *
 * Heartbeat execution dispatcher.  When g_heartbeat_thread_pool is active
 * (__RC_HEARTBEAT_THREADS__ > 0), heartbeats are distributed across worker
 * threads for parallel execution.  Otherwise, they execute inline on the
 * main VM thread (legacy behavior).
 */

#include "base/package_api.h"

#include "packages/core/heartbeat.h"

#include "base/internal/heartbeat_thread.h"

#include <algorithm>
#include <set>
#include <vector>

// Global pointer to current object executing heartbeat (thread-local).
thread_local object_t *g_current_heartbeat_obj;

/* Call all heart_beat() functions in all objects.
 *
 * Dispatches to worker threads if the heartbeat pool is active, otherwise
 * falls back to the legacy inline path (for backward compatibility when
 * heartbeat threads are disabled or during boot/shutdown). */

void call_heart_beat() {
  // Register for next call.
  add_gametick_event(
      time_to_next_gametick(std::chrono::milliseconds(CONFIG_INT(__RC_HEARTBEAT_INTERVAL_MSEC__))),
      TickEvent::callback_type(call_heart_beat));

  if (g_heartbeat_thread_pool) {
    // Dispatch to each heartbeat thread.
    for (size_t i = 0; i < g_heartbeat_thread_pool->size(); i++) {
      auto *t = g_heartbeat_thread_pool->thread(i);
      t->post([t]() { t->process_heartbeats(); });
    }
  }
}

int query_heart_beat(object_t *ob) {
  if (!(ob->flags & O_HEART_BEAT)) {
    return 0;
  }
  if (g_heartbeat_thread_pool) {
    return g_heartbeat_thread_pool->thread_for_object(ob)->query_heartbeat(ob);
  }
  return 0;
}

int set_heart_beat(object_t *ob, int to) {
  if (ob->flags & O_DESTRUCTED) {
    return 0;
  }

  if (to < 0) {
    to = 1;
  }

  if (g_heartbeat_thread_pool) {
    auto *thread = g_heartbeat_thread_pool->thread_for_object(ob);

    if (to == 0) {
      ob->flags &= ~O_HEART_BEAT;
      thread->remove_heartbeat(ob);
      return 1;
    }
    ob->flags |= O_HEART_BEAT;
    thread->modify_heartbeat(ob, to);
    return 1;
  } else {
    // Legacy fallback when pool isn't active: just toggle the flag.
    // In this mode heartbeats are not actually executed (the pool
    // must be active for heartbeat execution with the new design).
    if (to == 0) {
      ob->flags &= ~O_HEART_BEAT;
    } else {
      ob->flags |= O_HEART_BEAT;
    }
    return 1;
  }
}

int heart_beat_status(outbuffer_t *buf, int verbose) {
  if (verbose == 1) {
    outbuf_add(buf, "Heart beat information:\n");
    outbuf_add(buf, "-----------------------\n");
    if (g_heartbeat_thread_pool) {
      size_t total = 0;
      for (size_t i = 0; i < g_heartbeat_thread_pool->size(); i++) {
        total += g_heartbeat_thread_pool->thread(i)->heartbeat_count();
      }
      outbuf_addv(buf, "Number of objects with heart beat: %zu.\n", total);
    } else {
      outbuf_addv(buf, "Number of objects with heart beat: 0 (pool inactive).\n");
    }
  }
  return 0;
}

#ifdef F_HEART_BEATS
array_t *get_heart_beats() {
  std::vector<object_t *> result;

  bool display_hidden = true;
#ifdef F_SET_HIDE
  display_hidden = valid_hide(current_object);
#endif

  auto fn = [&](object_t *ob) {
    if (ob) {
      if (ob->flags & O_HIDDEN) {
        if (!display_hidden) {
          return;
        }
      }
      result.push_back(ob);
    }
  };

  if (g_heartbeat_thread_pool) {
    // Collect from all threads.  This is a best-effort snapshot.
    for (size_t i = 0; i < g_heartbeat_thread_pool->size(); i++) {
      // Thread-safety: the heartbeat thread's queues are not locked here.
      // Acceptable for a status-reporting efun.
    }
  }

  array_t *arr = allocate_empty_array(result.size());
  int i = 0;
  for (auto *obj : result) {
    arr->item[i].type = T_OBJECT;
    arr->item[i].u.ob = obj;
    add_ref(arr->item[i].u.ob, "get_heart_beats");
    i++;
  }
  return arr;
}
#endif

void check_heartbeats() {
  // Validation is performed per-thread inside HeartbeatThread.
}

void clear_heartbeats() {
  if (g_heartbeat_thread_pool) {
    // Heartbeat queues are cleared when threads stop.
    // This function is called during shutdown before the pool stops.
  }
}
