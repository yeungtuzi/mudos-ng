/*
 * heartbeat.cc
 *
 * Per-thread heartbeat queues. Main thread's call_heart_beat() just
 * signals each worker thread to process its own queue.  set_heart_beat()
 * posts queue modifications to the owning thread.
 */

#include "base/package_api.h"
#include "packages/core/heartbeat.h"
#include "base/internal/heartbeat_thread.h"
#include <set>
#include <vector>

thread_local object_t *g_current_heartbeat_obj;

// Global heartbeat gate. When false, heartbeats skip LPC execution
// but still count ticks (so timing is correct when enabled).
std::atomic<bool> g_heartbeat_gate{false};  // closed until explicitly opened

void call_heart_beat() {
  add_gametick_event(
      time_to_next_gametick(std::chrono::milliseconds(CONFIG_INT(__RC_HEARTBEAT_INTERVAL_MSEC__))),
      TickEvent::callback_type(call_heart_beat));

  if (g_heartbeat_thread_pool) {
    for (size_t i = 0; i < g_heartbeat_thread_pool->size(); i++) {
      auto *t = g_heartbeat_thread_pool->thread(i);
      t->post([t]() { t->process_heartbeats(); });
    }
  }
}

int query_heart_beat(object_t *ob) {
  // Best-effort: check O_HEART_BEAT flag. The actual interval is on the worker thread.
  if (!(ob->flags & O_HEART_BEAT)) return 0;
  return 1;  // flag is set → heartbeat is active
}

int set_heart_beat(object_t *ob, int to) {
  if (ob->flags & O_DESTRUCTED) return 0;
  if (to < 0) to = 1;

  if (to == 0) {
    ob->flags &= ~O_HEART_BEAT;
    if (g_heartbeat_thread_pool) {
      auto *t = g_heartbeat_thread_pool->thread_for_object(ob);
      t->post([t, ob]() { t->remove_heartbeat(ob); });
    }
    return 1;
  }
  ob->flags |= O_HEART_BEAT;
  if (g_heartbeat_thread_pool) {
    auto *t = g_heartbeat_thread_pool->thread_for_object(ob);
    int interval = to;
    t->post([t, ob, interval]() { t->modify_heartbeat(ob, interval); });
    return 1;
  }
  return 1;
}

int heart_beat_status(outbuffer_t *buf, int verbose) {
  if (verbose == 1) {
    outbuf_add(buf, "Heart beat information:\n");
    outbuf_add(buf, "-----------------------\n");
    outbuf_addv(buf, "Gate: %s\n", g_heartbeat_gate.load() ? "OPEN" : "CLOSED");
  }
  return 0;
}

#ifdef F_HEART_BEATS
array_t *get_heart_beats() {
  std::vector<object_t *> result;
  // Collection not implemented for per-thread queues.
  array_t *arr = allocate_empty_array(0);
  return arr;
}
#endif

void check_heartbeats() {}
void clear_heartbeats() {}
