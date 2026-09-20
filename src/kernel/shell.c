#include "shell.h"
#include "kprint.h"
#include "lib.h"
#include "pmm.h"
#include "proc.h"
#include "thread.h"
#include "uart.h"

extern char _user_hello_start[];

static int eq(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

static void run_cmd(char *cmd)
{
    if (eq(cmd, "help")) {
        kprint("commands: help ps free run\n");
    } else if (eq(cmd, "ps")) {
        sched_dump();
    } else if (eq(cmd, "free")) {
        kprint("free: %lu frames (%lu MiB)\n", pmm_free_frames(),
               pmm_free_frames() * 4096 / (1024 * 1024));
    } else if (eq(cmd, "run")) {
        proc_exec(_user_hello_start);
    } else if (cmd[0]) {
        kprint("unknown: %s\n", cmd);
    }
}

void shell_main(void *arg)
{
    (void)arg;
    char line[64];
    int n = 0;

    kprint("\nbioos> ");
    for (;;) {
        int c = uart_getc();
        if (c < 0) {
            yield();
            continue;
        }
        if (c == '\r' || c == '\n') {
            uart_putc('\n');
            line[n] = 0;
            run_cmd(line);
            n = 0;
            kprint("bioos> ");
        } else if ((c == 0x7f || c == '\b') && n > 0) {
            n--;
            uart_write("\b \b");
        } else if (n < (int)sizeof(line) - 1 && c >= 32 && c < 127) {
            line[n++] = (char)c;
            uart_putc((char)c);
        }
    }
}
