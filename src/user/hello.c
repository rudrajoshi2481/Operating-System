typedef unsigned long size_t;
typedef unsigned long uint64_t;

void sys_write(int fd, const void *buf, size_t len);
void sys_exit(int code);
void sys_yield(void);

static size_t slen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

void _start(void)
{
    sys_write(1, "hello from EL0\n", 15);
    sys_exit(0);
    for (;;)
        ;
    (void)slen;
    (void)sys_yield;
}
