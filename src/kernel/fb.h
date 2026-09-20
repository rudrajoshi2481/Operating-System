#pragma once

#include <stdint.h>

/* fb.c — linear framebuffer + tiny rasterizer (Step 14). */
int  fb_init(uint64_t hhdm);
int  fb_ok(void);
void fb_clear(uint32_t rgb);
void fb_rect(int x, int y, int w, int h, uint32_t rgb);
void fb_line(int x0, int y0, int x1, int y1, uint32_t rgb);
void fb_char(int x, int y, char c, uint32_t rgb);
void fb_text(int x, int y, const char *s, uint32_t rgb);
