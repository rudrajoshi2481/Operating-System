/*
 * hostlink.c — guest end of the host channel (virtio-console).
 * Reads newline-terminated lines from the host, logs them on serial,
 * and replies "ok <line>\n". This is the seed of the agent/UI pipe:
 * later steps will route typed commands and objects over it.
 */
#include "hostlink.h"
#include "kprint.h"
#include "virtio_console.h"

void hostlink_main(void *arg)
{
    (void)arg;
    char line[96];
    int n = 0;

    for (;;) {
        con_wait();
        char c;
        while (con_read(&c, 1) == 1) {
            if (c == '\n' || c == '\r') {
                if (n) {
                    line[n] = 0;
                    kprint("[host] %s\n", line);
                    con_write("ok ", 3);
                    con_write(line, (uint32_t)n);
                    con_write("\n", 1);
                    n = 0;
                }
            } else if (n < (int)sizeof(line) - 1) {
                line[n++] = c;
            }
        }
    }
}
