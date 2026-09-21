#include "thread.h"
#include "assert.h"
#include "heap.h"
#include "irq.h"
#include "kprint.h"
#include "lib.h"
#include "spinlock.h"

#define NTHREADS    32
#define STACK_SIZE  (16 * 1024)

enum { T_UNUSED, T_RUNNABLE, T_RUNNING, T_SLEEPING, T_ZOMBIE };

struct thread {
    int       state;
    int       id;
    void     *chan;          /* sleep_on channel; == &sleepers for ksleep */
    uint64_t  wake_tick;     /* deadline for ksleep sleepers            */
    uint64_t *stack;
    uint64_t  ksp;           /* saved SP pointing at the switch frame   */
    uint64_t  pgd;           /* TTBR0 phys; 0 = kernel thread           */
    uint64_t  uentry;        /* EL0 entry point (user threads)          */
    uint64_t  usp;           /* EL0 stack pointer                       */
};

extern void swtch(uint64_t *save_sp, uint64_t load_sp);
extern void thread_trampoline(void);
#ifdef __aarch64__
extern void user_trampoline(void);
extern void enter_el0(uint64_t entry, uint64_t usp, uint64_t pgd,
                      uint64_t kstack_top);
#endif

static struct thread  threads[NTHREADS];
static struct thread *cur;
static spinlock_t     sched_lock;
static int            sleepers;   /* address doubles as ksleep channel */

/* Called with sched_lock held. Leaves prev->state as the caller set it. */
static void schedule(void)
{
    struct thread *prev = cur;
    int start = (int)(prev - threads);

    struct thread *next = 0;
    for (int i = 1; i <= NTHREADS; i++) {
        struct thread *t = &threads[(start + i) % NTHREADS];
        if (t->state == T_RUNNABLE) {
            next = t;
            break;
        }
    }
    if (!next || next == prev)
        return;
    next->state = T_RUNNING;
    cur = next;
#ifdef __aarch64__
    if (next->pgd && next->pgd != prev->pgd)
        __asm__ volatile(
            "msr ttbr0_el1, %0\n"
            "dsb ishst\n"
            "tlbi vmalle1\n"
            "dsb sy\n"
            "isb\n" :: "r"(next->pgd));
#endif
    /* x86_64: no per-process CR3 yet — all threads are kernel threads */
    swtch(&prev->ksp, next->ksp);
}

void sched_init(void)
{
    threads[0].state = T_RUNNING;
    threads[0].id = 0;
    cur = &threads[0];
}

/* thread_trampoline lands here: drop the lock the scheduler handed us. */
void sched_thread_entry(void)
{
    spin_unlock(&sched_lock);
    irq_unmask();
}

int thread_create(thread_fn fn, void *arg)
{
    irq_mask();
    spin_lock(&sched_lock);

    struct thread *t = 0;
    static int next_id = 1;
    for (int i = 0; i < NTHREADS; i++) {
        if (threads[i].state == T_ZOMBIE) {
            if (threads[i].stack)
                kfree(threads[i].stack);
            threads[i].stack = 0;
            threads[i].state = T_UNUSED;
        }
        if (threads[i].state == T_UNUSED && !t)
            t = &threads[i];
    }
    if (!t) {
        spin_unlock(&sched_lock);
        irq_unmask();
        return -1;
    }
    t->state = T_RUNNABLE;
    t->id = next_id++;
    spin_unlock(&sched_lock);
    irq_unmask();

    t->stack = kmalloc(STACK_SIZE);
    assert(t->stack);

    uint64_t *sp = (uint64_t *)(((uintptr_t)t->stack + STACK_SIZE) & ~15ULL);
#ifdef __aarch64__
    sp -= 12;
    memset(sp, 0, 96);
    sp[0]  = (uint64_t)fn;                 /* x19 */
    sp[1]  = (uint64_t)arg;                /* x20 */
    sp[10] = 0;                            /* x29 */
    sp[11] = (uint64_t)thread_trampoline;  /* x30 */
#else
    /* x86_64 frame (low->high): r15 r14 r13 r12 rbx rbp ret */
    sp -= 7;
    memset(sp, 0, 56);
    sp[0] = (uint64_t)fn;                  /* r15 */
    sp[1] = (uint64_t)arg;                 /* r14 */
    sp[6] = (uint64_t)thread_trampoline;   /* ret addr */
#endif
    t->ksp = (uint64_t)sp;

    return t->id;
}

#ifdef __aarch64__
int thread_spawn_user(uint64_t entry, uint64_t usp, uint64_t pgd)
{
    irq_mask();
    spin_lock(&sched_lock);

    struct thread *t = 0;
    static int next_uid = 100;
    for (int i = 0; i < NTHREADS; i++) {
        if (threads[i].state == T_ZOMBIE) {
            if (threads[i].stack)
                kfree(threads[i].stack);
            threads[i].stack = 0;
            threads[i].pgd = 0;
            threads[i].state = T_UNUSED;
        }
        if (threads[i].state == T_UNUSED && !t)
            t = &threads[i];
    }
    if (!t) {
        spin_unlock(&sched_lock);
        irq_unmask();
        return -1;
    }
    t->state = T_RUNNABLE;
    t->id = next_uid++;
    t->pgd = pgd;
    t->uentry = entry;
    t->usp = usp;
    spin_unlock(&sched_lock);
    irq_unmask();

    t->stack = kmalloc(STACK_SIZE);
    assert(t->stack);

    uint64_t *sp = (uint64_t *)(((uintptr_t)t->stack + STACK_SIZE) & ~15ULL);
    sp -= 12;
    memset(sp, 0, 96);
    sp[11] = (uint64_t)user_trampoline;    /* x30 */
    t->ksp = (uint64_t)sp;

    return t->id;
}

/* user_trampoline lands here holding sched_lock with IRQs masked. */
void user_thread_enter(void)
{
    struct thread *t = cur;
    spin_unlock(&sched_lock);
    enter_el0(t->uentry, t->usp, t->pgd,
              (uint64_t)t->stack + STACK_SIZE);
}

uint64_t cur_pgd(void)
{
    return cur->pgd;
}
#endif /* __aarch64__ (user threads) */

int sched_fmt(char *buf, uint32_t cap)
{
    static const char *names[] = {
        "unused", "runnable", "running", "sleeping", "zombie"
    };
    uint32_t off = 0;
    irq_mask();
    for (int i = 0; i < NTHREADS; i++)
        if (threads[i].state != T_UNUSED && off < cap)
            off += (uint32_t)ksnprintf(buf + off, cap - off,
                                       "  thread %d: %s%s\n", threads[i].id,
                                       names[threads[i].state],
                                       threads[i].pgd ? " (user)" : "");
    irq_unmask();
    return (int)off;
}

void sched_dump(void)
{
    static char b[512];
    sched_fmt(b, sizeof(b));
    kprint("%s", b);
}

void yield(void)
{
    irq_mask();
    spin_lock(&sched_lock);
    cur->state = T_RUNNABLE;
    schedule();
    spin_unlock(&sched_lock);
    irq_unmask();
}

void sleep_on(void *chan)
{
    irq_mask();
    spin_lock(&sched_lock);
    cur->chan = chan;
    cur->state = T_SLEEPING;
    schedule();
    spin_unlock(&sched_lock);
    irq_unmask();
}

void wakeup(void *chan)
{
    irq_mask();
    spin_lock(&sched_lock);
    for (int i = 0; i < NTHREADS; i++)
        if (threads[i].state == T_SLEEPING && threads[i].chan == chan) {
            threads[i].state = T_RUNNABLE;
            threads[i].chan = 0;
        }
    spin_unlock(&sched_lock);
    irq_unmask();
}

void ksleep(uint64_t nticks)
{
    irq_mask();
    spin_lock(&sched_lock);
    cur->chan = &sleepers;
    cur->wake_tick = timer_ticks() + nticks;
    cur->state = T_SLEEPING;
    schedule();
    spin_unlock(&sched_lock);
    irq_unmask();
}

void thread_exit(void)
{
    irq_mask();
    spin_lock(&sched_lock);
    cur->state = T_ZOMBIE;
    schedule();
    kpanic("zombie thread resumed");
}

/* Called from the timer IRQ handler with IRQs masked. */
void sched_tick(void)
{
    if (!cur)                   /* tick before sched_init (early sti) */
        return;
    uint64_t now = timer_ticks();

    spin_lock(&sched_lock);
    for (int i = 0; i < NTHREADS; i++)
        if (threads[i].state == T_SLEEPING &&
            threads[i].chan == &sleepers &&
            threads[i].wake_tick <= now) {
            threads[i].state = T_RUNNABLE;
            threads[i].chan = 0;
        }
    if (cur->state == T_RUNNING)
        cur->state = T_RUNNABLE;
    schedule();
    spin_unlock(&sched_lock);
}
