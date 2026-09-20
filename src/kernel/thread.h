#pragma once

#include <stdint.h>

typedef void (*thread_fn)(void *);

void     sched_init(void);
int      thread_create(thread_fn fn, void *arg);
int      thread_spawn_user(uint64_t entry, uint64_t usp, uint64_t pgd);
void     yield(void);
void     sleep_on(void *chan);
void     wakeup(void *chan);
void     ksleep(uint64_t nticks);
void     thread_exit(void);
void     sched_tick(void);
void     sched_dump(void);
uint64_t cur_pgd(void);
uint64_t timer_ticks(void);
