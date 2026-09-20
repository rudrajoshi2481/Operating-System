#pragma once

typedef struct {
    int v;
} spinlock_t;

static inline void spin_lock(spinlock_t *l)
{
    while (__atomic_exchange_n(&l->v, 1, __ATOMIC_ACQUIRE)) {
#ifdef __aarch64__
        __asm__ volatile("yield");
#endif
    }
}

static inline void spin_unlock(spinlock_t *l)
{
    __atomic_store_n(&l->v, 0, __ATOMIC_RELEASE);
}
