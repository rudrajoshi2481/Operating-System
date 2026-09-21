/*
 * fb.c — Limine framebuffer + tiny rasterizer (Step 14).
 *
 * 32bpp linear buffer; drawing primitives: filled rects, Bresenham
 * lines, and a 3x5 pixel font scaled 2x (6x10 effective). The font
 * covers hex digits, uppercase letters, and the punctuation the URI
 * namespace uses.
 */
#include "fb.h"
#include "kprint.h"
#include "limine.h"
#include "lib.h"

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request fb_req = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0,
};

static volatile uint32_t *fb;
static uint32_t fb_w, fb_h, fb_pitch;   /* pitch in pixels */
static int ready;

int fb_init(uint64_t hhdm)
{
    (void)hhdm;
    if (!fb_req.response || !fb_req.response->framebuffer_count)
        return -1;
    struct limine_framebuffer *f = fb_req.response->framebuffers[0];
    if (f->bpp != 32)
        return -2;
    fb = (volatile uint32_t *)f->address;   /* limine gives an HHDM VA */
    fb_w = (uint32_t)f->width;
    fb_h = (uint32_t)f->height;
    fb_pitch = (uint32_t)(f->pitch / 4);
    ready = 1;
    fb_clear(0x101418);
    kprint("fb: %ux%u\n", fb_w, fb_h);
    return 0;
}

int fb_ok(void) { return ready; }

static void px(int x, int y, uint32_t rgb)
{
    if (x >= 0 && y >= 0 && (uint32_t)x < fb_w && (uint32_t)y < fb_h)
        fb[(uint32_t)y * fb_pitch + (uint32_t)x] = rgb;
}

void fb_clear(uint32_t rgb)
{
    if (!ready)
        return;
    for (uint32_t y = 0; y < fb_h; y++)
        for (uint32_t x = 0; x < fb_w; x++)
            fb[y * fb_pitch + x] = rgb;
}

void fb_rect(int x, int y, int w, int h, uint32_t rgb)
{
    if (!ready)
        return;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            px(x + i, y + j, rgb);
}

void fb_line(int x0, int y0, int x1, int y1, uint32_t rgb)
{
    if (!ready)
        return;
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        px(x0, y0, rgb);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = err * 2;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

/* 3x5 font: low 3 bits per row, bit i = column i from left */
struct glyph { char c; uint8_t r[5]; };
static const struct glyph font[] = {
    {'0', {7,5,5,5,7}}, {'1', {2,6,2,2,7}}, {'2', {7,1,7,4,7}},
    {'3', {7,1,7,1,7}}, {'4', {5,5,7,1,1}}, {'5', {7,4,7,1,7}},
    {'6', {7,4,7,5,7}}, {'7', {7,1,1,2,2}}, {'8', {7,5,7,5,7}},
    {'9', {7,5,7,1,7}},
    {'A', {2,5,7,5,5}}, {'B', {6,5,6,5,6}}, {'C', {3,4,4,4,3}},
    {'D', {6,5,5,5,6}}, {'E', {7,4,6,4,7}}, {'F', {7,4,6,4,4}},
    {'G', {3,4,5,5,3}}, {'H', {5,5,7,5,5}}, {'I', {7,2,2,2,7}},
    {'J', {1,1,1,5,2}}, {'K', {5,6,4,6,5}}, {'L', {4,4,4,4,7}},
    {'M', {5,7,7,5,5}}, {'N', {6,5,5,5,5}}, {'O', {2,5,5,5,2}},
    {'P', {6,5,6,4,4}}, {'Q', {2,5,5,2,1}}, {'R', {6,5,6,6,5}},
    {'S', {3,4,2,1,6}}, {'T', {7,2,2,2,2}}, {'U', {5,5,5,5,7}},
    {'V', {5,5,5,5,2}}, {'W', {5,5,7,7,5}}, {'X', {5,5,2,5,5}},
    {'Y', {5,5,2,2,2}}, {'Z', {7,1,2,4,7}},
    {':', {0,2,0,2,0}}, {'-', {0,0,7,0,0}}, {'.', {0,0,0,0,2}},
    {'/', {1,1,2,4,4}}, {'=', {0,7,0,7,0}}, {'#', {5,7,5,7,5}},
    {'>', {4,2,1,2,4}}, {'<', {1,2,4,2,1}}, {' ', {0,0,0,0,0}},
};

static const uint8_t *glyph_for(char c)
{
    if (c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');
    for (uint32_t i = 0; i < sizeof(font) / sizeof(font[0]); i++)
        if (font[i].c == c)
            return font[i].r;
    return 0;
}

void fb_char(int x, int y, char c, uint32_t rgb)
{
    if (!ready)
        return;
    const uint8_t *g = glyph_for(c);
    if (!g)
        return;
    for (int r = 0; r < 5; r++)
        for (int b = 0; b < 3; b++)
            if (g[r] & (1 << b))
                fb_rect(x + b * 2, y + r * 2, 2, 2, rgb);
}

void fb_text(int x, int y, const char *s, uint32_t rgb)
{
    for (; *s; s++, x += 8)
        fb_char(x, y, *s, rgb);
}
