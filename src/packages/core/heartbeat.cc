/*
 * heartbeat.cc
 *
 * Heartbeat queue management stays on the main thread.  Only the LPC
 * execution (call_direct) is offloaded to the heartbeat thread pool.
 */

#include "base/package_api.h"

#include "packages/core/heartbeat.h"

#include "base/internal/heartbeat_thread.h"

#include <algorithm>
#include <deque>
#include <set>
#include <vector>

struct heart_beat_t {
  object_t *ob;
  short heart_beat_ticks;
  short time_to_heart_beat;
};

thread_local object_t *g_current_heartbeat_obj;

// Heartbeat queues owned by the main thread (same as original design).
static std::deque<heart_beat_t> heartbeats, heartbeats_next;

void call_heart_beat() {
  add_gametick_event(
      time_to_next_gametick(std::chrono::milliseconds(CONFIG_INT(__RC_HEARTBEAT_INTERVAL_MSEC__))),
      TickEvent::callback_type(call_heart_beat));

  heartbeats.insert(heartbeats.end(), heartbeats_next.begin(), heartbeats_next.end());
  heartbeats_next.clear();

  auto *pool = g_heartbeat_thread_pool;

  while (!heartbeats.empty()) {
    auto &hb = heartbeats.front();

    if (hb.ob == nullptr || !(hb.ob->flags & O_HEART_BEAT) || (hb.ob->flags & O_DESTRUCTED)) {
      heartbeats.pop_front();
      continue;
    }

    heartbeats_next.push_back(hb);
    heartbeats.pop_front();
    auto *curr_hb = &heartbeats_next.back();

    if (--curr_hb->heart_beat_ticks > 0) {
      continue;
    }
    curr_hb->heart_beat_ticks = curr_hb->time_to_heart_beat;

    auto *ob = curr_hb->ob;
    if (ob->prog->heart_beat == 0) {
      continue;
    }

    if (pool) {
      // Offload LPC execution to heartbeat thread pool.
      auto *thread = pool->thread_for_object(ob);
      thread->post([thread, ob]() { thread->execute_heartbeat(ob); });
    } else {
      // Fallback: execute inline on main thread.
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

      g_current_heartbeat_obj = ob;
      error_context_t econ;
      save_context(&econ);
      try {
        set_eval(max_eval_cost);
        call_direct(ob, ob->prog->heart_beat - 1, ORIGIN_DRIVER, 0);
        pop_stack();
      } catch (const char *) {
        restore_context(&econ);
      }
      pop_context(&econ);

      restore_command_giver();
      current_interactive = nullptr;
      g_current_heartbeat_obj = nullptr;
    }
    curr_hb = nullptr;
  }
}

int query_heart_beat(object_t *ob) {
  if (!(ob->flags & O_HEART_BEAT)) return 0;
  for (auto &hb : heartbeats) if (hb.ob == ob) return hb.time_to_heart_beat;
  for (auto &hb : heartbeats_next) if (hb.ob == ob) return hb.time_to_heart_beat;
  return 0;
}

int set_heart_beat(object_t *ob, int to) {
  if (ob->flags & O_DESTRUCTED) return 0;
  if (to < 0) to = 1;

  if (to == 0) {
    ob->flags &= ~O_HEART_BEAT;
    bool found = false;
    for (auto &hb : heartbeats) if (hb.ob == ob) { hb.ob = nullptr; found = true; }
    for (auto &hb : heartbeats_next) if (hb.ob == ob) { hb.ob = nullptr; found = true; }
    return found ? 1 : 0;
  }
  ob->flags |= O_HEART_BEAT;

  for (auto &hb : heartbeats) {
    if (hb.ob == ob) { hb.time_to_heart_beat = hb.heart_beat_ticks = to; return 1; }
  }
  for (auto &hb : heartbeats_next) {
    if (hb.ob == ob) { hb.time_to_heart_beat = hb.heart_beat_ticks = to; return 1; }
  }
  heartbeats_next.push_back({ob, static_cast<short>(to), static_cast<short>(to)});
  return 1;
}

int heart_beat_status(outbuffer_t *buf, int verbose) {
  if (verbose == 1) {
    outbuf_add(buf, "Heart beat information:\n");
    outbuf_add(buf, "-----------------------\n");
    outbuf_addv(buf, "Number of objects with heart beat: %zu.\n",
                heartbeats.size() + heartbeats_next.size());
  }
  return 0;
}

#ifdef F_HEART_BEATS
array_t *get_heart_beats() {
  std::vector<object_t *> result;
  result.reserve(heartbeats.size() + heartbeats_next.size());
  bool display_hidden = true;
#ifdef F_SET_HIDE
  display_hidden = valid_hide(current_object);
#endif
  for (auto &q : {&heartbeats, &heartbeats_next}) {
    for (auto &hb : *q) {
      if (hb.ob && !(hb.ob->flags & O_HIDDEN)) result.push_back(hb.ob);
      else if (hb.ob && display_hidden) result.push_back(hb.ob);
    }
  }
  array_t *arr = allocate_empty_array(result.size());
  for (size_t i = 0; i < result.size(); i++) {
    arr->item[i].type = T_OBJECT;
    arr->item[i].u.ob = result[i];
    add_ref(arr->item[i].u.ob, "get_heart_beats");
  }
  return arr;
}
#endif

void check_heartbeats() {}
void clear_heartbeats() { heartbeats.clear(); heartbeats_next.clear(); }
