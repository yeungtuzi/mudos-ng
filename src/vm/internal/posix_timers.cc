// Timer implementation for eval limit.
// Linux: POSIX per-process timers (timer_create/timer_settime) +
//        per-thread timers (SIGEV_THREAD_ID).
// macOS/other POSIX: setitimer/getitimer fallback +
//                    per-thread timers (dispatch_source).

#include "base/std.h"

#include "vm/internal/posix_timers.h"

#include <cstdio>   // for perror()
#include <cstdlib>  // for exit()
#include <sys/signal.h>

#include "vm/internal/eval_limit.h"

// ---------------------------------------------------------------------------
// Per-process timer (main VM thread)
// ---------------------------------------------------------------------------

#ifdef __linux__

#include <time.h>

static timer_t eval_timer_id;

/*
 * SIGVTALRM handler.
 */
static void sigalrm_handler(int sig, siginfo_t *si, void *uc) {
  if (!si->si_value.sival_ptr) {
    outoftime = 1;
  }
}

void init_posix_timers(void) {
  struct sigevent sev;
  struct sigaction sa;
  /* This mimics the behavior of setitimer in uvalarm.c */
  memset(&sev, 0, sizeof(sev));
  sev.sigev_signo = SIGVTALRM;
  sev.sigev_notify = SIGEV_SIGNAL;
  sev.sigev_value.sival_ptr = NULL;

  int i = -1;
  // Only CLOCK_REALTIME is standard.
#if defined(CLOCK_MONOTONIC_COARSE)
  i = timer_create(CLOCK_MONOTONIC_COARSE, &sev, &eval_timer_id);
#endif
#if defined(CLOCK_MONOTONIC)
  if (i < 0) {
    i = timer_create(CLOCK_MONOTONIC, &sev, &eval_timer_id);
  }
#endif
  if (i < 0) {
    i = timer_create(CLOCK_REALTIME, &sev, &eval_timer_id);
  }
  if (i < 0) {
    perror("init_posix_timers: timer_create");
    exit(-1);
  }

  sa.sa_sigaction = sigalrm_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_SIGINFO;

  i = sigaction(SIGVTALRM, &sa, NULL);
  if (i < 0) {
    perror("init_posix_timers: sigaction");
    exit(-1);
  }
}

void posix_eval_timer_set(uint64_t micros) {
  struct itimerspec it;

  it.it_interval.tv_sec = 0;
  it.it_interval.tv_nsec = 0;

  it.it_value.tv_sec = micros / 1000000;
  it.it_value.tv_nsec = micros % 1000000 * 1000;

  timer_settime(eval_timer_id, 0, &it, NULL);
}

uint64_t posix_eval_timer_get(void) {
  struct itimerspec it;

  if (timer_gettime(eval_timer_id, &it) < 0) {
    return 100;
  }

  return it.it_value.tv_sec * static_cast<uint64_t>(1000000) + it.it_value.tv_nsec / 1000;
}

#else  // !__linux__ : use setitimer / getitimer (macOS, BSD, etc.)

#include <signal.h>
#include <sys/time.h>

static void sigprof_handler(int) {
  outoftime = 1;
}

void init_posix_timers(void) {
  struct sigaction sa {};
  sa.sa_handler = sigprof_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;

  if (sigaction(SIGPROF, &sa, nullptr) < 0) {
    perror("init_posix_timers: sigaction");
    exit(-1);
  }
}

void posix_eval_timer_set(uint64_t micros) {
  struct itimerval it;

  it.it_interval.tv_sec = 0;
  it.it_interval.tv_usec = 0;

  it.it_value.tv_sec = micros / 1000000;
  it.it_value.tv_usec = micros % 1000000;

  setitimer(ITIMER_PROF, &it, NULL);
}

uint64_t posix_eval_timer_get(void) {
  struct itimerval it;

  if (getitimer(ITIMER_PROF, &it) < 0) {
    return 100;
  }

  return it.it_value.tv_sec * static_cast<uint64_t>(1000000) + it.it_value.tv_usec;
}

#endif  // __linux__

// ---------------------------------------------------------------------------
// Per-thread timer
// ---------------------------------------------------------------------------

#ifdef __linux__

#include <sys/syscall.h>
#include <unistd.h>

struct PerThreadTimer {
  timer_t timer_id;
};

PerThreadTimer *per_thread_timer_create(void) {
  auto *pt = new PerThreadTimer;
  struct sigevent sev;
  memset(&sev, 0, sizeof(sev));
  sev.sigev_signo = SIGVTALRM;
  sev.sigev_notify = SIGEV_THREAD_ID;
  sev._sigev_un._tid = static_cast<pid_t>(syscall(SYS_gettid));
  sev.sigev_value.sival_ptr = NULL;

  if (timer_create(CLOCK_MONOTONIC, &sev, &pt->timer_id) < 0) {
    delete pt;
    return nullptr;
  }
  return pt;
}

void per_thread_timer_set(PerThreadTimer *t, uint64_t micros) {
  struct itimerspec it;
  it.it_interval.tv_sec = 0;
  it.it_interval.tv_nsec = 0;
  it.it_value.tv_sec = micros / 1000000;
  it.it_value.tv_nsec = micros % 1000000 * 1000;
  timer_settime(t->timer_id, 0, &it, NULL);
}

uint64_t per_thread_timer_get(PerThreadTimer *t) {
  struct itimerspec it;
  if (timer_gettime(t->timer_id, &it) < 0) {
    return 100;
  }
  return it.it_value.tv_sec * static_cast<uint64_t>(1000000) + it.it_value.tv_nsec / 1000;
}

void per_thread_timer_delete(PerThreadTimer *t) {
  timer_delete(t->timer_id);
  delete t;
}

#elif defined(__APPLE__)

#include <dispatch/dispatch.h>

struct PerThreadTimer {
  dispatch_source_t source;
  dispatch_queue_t queue;
};

PerThreadTimer *per_thread_timer_create(void) {
  auto *pt = new PerThreadTimer;
  pt->queue = dispatch_queue_create("eval_timer", DISPATCH_QUEUE_SERIAL);
  pt->source = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, pt->queue);

  __block volatile int *ot = &outoftime;
  dispatch_source_set_event_handler(pt->source, ^{
    *ot = 1;
  });
  dispatch_resume(pt->source);
  return pt;
}

void per_thread_timer_set(PerThreadTimer *t, uint64_t micros) {
  dispatch_source_set_timer(t->source,
                            dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(micros) * 1000),
                            DISPATCH_TIME_FOREVER, 0);
}

uint64_t per_thread_timer_get(PerThreadTimer *t) {
  // dispatch_source doesn't provide a "time remaining" query.
  // Return a non-zero default; the eval loop checks outoftime separately.
  return 100;
}

void per_thread_timer_delete(PerThreadTimer *t) {
  dispatch_source_cancel(t->source);
  dispatch_release(t->source);
  dispatch_release(t->queue);
  delete t;
}

#else  // !__linux__ && !__APPLE__

// No per-thread timer support; fall back to per-process mechanism.

struct PerThreadTimer {};

PerThreadTimer *per_thread_timer_create(void) { return nullptr; }
void per_thread_timer_set(PerThreadTimer *, uint64_t) {}
uint64_t per_thread_timer_get(PerThreadTimer *) { return 100; }
void per_thread_timer_delete(PerThreadTimer *t) { delete t; }

#endif
