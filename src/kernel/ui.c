/*
 * ui.c — in-OS UI screens (Step 14). Rendered on demand to the Limine
 * framebuffer; serial remains the primary interface.
 */
#include "ui.h"
#include "fb.h"
#include "heap.h"
#include "kprint.h"
#include "lib.h"
#include "objstore.h"
#include "pmm.h"
#include "prov.h"
#include "sha256.h"
#include "thread.h"

static int cur = 1;

static uint32_t type_color(uint32_t t)
{
    static const uint32_t c[] = {
        0x606060,   /* 0 raw     gray    */
        0x40c0ff,   /* 1 text    blue    */
        0x40c0ff,   /* 2 json    blue    */
        0x40ff60,   /* 3 genome  green   */
        0xffc040,   /* 4 prov    amber   */
        0x808080,   /* 5 chunk   d.gray  */
        0x40ff60,   /* 6 array   green   */
        0x80ff80,   /* 7 embed   l.green */
        0xff60ff,   /* 8 variants magent */
        0xff8080,   /* 9 sample  salmon  */
        0x80ffff,   /* 10 plate  cyan    */
        0xffff80,   /* 11 proto  yellow  */
        0xff8040,   /* 12 instr  orange  */
        0xc080ff,   /* 13 exp    purple  */
        0xff4040,   /* 14 audit  red     */
    };
    return t < sizeof(c) / sizeof(c[0]) ? c[t] : 0x404040;
}

static void titlebar(const char *name)
{
    fb_rect(0, 0, 1024, 18, 0x203040);
    fb_text(8, 4, "BIOOS", 0x80e0ff);
    fb_text(56, 4, name, 0xffffff);
}

static void scr_shell(void)
{
    char buf[256];
    int n = ksnprintf(buf, sizeof(buf), "MEM %lu MIB FREE",
                      pmm_free_frames() * 4096 / (1024 * 1024));
    buf[n] = 0;
    fb_text(16, 40, buf, 0xc0c0c0);
    n = ksnprintf(buf, sizeof(buf), "OBJECTS %u  PROV %u",
                  obj_count(), prov_count());
    buf[n] = 0;
    fb_text(16, 56, buf, 0xc0c0c0);
    n = sched_fmt(buf + 0, sizeof(buf) - 1);
    buf[n < 0 ? 0 : n] = 0;
    int y = 80;
    for (char *p = buf; *p && y < 300; ) {
        char *e = p;
        while (*e && *e != '\n')
            e++;
        char save = *e;
        *e = 0;
        fb_text(16, y, p, 0xa0a0a0);
        y += 12;
        *e = save;
        p = e + (save ? 1 : 0);
    }
}

static void scr_objects(void)
{
    uint32_t n = obj_count();
    int cols = 24, cell = 20;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t h[SHA256_LEN];
        uint32_t len, type;
        if (obj_at(i, h, &len, &type) != 0)
            break;
        int x = 16 + (int)(i % cols) * (cell + 4);
        int y = 40 + (int)(i / cols) * (cell + 4);
        fb_rect(x, y, cell, cell, type_color(type));
        /* label with a hash nibble pair so cells are distinguishable */
        char lab[3];
        lab[0] = "0123456789abcdef"[h[0] >> 4];
        lab[1] = "0123456789abcdef"[h[0] & 0xf];
        lab[2] = 0;
        fb_text(x + 2, y + 6, lab, 0x101010);
    }
}

static void scr_lineage(void)
{
    if (!obj_count()) {
        fb_text(16, 40, "EMPTY STORE", 0x808080);
        return;
    }
    uint8_t h[SHA256_LEN];
    uint32_t len, type;
    obj_at(obj_count() - 1, h, &len, &type);
    char *buf = kmalloc(2048);
    if (!buf)
        return;
    int n = prov_lineage(h, buf, 2047);
    if (n <= 0) {
        fb_text(16, 40, "NO LINEAGE", 0x808080);
        kfree(buf);
        return;
    }
    buf[n] = 0;
    int y = 40;
    for (char *p = buf; *p && y < 500; ) {
        char *e = p;
        while (*e && *e != '\n')
            e++;
        char save = *e;
        *e = 0;
        int depth = 0;
        const char *t = p;
        while (t[0] == ' ' && t[1] == ' ') {
            depth++;
            t += 2;
        }
        int x = 16 + depth * 24;
        if (y > 40)
            fb_line(x - 12, y - 4, x - 2, y + 4, 0x607080);
        fb_rect(x - 4, y - 3, 2, 12, 0x40c0ff);
        fb_text(x, y, t, 0xe0e0e0);
        y += 14;
        *e = save;
        p = e + (save ? 1 : 0);
    }
    kfree(buf);
}

static void scr_heatmap(void)
{
    /* heatmap of the newest object's bytes: value -> blue..red ramp */
    if (!obj_count()) {
        fb_text(16, 40, "EMPTY STORE", 0x808080);
        return;
    }
    uint8_t h[SHA256_LEN];
    uint32_t len, type;
    obj_at(obj_count() - 1, h, &len, &type);
    uint8_t *data = kmalloc(len ? len : 1);
    if (!data)
        return;
    int n = obj_get(h, data, len);
    if (n <= 0) {
        kfree(data);
        return;
    }
    int cell = 8, cols = 32;
    for (int i = 0; i < n; i++) {
        uint8_t v = data[i];
        uint32_t rgb = ((uint32_t)v << 16) |       /* r = heat */
                       (0x40 - (v >> 3)) |          /* g falls */
                       (0xff - v);                  /* b inverse */
        int x = 16 + (i % cols) * (cell + 1);
        int y = 40 + (i / cols) * (cell + 1);
        fb_rect(x, y, cell, cell, rgb);
    }
    kfree(data);
    char lab[80];
    char hex[SHA256_LEN * 2 + 1];
    sha256_hex(h, hex);
    hex[16] = 0;
    int m = ksnprintf(lab, sizeof(lab), "OBJ %s TYPE %u LEN %u",
                      hex, type, len);
    lab[m] = 0;
    fb_text(16, 30, lab, 0x808080);
}

void ui_render(int screen)
{
    if (!fb_ok())
        return;
    if (screen >= 0 && screen <= 3)
        cur = screen;
    fb_clear(0x101418);
    static const char *names[] = {
        "SHELL", "OBJECTS", "LINEAGE", "HEATMAP"
    };
    titlebar(names[cur]);
    switch (cur) {
    case 0: scr_shell();    break;
    case 1: scr_objects();  break;
    case 2: scr_lineage();  break;
    case 3: scr_heatmap();  break;
    }
}

int ui_screen(void)
{
    return cur;
}
