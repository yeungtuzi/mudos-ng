#ifndef POSIX_TIMERS_H
#define POSIX_TIMERS_H

#include <cstdint>

// Per-process timer (used by the main VM thread).
void init_posix_timers(void);
void posix_eval_timer_set(uint64_t micros);
uint64_t posix_eval_timer_get(void);

// Per-thread timer (for heartbeat worker threads).
// Returns nullptr on unsupported platforms.
struct PerThreadTimer;
PerThreadTimer *per_thread_timer_create(void);
void per_thread_timer_set(PerThreadTimer *t, uint64_t micros);
uint64_t per_thread_timer_get(PerThreadTimer *t);
void per_thread_timer_delete(PerThreadTimer *t);

#endif
