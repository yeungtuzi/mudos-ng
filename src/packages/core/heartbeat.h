/*
 * heartbeat.h
 *
 *  Created on: Nov 5, 2014
 *      Author: sunyc
 */

#ifndef HEARTBEAT_H_
#define HEARTBEAT_H_

#include <atomic>
// FIXME: remove this usage
extern thread_local struct object_t *g_current_heartbeat_obj;
// Heartbeat execution gate: false = skip LPC, queue management only.
extern std::atomic<bool> g_heartbeat_gate;

int set_heart_beat(struct object_t *, int);
int query_heart_beat(struct object_t *);
int heart_beat_status(struct outbuffer_t *, int);
struct array_t *get_heart_beats();

// Used by backend.cc
void call_heart_beat();

// Used by md.cc for verifying.
void check_heartbeats();
// Shutdown hook
void clear_heartbeats();

#endif /* HEARTBEAT_H_ */
