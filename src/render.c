#include "binmap.h"
#include "font.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static inline uint32_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return (0xFFu << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static inline uint8_t clamp_u8(int v)
{
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

static void fill(uint32_t *pixels, int w, int h, uint32_t color)
{
    uint64_t n = (uint64_t)w * (uint64_t)h;
    for (uint64_t i = 0; i < n; i++) pixels[i] = color;
}

static void draw_line_pix(uint32_t *pixels, int w, int h,
                          float x0, float y0, float x1, float y1, uint32_t color);

/* ---------- shared palettes ---------- */

static uint32_t byte_class_color(uint8_t b)
{
    if (b == 0x00) return rgb(20, 20, 30);
    if (b == 0xFF) return rgb(240, 240, 245);
    if (b == ' ' || b == '\t' || b == '\n' || b == '\r' || b == '\f' || b == '\v')
        return rgb(60, 200, 110);
    if (b < 0x20) return rgb(220, 80, 80);
    if (b < 0x80) return rgb(70, 150, 230);
    return rgb(230, 180, 60);
}

/* dark-blue → cyan → yellow → white, t in 0..1 */
static uint32_t heat_color(double t)
{
    if (t < 0) t = 0; else if (t > 1) t = 1;
    int r, g, b;
    if (t < 0.33) {
        double k = t / 0.33;
        r = 0;
        g = (int)(80 * k);
        b = (int)(80 + 175 * k);
    } else if (t < 0.66) {
        double k = (t - 0.33) / 0.33;
        r = (int)(255 * k);
        g = (int)(80 + 175 * k);
        b = (int)(255 - 100 * k);
    } else {
        double k = (t - 0.66) / 0.34;
        r = 255;
        g = 255;
        b = (int)(155 + 100 * k);
    }
    return rgb(clamp_u8(r), clamp_u8(g), clamp_u8(b));
}

/* ---------- label / overlay helpers ---------- */

#define OVERLAY_LABEL_COLOR rgb(220, 220, 235)
#define OVERLAY_TICK_COLOR  rgb(140, 140, 165)
#define OVERLAY_DIM_COLOR   rgb(150, 150, 170)
#define OVERLAY_GRID_COLOR  rgb(50, 50, 70)
#define OVERLAY_TITLE_COLOR rgb(255, 200, 100)

static void format_hex_offset(char *buf, size_t bufsz, size_t off, size_t file_size)
{
    int digits;
    if      (file_size <= 0xFFFFul)         digits = 4;
    else if (file_size <= 0xFFFFFFul)       digits = 6;
    else if (file_size <= 0xFFFFFFFFul)     digits = 8;
    else                                    digits = 12;
    snprintf(buf, bufsz, "0X%0*zX", digits, off);
}

static void draw_hline(uint32_t *pixels, int w, int h, int x0, int x1, int y, uint32_t color)
{
    if (y < 0 || y >= h) return;
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (x0 < 0) x0 = 0;
    if (x1 >= w) x1 = w - 1;
    uint32_t *row = pixels + (size_t)y * (size_t)w;
    for (int x = x0; x <= x1; x++) row[x] = color;
}

static void draw_vline(uint32_t *pixels, int w, int h, int x, int y0, int y1, uint32_t color)
{
    if (x < 0 || x >= w) return;
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    if (y0 < 0) y0 = 0;
    if (y1 >= h) y1 = h - 1;
    for (int y = y0; y <= y1; y++) pixels[(size_t)y * (size_t)w + (size_t)x] = color;
}

/* Right-edge offset rail used by row-major views (byte-class, entropy,
 * strings density, RGB-raw). Draws 5 ticks (0/25/50/75/100%) labeled with
 * absolute file offsets. y_top..y_bot is the vertical span the data covers. */
static void draw_offset_rail_right(uint32_t *pixels, int w, int h,
                                   int x_edge, int y_top, int y_bot,
                                   size_t base, size_t span, size_t file_size)
{
    if (y_bot <= y_top) return;
    int tick_len = 6;
    int label_pad = 3;
    int scale = 1;
    int label_h = FONT_H * scale;
    for (int q = 0; q <= 4; q++) {
        size_t off = base + (size_t)((double)q * 0.25 * (double)span);
        if (q == 4) off = base + span;
        int y = y_top + (q * (y_bot - y_top)) / 4;
        if (y < 0 || y >= h) continue;
        draw_hline(pixels, w, h, x_edge - tick_len, x_edge - 1, y, OVERLAY_TICK_COLOR);
        char buf[32];
        format_hex_offset(buf, sizeof(buf), off, file_size);
        int tw = text_width(scale, buf);
        int tx = x_edge - tick_len - label_pad - tw;
        if (tx < 2) tx = 2;
        int ty = y - label_h / 2;
        if (ty < 0) ty = 0;
        if (ty + label_h >= h) ty = h - label_h - 1;
        /* faint shadow background so text reads over any palette. Skip
         * pixels not yet drawn to (alpha=0): in DECOR_ONLY mode the buffer
         * is otherwise transparent and a shadow would blot black boxes
         * behind the label in the overlay composite. */
        for (int by = ty - 1; by < ty + label_h + 1; by++) {
            for (int bx = tx - 1; bx < tx + tw + 1; bx++) {
                if (bx < 0 || bx >= w || by < 0 || by >= h) continue;
                uint32_t *p = &pixels[by * w + bx];
                uint32_t c = *p;
                if (!(c & 0xFF000000u)) continue;
                uint8_t r = (uint8_t)((c >> 16) & 0xFF);
                uint8_t g = (uint8_t)((c >> 8)  & 0xFF);
                uint8_t b = (uint8_t)( c        & 0xFF);
                r = (uint8_t)(r / 3);
                g = (uint8_t)(g / 3);
                b = (uint8_t)(b / 3);
                *p = rgb(r, g, b);
            }
        }
        blit_text(pixels, w, h, tx, ty, scale, OVERLAY_LABEL_COLOR, buf);
    }
}

/* ---------- Byte-class stripe ---------- */

void render_byte_class(uint32_t *pixels, int w, int h, const uint8_t *data, size_t size,
                       size_t base_offset, size_t file_size, render_mode_t mode)
{
    const uint32_t bg = rgb(15, 15, 20);
    uint64_t total = (uint64_t)w * (uint64_t)h;
    if (size == 0 || total == 0) {
        if (mode == RENDER_FULL) fill(pixels, w, h, bg);
        return;
    }
    if (mode != RENDER_DECOR_ONLY) {
        for (uint64_t i = 0; i < total; i++) {
            uint64_t bi = (i * size) / total;
            if (bi >= size) {
                if (mode == RENDER_FULL) pixels[i] = bg;
                continue;
            }
            pixels[i] = byte_class_color(data[bi]);
        }
    }
    /* offset rail is meaningless on the 1-pixel-tall minimap strip */
    if (mode != RENDER_DATA_ONLY && h >= 32) {
        draw_offset_rail_right(pixels, w, h, w - 2, 2, h - 2,
                               base_offset, size, file_size);
    }
}

/* ---------- Hilbert curve ---------- */

static void d2xy_hilbert(int n, int d, int *x, int *y)
{
    int rx, ry, t = d;
    *x = 0; *y = 0;
    for (int s = 1; s < n; s *= 2) {
        rx = 1 & (t / 2);
        ry = 1 & (t ^ rx);
        if (ry == 0) {
            if (rx == 1) { *x = s - 1 - *x; *y = s - 1 - *y; }
            int tmp = *x; *x = *y; *y = tmp;
        }
        *x += s * rx;
        *y += s * ry;
        t /= 4;
    }
}

static uint32_t curve_color(uint8_t b)
{
    if (b == 0)    return rgb(8, 8, 12);
    if (b == 0xFF) return rgb(255, 255, 255);
    if (b < 0x20) {
        uint8_t v = (uint8_t)(b * 7);
        return rgb(200, v / 3, v / 4);
    }
    if (b < 0x80) {
        uint8_t v = (uint8_t)((b - 0x20) * 2);
        return rgb(v / 3, v / 2 + 40, 160 + v / 3);
    }
    uint8_t v = (uint8_t)((b - 0x80) * 2);
    return rgb(180 + v / 4, 140 + v / 4, 40 + v / 4);
}

static void draw_corner_label(uint32_t *pixels, int w, int h,
                              int cx, int cy, int img_x0, int img_y0, int img_size,
                              size_t off, size_t file_size)
{
    char buf[32];
    format_hex_offset(buf, sizeof(buf), off, file_size);
    int scale = 1;
    int tw = text_width(scale, buf);
    int th = FONT_H * scale;
    int x, y;
    /* place label just inside the corner, opposite to its (cx,cy) extremes */
    if (cx == 0) x = img_x0 + 2;
    else         x = img_x0 + img_size - tw - 2;
    if (cy == 0) y = img_y0 + 2;
    else         y = img_y0 + img_size - th - 2;
    /* shadow (skip transparent pixels — DECOR_ONLY has none to darken) */
    for (int by = y - 1; by < y + th + 1; by++) {
        for (int bx = x - 1; bx < x + tw + 1; bx++) {
            if (bx < 0 || bx >= w || by < 0 || by >= h) continue;
            uint32_t *p = &pixels[by * w + bx];
            uint32_t c = *p;
            if (!(c & 0xFF000000u)) continue;
            uint8_t rr = (uint8_t)(((c >> 16) & 0xFF) / 3);
            uint8_t gg = (uint8_t)(((c >> 8)  & 0xFF) / 3);
            uint8_t bb = (uint8_t)(( c        & 0xFF) / 3);
            *p = rgb(rr, gg, bb);
        }
    }
    blit_text(pixels, w, h, x, y, scale, OVERLAY_LABEL_COLOR, buf);
}

/* Marker drawn at the curve cell containing the file's final byte, used when
 * the curve grid is bigger than the data so several corners would otherwise
 * collapse to the same clamped file-end offset. */
static void draw_end_marker(uint32_t *pixels, int w, int h,
                            int img_x0, int img_y0, int img_size, int cell,
                            int ex, int ey, size_t off, size_t file_size)
{
    int px = img_x0 + ex * cell;
    int py = img_y0 + ey * cell;
    int pad = 2;
    int r0 = px - pad;
    int r1 = px + cell + pad - 1;
    int rt = py - pad;
    int rb = py + cell + pad - 1;
    /* 2px-thick outline ring */
    draw_hline(pixels, w, h, r0,     r1,     rt,     OVERLAY_TITLE_COLOR);
    draw_hline(pixels, w, h, r0,     r1,     rb,     OVERLAY_TITLE_COLOR);
    draw_vline(pixels, w, h, r0,     rt,     rb,     OVERLAY_TITLE_COLOR);
    draw_vline(pixels, w, h, r1,     rt,     rb,     OVERLAY_TITLE_COLOR);
    draw_hline(pixels, w, h, r0 + 1, r1 - 1, rt + 1, OVERLAY_TITLE_COLOR);
    draw_hline(pixels, w, h, r0 + 1, r1 - 1, rb - 1, OVERLAY_TITLE_COLOR);
    draw_vline(pixels, w, h, r0 + 1, rt + 1, rb - 1, OVERLAY_TITLE_COLOR);
    draw_vline(pixels, w, h, r1 - 1, rt + 1, rb - 1, OVERLAY_TITLE_COLOR);

    char buf[32];
    format_hex_offset(buf, sizeof(buf), off, file_size);
    int scale = 1;
    int tw = text_width(scale, buf);
    int th = FONT_H * scale;
    int label_pad = 4;
    /* prefer placing label to the right of the marker; fall back to left */
    int tx = r1 + label_pad + 1;
    if (tx + tw > img_x0 + img_size - 2) tx = r0 - label_pad - tw;
    if (tx < img_x0 + 2) tx = img_x0 + 2;
    int ty = py + cell / 2 - th / 2;
    if (ty < img_y0 + 2) ty = img_y0 + 2;
    if (ty + th > img_y0 + img_size - 2) ty = img_y0 + img_size - th - 2;
    /* shadow box, same recipe as draw_corner_label */
    for (int by = ty - 1; by < ty + th + 1; by++) {
        for (int bx = tx - 1; bx < tx + tw + 1; bx++) {
            if (bx < 0 || bx >= w || by < 0 || by >= h) continue;
            uint32_t *p = &pixels[by * w + bx];
            uint32_t c = *p;
            if (!(c & 0xFF000000u)) continue;
            uint8_t rr = (uint8_t)(((c >> 16) & 0xFF) / 3);
            uint8_t gg = (uint8_t)(((c >> 8)  & 0xFF) / 3);
            uint8_t bb = (uint8_t)(( c        & 0xFF) / 3);
            *p = rgb(rr, gg, bb);
        }
    }
    blit_text(pixels, w, h, tx, ty, scale, OVERLAY_TITLE_COLOR, buf);
}

static void render_curve(uint32_t *pixels, int w, int h,
                         const uint8_t *data, size_t size,
                         size_t base_offset, size_t file_size,
                         render_mode_t mode,
                         void (*d2xy)(int, int, int *, int *))
{
    const uint32_t bg = rgb(8, 8, 12);
    if (mode == RENDER_FULL) fill(pixels, w, h, bg);
    if (size == 0 || w < 2 || h < 2) return;

    int side = (w < h) ? w : h;
    int n = 1;
    while ((n * 2) <= side) n *= 2;
    if (n < 2) return;
    int cell = side / n;
    if (cell < 1) cell = 1;
    int img = n * cell;
    int ox = (w - img) / 2;
    int oy = (h - img) / 2;

    uint64_t total_cells = (uint64_t)n * (uint64_t)n;
    uint64_t bpc = (size + total_cells - 1) / total_cells;
    if (bpc < 1) bpc = 1;

    /* track which d-value lands at each of the 4 corners */
    uint64_t corner_d[2][2] = {{0,0},{0,0}};
    bool corner_found[2][2] = {{false,false},{false,false}};

    for (uint64_t d = 0; d < total_cells; d++) {
        int cx, cy;
        d2xy(n, (int)d, &cx, &cy);
        if ((cx == 0 || cx == n - 1) && (cy == 0 || cy == n - 1)) {
            int ix = (cx == 0) ? 0 : 1;
            int iy = (cy == 0) ? 0 : 1;
            if (!corner_found[iy][ix]) {
                corner_d[iy][ix] = d;
                corner_found[iy][ix] = true;
            }
        }
        if (mode == RENDER_DECOR_ONLY) continue;
        uint64_t bs = d * bpc;
        if (bs >= size) continue;
        uint64_t be = bs + bpc;
        if (be > size) be = size;
        uint32_t sum = 0;
        uint32_t count = (uint32_t)(be - bs);
        for (uint64_t b = bs; b < be; b++) sum += data[b];
        uint8_t avg = (uint8_t)(sum / count);
        uint32_t color = curve_color(avg);
        int px = ox + cx * cell;
        int py = oy + cy * cell;
        for (int dy = 0; dy < cell; dy++) {
            uint32_t *row = pixels + (uint64_t)(py + dy) * (uint64_t)w + (uint64_t)px;
            for (int dx = 0; dx < cell; dx++) row[dx] = color;
        }
    }

    if (mode == RENDER_DATA_ONLY) return;

    /* corner offset labels — skip corners the data never reaches */
    for (int iy = 0; iy < 2; iy++) {
        for (int ix = 0; ix < 2; ix++) {
            if (!corner_found[iy][ix]) continue;
            uint64_t d = corner_d[iy][ix];
            if (d * bpc >= size) continue;
            size_t off = base_offset + (size_t)(d * bpc);
            draw_corner_label(pixels, w, h, ix, iy, ox, oy, img, off, file_size);
        }
    }

    /* end-of-data marker: only when the curve overshoots the actual data,
     * so the four-corner labels can't express where the file truly ends */
    uint64_t d_end = (size - 1) / bpc;
    if (d_end < total_cells - 1) {
        int ex = 0, ey = 0;
        d2xy(n, (int)d_end, &ex, &ey);
        draw_end_marker(pixels, w, h, ox, oy, img, cell,
                        ex, ey, base_offset + size, file_size);
    }
}

void render_hilbert(uint32_t *pixels, int w, int h, const uint8_t *data, size_t size,
                    size_t base_offset, size_t file_size, render_mode_t mode)
{
    render_curve(pixels, w, h, data, size, base_offset, file_size, mode, d2xy_hilbert);
}

/* Inverse of d2xy_hilbert: (cx, cy) -> d on an n x n grid. Mirrors the
 * rotation/reflection sequence used in d2xy_hilbert exactly. */
static int xy2d_hilbert(int n, int x, int y)
{
    int rx, ry, d = 0;
    for (int s = n / 2; s > 0; s /= 2) {
        rx = (x & s) ? 1 : 0;
        ry = (y & s) ? 1 : 0;
        d += s * s * ((3 * rx) ^ ry);
        if (ry == 0) {
            if (rx == 1) { x = s - 1 - x; y = s - 1 - y; }
            int tmp = x; x = y; y = tmp;
        }
    }
    return d;
}

bool render_hilbert_offset_at(int mx, int my, int w, int h,
                              size_t size, size_t base_offset, size_t *out_off)
{
    if (size == 0 || w < 2 || h < 2) return false;
    int side = (w < h) ? w : h;
    int n = 1;
    while ((n * 2) <= side) n *= 2;
    if (n < 2) return false;
    int cell = side / n;
    if (cell < 1) cell = 1;
    int img = n * cell;
    int ox = (w - img) / 2;
    int oy = (h - img) / 2;

    if (mx < ox || mx >= ox + img) return false;
    if (my < oy || my >= oy + img) return false;
    int cx = (mx - ox) / cell;
    int cy = (my - oy) / cell;
    if (cx < 0 || cx >= n || cy < 0 || cy >= n) return false;

    uint64_t total_cells = (uint64_t)n * (uint64_t)n;
    uint64_t bpc = (size + total_cells - 1) / total_cells;
    if (bpc < 1) bpc = 1;

    uint64_t d = (uint64_t)xy2d_hilbert(n, cx, cy);
    uint64_t cell_off = d * bpc;
    if (cell_off >= size) return false;
    if (out_off) *out_off = base_offset + (size_t)cell_off;
    return true;
}

/* ---------- Digraph dot plot ---------- */

void render_digraph(uint32_t *pixels, int w, int h, const uint8_t *data, size_t size,
                    size_t base_offset, size_t file_size, render_mode_t mode)
{
    (void)base_offset; (void)file_size;
    const uint32_t bg = rgb(5, 5, 10);
    if (mode == RENDER_FULL) fill(pixels, w, h, bg);
    if (size < 2) return;

    int side = (w < h) ? w : h;
    int cell = side / 256;
    if (cell < 1) cell = 1;
    int img = 256 * cell;
    int ox = (w - img) / 2;
    int oy = (h - img) / 2;

    /* faint grid every 0x10 = 16 byte values, drawn beneath the dots */
    if (mode != RENDER_DATA_ONLY) {
        for (int g = 16; g < 256; g += 16) {
            int gx = ox + g * cell;
            int gy = oy + g * cell;
            if (gx >= 0 && gx < w) draw_vline(pixels, w, h, gx, oy, oy + img - 1, OVERLAY_GRID_COLOR);
            if (gy >= 0 && gy < h) draw_hline(pixels, w, h, ox, ox + img - 1, gy, OVERLAY_GRID_COLOR);
        }
    }

    if (mode != RENDER_DECOR_ONLY) {
        static uint32_t counts[256 * 256];
        memset(counts, 0, sizeof(counts));
        uint32_t max_count = 0;
        for (size_t i = 0; i + 1 < size; i++) {
            uint32_t idx = ((uint32_t)data[i] << 8) | data[i + 1];
            counts[idx]++;
            if (counts[idx] > max_count) max_count = counts[idx];
        }
        double log_max = log((double)max_count + 1.0);
        if (log_max < 1e-9) log_max = 1.0;
        for (int y = 0; y < 256; y++) {
            for (int x = 0; x < 256; x++) {
                uint32_t c = counts[y * 256 + x];
                if (c == 0) continue;
                double t = log((double)c + 1.0) / log_max;
                uint32_t color = heat_color(t);
                int px = ox + x * cell;
                int py = oy + y * cell;
                for (int dy = 0; dy < cell; dy++) {
                    uint32_t *row = pixels + (uint64_t)(py + dy) * (uint64_t)w + (uint64_t)px;
                    for (int dx = 0; dx < cell; dx++) row[dx] = color;
                }
            }
        }
    }

    if (mode == RENDER_DATA_ONLY) return;

    /* axis title + edge labels at every 0x20 along both axes */
    int scale = 1;
    int label_h = FONT_H * scale;
    static const int ticks[] = {0x00, 0x20, 0x40, 0x60, 0x80, 0xA0, 0xC0, 0xE0, 0xFF};
    for (size_t i = 0; i < sizeof(ticks) / sizeof(ticks[0]); i++) {
        int v = ticks[i];
        int gx = ox + v * cell;
        int gy = oy + v * cell;
        char buf[8];
        snprintf(buf, sizeof(buf), "%02X", v);
        int tw = text_width(scale, buf);
        /* X axis (top edge) — labels above */
        int lx = gx - tw / 2;
        int ly = oy - label_h - 2;
        if (ly < 0) ly = 0;
        if (lx < 0) lx = 0;
        blit_text(pixels, w, h, lx, ly, scale, OVERLAY_DIM_COLOR, buf);
        /* Y axis (left edge) — labels to the left */
        int ly2 = gy - label_h / 2;
        int lx2 = ox - tw - 4;
        if (lx2 < 0) lx2 = 0;
        if (ly2 < 0) ly2 = 0;
        if (ly2 + label_h >= h) ly2 = h - label_h - 1;
        blit_text(pixels, w, h, lx2, ly2, scale, OVERLAY_DIM_COLOR, buf);
        /* tick marks on the axes */
        if (gx >= 0 && gx < w) draw_vline(pixels, w, h, gx, oy - 3, oy - 1, OVERLAY_TICK_COLOR);
        if (gy >= 0 && gy < h) draw_hline(pixels, w, h, ox - 3, ox - 1, gy, OVERLAY_TICK_COLOR);
    }

    if (mode == RENDER_FULL) {
        blit_text(pixels, w, h, ox, oy - label_h - 14, 2, OVERLAY_TITLE_COLOR,
                  "DIGRAPH  X=BYTE[I+1]  Y=BYTE[I]");
    }
}

/* ---------- Entropy heatmap ---------- */

static uint32_t entropy_color(double e)
{
    double t = e / 8.0;
    if (t < 0) t = 0; else if (t > 1) t = 1;
    int r, g, b;
    if (t < 0.25) {
        double k = t / 0.25;
        r = 0;
        g = 0;
        b = (int)(80 + 175 * k);
    } else if (t < 0.5) {
        double k = (t - 0.25) / 0.25;
        r = 0;
        g = (int)(180 * k);
        b = (int)(255 - 100 * k);
    } else if (t < 0.75) {
        double k = (t - 0.5) / 0.25;
        r = (int)(220 * k);
        g = (int)(180 + 50 * k);
        b = (int)(155 - 155 * k);
    } else {
        double k = (t - 0.75) / 0.25;
        r = (int)(220 + 35 * k);
        g = (int)(230 - 130 * k);
        b = 0;
    }
    return rgb(clamp_u8(r), clamp_u8(g), clamp_u8(b));
}

void render_entropy(uint32_t *pixels, int w, int h, const uint8_t *data, size_t size,
                    size_t base_offset, size_t file_size, render_mode_t mode)
{
    const uint32_t bg = rgb(8, 8, 12);
    uint64_t total_px = (uint64_t)w * (uint64_t)h;
    if (mode == RENDER_FULL) fill(pixels, w, h, bg);
    if (size == 0) return;

    if (mode != RENDER_DECOR_ONLY) {
        size_t chunk = total_px ? size / total_px : 0;
        if (chunk < 64) chunk = 64;
        size_t n_chunks = size / chunk;
        if (n_chunks > 0) {
            double *e = (double *)malloc(n_chunks * sizeof(double));
            if (e) {
                for (size_t c = 0; c < n_chunks; c++) {
                    uint32_t hist[256] = {0};
                    const uint8_t *p = data + c * chunk;
                    for (size_t i = 0; i < chunk; i++) hist[p[i]]++;
                    double ent = 0.0;
                    double inv = 1.0 / (double)chunk;
                    for (int i = 0; i < 256; i++) {
                        if (!hist[i]) continue;
                        double pp = (double)hist[i] * inv;
                        ent -= pp * log2(pp);
                    }
                    e[c] = ent;
                }
                for (uint64_t i = 0; i < total_px; i++) {
                    uint64_t ci = (i * n_chunks) / total_px;
                    if (ci >= n_chunks) {
                        if (mode == RENDER_FULL) pixels[i] = bg;
                        continue;
                    }
                    pixels[i] = entropy_color(e[ci]);
                }
                free(e);
            }
        }
    }

    if (mode != RENDER_DATA_ONLY && h >= 32) {
        draw_offset_rail_right(pixels, w, h, w - 2, 2, h - 2,
                               base_offset, size, file_size);
    }
}

/* ---------- Bit-plane view ----------
 * 8 sub-images in a 4x2 grid, one per bit position. */

void render_bit_plane(uint32_t *pixels, int w, int h, const uint8_t *data, size_t size,
                      size_t base_offset, size_t file_size, render_mode_t mode)
{
    const uint32_t bg     = rgb(15, 15, 20);
    const uint32_t border = rgb(60, 60, 75);
    const uint32_t off    = rgb(20, 20, 28);
    const uint32_t on     = rgb(220, 220, 235);
    if (mode == RENDER_FULL) fill(pixels, w, h, bg);
    if (size == 0 || w < 8 || h < 8) return;

    int cols = 4, rows = 2;
    int cell_w = w / cols;
    int cell_h = h / rows;
    int pad = 4;
    int label_h = (FONT_H + 2) * 2;

    for (int bit = 0; bit < 8; bit++) {
        int row = bit / cols;
        int col = bit % cols;
        int x0 = col * cell_w + pad;
        int y0 = row * cell_h + pad;
        int sw = cell_w - 2 * pad;
        int sh = cell_h - 2 * pad - label_h;
        if (sw < 4 || sh < 4) continue;

        /* border */
        if (mode != RENDER_DATA_ONLY) {
            for (int x = x0 - 1; x < x0 + sw + 1; x++) {
                if (x < 0 || x >= w) continue;
                if (y0 - 1 >= 0)             pixels[(y0 - 1) * w + x] = border;
                if (y0 + sh < h)             pixels[(y0 + sh) * w + x] = border;
            }
            for (int y = y0 - 1; y < y0 + sh + 1; y++) {
                if (y < 0 || y >= h) continue;
                if (x0 - 1 >= 0)             pixels[y * w + x0 - 1] = border;
                if (x0 + sw < w)             pixels[y * w + x0 + sw] = border;
            }
        }

        if (mode != RENDER_DECOR_ONLY) {
            uint64_t cell_px = (uint64_t)sw * (uint64_t)sh;
            for (int py = 0; py < sh; py++) {
                for (int px = 0; px < sw; px++) {
                    uint64_t i = (uint64_t)py * (uint64_t)sw + (uint64_t)px;
                    uint64_t bi = (i * size) / cell_px;
                    if (bi >= size) {
                        if (mode == RENDER_FULL)
                            pixels[(y0 + py) * w + (x0 + px)] = bg;
                        continue;
                    }
                    uint8_t b = data[bi];
                    pixels[(y0 + py) * w + (x0 + px)] = ((b >> bit) & 1) ? on : off;
                }
            }
        }

        if (mode != RENDER_DATA_ONLY) {
            char label[16];
            snprintf(label, sizeof(label), "BIT %d", bit);
            blit_text(pixels, w, h, x0 + 2, y0 + sh + 4, 2, OVERLAY_TITLE_COLOR, label);

            /* corner offsets on the first panel only — applies to all panels */
            if (bit == 0) {
                char start_buf[32], end_buf[32];
                format_hex_offset(start_buf, sizeof(start_buf), base_offset, file_size);
                format_hex_offset(end_buf,   sizeof(end_buf),   base_offset + size, file_size);
                blit_text(pixels, w, h, x0 + 2, y0 + 2, 1, OVERLAY_LABEL_COLOR, start_buf);
                int ew = text_width(1, end_buf);
                int ex = x0 + sw - ew - 2;
                int ey = y0 + sh - FONT_H - 2;
                if (ex < x0 + 2) ex = x0 + 2;
                if (ey < y0 + 2) ey = y0 + 2;
                blit_text(pixels, w, h, ex, ey, 1, OVERLAY_LABEL_COLOR, end_buf);
            }
        }
    }
}

/* ---------- Strings density ----------
 * Per-chunk fraction of bytes that participate in a printable-ASCII run >= 4.
 * Reveals where strings cluster in firmware / binaries. */

static inline bool is_printable(uint8_t b) { return b >= 0x20 && b < 0x7F; }

void render_strings_density(uint32_t *pixels, int w, int h,
                            const uint8_t *data, size_t size,
                            size_t base_offset, size_t file_size, render_mode_t mode)
{
    const uint32_t bg = rgb(8, 8, 14);
    if (mode == RENDER_FULL) fill(pixels, w, h, bg);
    if (size == 0) return;

    if (mode != RENDER_DECOR_ONLY) {
        uint64_t total = (uint64_t)w * (uint64_t)h;
        size_t chunk = total ? size / total : 0;
        if (chunk < 64) chunk = 64;
        size_t n_chunks = size / chunk;
        if (n_chunks > 0) {
            double *density = (double *)malloc(n_chunks * sizeof(double));
            if (density) {
                for (size_t c = 0; c < n_chunks; c++) {
                    const uint8_t *p = data + c * chunk;
                    size_t in_str = 0;
                    size_t run = 0;
                    for (size_t i = 0; i < chunk; i++) {
                        if (is_printable(p[i])) {
                            run++;
                        } else {
                            if (run >= 4) in_str += run;
                            run = 0;
                        }
                    }
                    if (run >= 4) in_str += run;
                    density[c] = (double)in_str / (double)chunk;
                }
                /* black -> dim orange -> yellow -> white */
                for (uint64_t i = 0; i < total; i++) {
                    uint64_t ci = (i * n_chunks) / total;
                    if (ci >= n_chunks) continue;
                    double t = density[ci];
                    if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
                    int r, g, b;
                    if (t < 0.5) {
                        double k = t / 0.5;
                        r = (int)(240 * k);
                        g = (int)(140 * k);
                        b = (int)(20 * k);
                    } else {
                        double k = (t - 0.5) / 0.5;
                        r = (int)(240 + 15 * k);
                        g = (int)(140 + 110 * k);
                        b = (int)(20 + 220 * k);
                    }
                    pixels[i] = rgb(clamp_u8(r), clamp_u8(g), clamp_u8(b));
                }
                free(density);
            }
        }
    }

    if (mode != RENDER_DATA_ONLY && h >= 32) {
        draw_offset_rail_right(pixels, w, h, w - 2, 2, h - 2,
                               base_offset, size, file_size);
    }

    if (mode == RENDER_FULL) {
        blit_text(pixels, w, h, 8, 4, 2, OVERLAY_TITLE_COLOR,
                  "STRINGS DENSITY (ASCII RUNS >= 4)");
    }
}

/* ---------- Self-similarity matrix ----------
 * Divide the file into N chunks; render the N x N matrix of histogram
 * distances (L1 over 256-bin byte-frequency histograms). Bright = similar.
 * Repeated regions appear as off-diagonal stripes / blocks. */

void render_self_similarity(uint32_t *pixels, int w, int h,
                            const uint8_t *data, size_t size,
                            size_t base_offset, size_t file_size, render_mode_t mode)
{
    const uint32_t bg = rgb(8, 8, 14);
    if (mode == RENDER_FULL) fill(pixels, w, h, bg);
    if (size < 256 || w < 80 || h < 80) return;

    int side = (w < h) ? w : h;
    side -= 28;
    if (side < 16) return;

    int N = side;
    if (N > 512) N = 512;
    if ((size_t)N > size) N = (int)size;
    if (N < 8) return;

    size_t chunk_size = size / (size_t)N;
    if (chunk_size == 0) chunk_size = 1;

    int cell = side / N;
    if (cell < 1) cell = 1;
    int img = N * cell;
    int ox = (w - img) / 2;
    int oy = (h - img) / 2;

    if (mode != RENDER_DECOR_ONLY) {
        uint32_t (*hists)[256] = (uint32_t (*)[256])
                                  calloc((size_t)N, sizeof(uint32_t[256]));
        if (hists) {
            for (int c = 0; c < N; c++) {
                const uint8_t *p = data + (size_t)c * chunk_size;
                for (size_t i = 0; i < chunk_size; i++) hists[c][p[i]]++;
            }
            uint32_t *dist = (uint32_t *)malloc((size_t)N * (size_t)N * sizeof(uint32_t));
            if (dist) {
                uint32_t max_d = 1;
                for (int a = 0; a < N; a++) {
                    dist[a * N + a] = 0;
                    for (int b = a + 1; b < N; b++) {
                        uint32_t d = 0;
                        for (int i = 0; i < 256; i++) {
                            int32_t diff = (int32_t)hists[a][i] - (int32_t)hists[b][i];
                            d += (uint32_t)(diff < 0 ? -diff : diff);
                        }
                        dist[a * N + b] = d;
                        dist[b * N + a] = d;
                        if (d > max_d) max_d = d;
                    }
                }
                double inv_max = 1.0 / (double)max_d;
                for (int a = 0; a < N; a++) {
                    for (int b = 0; b < N; b++) {
                        uint32_t d = dist[a * N + b];
                        double t = 1.0 - (double)d * inv_max;   /* similarity 0..1 */
                        uint32_t color = heat_color(t);
                        int px = ox + b * cell;
                        int py = oy + a * cell;
                        for (int dy = 0; dy < cell; dy++) {
                            uint32_t *row = pixels + (uint64_t)(py + dy) * (uint64_t)w + (uint64_t)px;
                            for (int dx = 0; dx < cell; dx++) row[dx] = color;
                        }
                    }
                }
                free(dist);
            }
            free(hists);
        }
    }

    if (mode == RENDER_DATA_ONLY) return;

    /* title + offset ticks on top (x axis) and left (y axis) */
    if (mode == RENDER_FULL) {
        blit_text(pixels, w, h, ox, oy - FONT_H * 2 - 6, 2, OVERLAY_TITLE_COLOR,
                  "SELF-SIMILARITY (HISTOGRAM L1 DISTANCE)");
    }

    int scale = 1;
    int th = FONT_H * scale;
    for (int q = 0; q <= 4; q++) {
        size_t off = base_offset + (size_t)((double)q * 0.25 * (double)size);
        if (q == 4) off = base_offset + size;
        char buf[32];
        format_hex_offset(buf, sizeof(buf), off, file_size);
        int tw = text_width(scale, buf);
        int pos = (q * img) / 4;
        /* x axis labels along bottom edge */
        int bx = ox + pos - tw / 2;
        int by = oy + img + 4;
        if (bx < 0) bx = 0;
        if (bx + tw >= w) bx = w - tw - 1;
        blit_text(pixels, w, h, bx, by, scale, OVERLAY_LABEL_COLOR, buf);
        /* y axis labels along left edge */
        int ly = oy + pos - th / 2;
        int lx = ox - tw - 4;
        if (lx < 0) lx = 0;
        if (ly < 0) ly = 0;
        if (ly + th >= h) ly = h - th - 1;
        blit_text(pixels, w, h, lx, ly, scale, OVERLAY_LABEL_COLOR, buf);
        /* matrix-edge tick marks */
        if (ox + pos >= 0 && ox + pos < w)
            draw_vline(pixels, w, h, ox + pos, oy + img, oy + img + 3, OVERLAY_TICK_COLOR);
        if (oy + pos >= 0 && oy + pos < h)
            draw_hline(pixels, w, h, ox - 3, ox - 1, oy + pos, OVERLAY_TICK_COLOR);
    }
}

/* ---------- RGB raw view ----------
 * Each pixel = one (R, G, B) triple of consecutive bytes.
 * Files containing embedded bitmaps (raw textures, dumps) literally show up. */

void render_rgb_raw(uint32_t *pixels, int w, int h, const uint8_t *data, size_t size,
                    size_t base_offset, size_t file_size, render_mode_t mode)
{
    const uint32_t bg = rgb(8, 8, 14);
    if (mode == RENDER_FULL) fill(pixels, w, h, bg);
    if (size < 3) return;

    if (mode != RENDER_DECOR_ONLY) {
        uint64_t total = (uint64_t)w * (uint64_t)h;
        size_t n_triples = size / 3;
        if (n_triples > 0) {
            for (uint64_t i = 0; i < total; i++) {
                uint64_t ti = (i * n_triples) / total;
                if (ti >= n_triples) {
                    if (mode == RENDER_FULL) pixels[i] = bg;
                    continue;
                }
                size_t off = ti * 3;
                pixels[i] = rgb(data[off], data[off + 1], data[off + 2]);
            }
        }
    }

    if (mode != RENDER_DATA_ONLY && h >= 32) {
        draw_offset_rail_right(pixels, w, h, w - 2, 2, h - 2,
                               base_offset, size, file_size);
    }

    if (mode == RENDER_FULL) {
        blit_text(pixels, w, h, 8, 4, 2, OVERLAY_TITLE_COLOR,
                  "RGB RAW (3 BYTES PER PIXEL)");
    }
}

/* ---------- 3D helpers ---------- */

static inline bool project_pt(float x, float y, float z,
                              float cy, float sy, float cp, float sp,
                              float cam_d, float f, float ox, float oo,
                              float *out_x, float *out_y)
{
    float x1 =  x * cy + z * sy;
    float z1 = -x * sy + z * cy;
    float y2 =  y * cp - z1 * sp;
    float z2 =  y * sp + z1 * cp;
    float zc = z2 + cam_d;
    if (zc < 0.1f) return false;
    *out_x = ox + x1 * f / zc;
    *out_y = oo + y2 * f / zc;
    return true;
}

static void plot_pt(uint32_t *counts, int w, int h, int px, int py, uint32_t *max_c)
{
    if (px < 0 || px >= w || py < 0 || py >= h) return;
    uint32_t c = ++counts[py * w + px];
    if (c > *max_c) *max_c = c;
}

static void draw_line_pix(uint32_t *pixels, int w, int h,
                          float x0, float y0, float x1, float y1, uint32_t color)
{
    float dx = x1 - x0, dy = y1 - y0;
    int steps = (int)(fabsf(dx) + fabsf(dy));
    if (steps < 1) steps = 1;
    for (int i = 0; i <= steps; i++) {
        float t = (float)i / (float)steps;
        int px = (int)(x0 + dx * t);
        int py = (int)(y0 + dy * t);
        if (px < 0 || px >= w || py < 0 || py >= h) continue;
        pixels[py * w + px] = color;
    }
}

/* ---------- 3D trigraph point cloud ---------- */

void render_trigraph(uint32_t *pixels, int w, int h, const uint8_t *data, size_t size,
                     size_t base_offset, size_t file_size,
                     float yaw, float pitch, float zoom, render_mode_t mode)
{
    (void)base_offset; (void)file_size;
    const uint32_t bg = rgb(4, 4, 10);
    if (mode == RENDER_FULL) fill(pixels, w, h, bg);
    if (size < 3 || w < 16 || h < 16) return;

    float cy = cosf(yaw),   sy = sinf(yaw);
    float cp = cosf(pitch), sp = sinf(pitch);
    float cam_d = 3.6f;
    float f = (float)((w < h) ? w : h) * 0.45f * zoom;
    float ox = w * 0.5f, oo = h * 0.5f;

    /* bounding cube wireframe so orientation is legible */
    static const float cv[8][3] = {
        {-1,-1,-1},{ 1,-1,-1},{ 1, 1,-1},{-1, 1,-1},
        {-1,-1, 1},{ 1,-1, 1},{ 1, 1, 1},{-1, 1, 1},
    };
    static const int ce[12][2] = {
        {0,1},{1,2},{2,3},{3,0},
        {4,5},{5,6},{6,7},{7,4},
        {0,4},{1,5},{2,6},{3,7},
    };
    float pv[8][2];
    bool ok[8];
    for (int i = 0; i < 8; i++) {
        ok[i] = project_pt(cv[i][0], cv[i][1], cv[i][2],
                           cy, sy, cp, sp, cam_d, f, ox, oo,
                           &pv[i][0], &pv[i][1]);
    }

    if (mode != RENDER_DATA_ONLY) {
        uint32_t cube_col = rgb(60, 60, 90);
        for (int i = 0; i < 12; i++) {
            if (!ok[ce[i][0]] || !ok[ce[i][1]]) continue;
            draw_line_pix(pixels, w, h,
                          pv[ce[i][0]][0], pv[ce[i][0]][1],
                          pv[ce[i][1]][0], pv[ce[i][1]][1],
                          cube_col);
        }
    }

    if (mode != RENDER_DECOR_ONLY) {
        uint32_t *counts = (uint32_t *)calloc((size_t)w * (size_t)h, sizeof(uint32_t));
        if (counts) {
            /* sample triples */
            size_t step = 1;
            size_t cap = 400000;
            size_t triples = size - 2;
            if (triples > cap) step = triples / cap;

            uint32_t max_c = 0;
            for (size_t i = 0; i + 2 < size; i += step) {
                float xx = ((float)data[i]     / 127.5f) - 1.0f;
                float yy = ((float)data[i + 1] / 127.5f) - 1.0f;
                float zz = ((float)data[i + 2] / 127.5f) - 1.0f;
                float sxp, syp;
                if (!project_pt(xx, yy, zz, cy, sy, cp, sp, cam_d, f, ox, oo, &sxp, &syp)) continue;
                plot_pt(counts, w, h, (int)sxp, (int)syp, &max_c);
            }

            if (max_c > 0) {
                double log_max = log((double)max_c + 1.0);
                if (log_max < 1e-9) log_max = 1.0;
                uint64_t n = (uint64_t)w * (uint64_t)h;
                for (uint64_t i = 0; i < n; i++) {
                    uint32_t c = counts[i];
                    if (c == 0) continue;
                    double t = log((double)c + 1.0) / log_max;
                    pixels[i] = heat_color(t);
                }
            }
            free(counts);
        }
    }

    if (mode == RENDER_DATA_ONLY) return;

    /* label each visible cube corner with its (a,b,c) value */
    for (int i = 0; i < 8; i++) {
        if (!ok[i]) continue;
        int a = (cv[i][0] < 0) ? 0x00 : 0xFF;
        int b = (cv[i][1] < 0) ? 0x00 : 0xFF;
        int c = (cv[i][2] < 0) ? 0x00 : 0xFF;
        char buf[16];
        snprintf(buf, sizeof(buf), "%02X,%02X,%02X", a, b, c);
        int scale = 1;
        int tw = text_width(scale, buf);
        int th = FONT_H * scale;
        int lx = (int)pv[i][0] + 4;
        int ly = (int)pv[i][1] - th - 2;
        if (lx + tw >= w) lx = (int)pv[i][0] - tw - 4;
        if (ly < 0)       ly = (int)pv[i][1] + 4;
        if (lx < 0) lx = 0;
        if (ly < 0) ly = 0;
        if (lx + tw >= w) lx = w - tw - 1;
        if (ly + th >= h) ly = h - th - 1;
        blit_text(pixels, w, h, lx, ly, scale, OVERLAY_LABEL_COLOR, buf);
    }

    if (mode == RENDER_FULL) {
        blit_text(pixels, w, h, 8, 4, 2, OVERLAY_TITLE_COLOR,
                  "TRIGRAPH (BYTE TRIPLES IN 0..255 CUBE)");
    }
}

/* ---------- Spherical trigraph (Veles-style) ----------
 * For each consecutive byte triple (a, b, c):
 *   theta = a/256 * 2pi   (azimuth)
 *   phi   = b/256 * pi    (inclination, 0 = north pole)
 *   r     = c/255         (radius in [0, 1])
 * yielding a point in the unit sphere (or its interior).
 */

void render_trigraph_spherical(uint32_t *pixels, int w, int h,
                               const uint8_t *data, size_t size,
                               size_t base_offset, size_t file_size,
                               float yaw, float pitch, float zoom, render_mode_t mode)
{
    (void)base_offset; (void)file_size;
    const uint32_t bg = rgb(4, 4, 10);
    if (mode == RENDER_FULL) fill(pixels, w, h, bg);
    if (size < 3 || w < 16 || h < 16) return;

    float cy = cosf(yaw),   sy = sinf(yaw);
    float cp = cosf(pitch), sp = sinf(pitch);
    float cam_d = 3.6f;
    float f = (float)((w < h) ? w : h) * 0.45f * zoom;
    float ox = w * 0.5f, oo = h * 0.5f;

    /* LUTs */
    float cos_th[256], sin_th[256], cos_ph[256], sin_ph[256];
    for (int i = 0; i < 256; i++) {
        float th = (float)i * (2.0f * (float)M_PI / 256.0f);
        float ph = (float)i * ((float)M_PI / 256.0f);
        cos_th[i] = cosf(th); sin_th[i] = sinf(th);
        cos_ph[i] = cosf(ph); sin_ph[i] = sinf(ph);
    }

    if (mode != RENDER_DATA_ONLY) {
        /* wireframe: equator + two perpendicular meridians */
        uint32_t wire_col = rgb(60, 60, 90);
        const int segs = 96;
        float pp_x = 0, pp_y = 0; bool have_prev;

        /* equator (phi = pi/2, varying theta) -> in XZ plane, y = 0 */
        have_prev = false;
        for (int s = 0; s <= segs; s++) {
            float th = (float)s * (2.0f * (float)M_PI / (float)segs);
            float xx = cosf(th), yy = 0.0f, zz = sinf(th);
            float sxp, syp;
            if (!project_pt(xx, yy, zz, cy, sy, cp, sp, cam_d, f, ox, oo, &sxp, &syp)) {
                have_prev = false; continue;
            }
            if (have_prev) draw_line_pix(pixels, w, h, pp_x, pp_y, sxp, syp, wire_col);
            pp_x = sxp; pp_y = syp; have_prev = true;
        }
        /* meridian theta = 0 -> circle in Y-Z plane */
        have_prev = false;
        for (int s = 0; s <= segs; s++) {
            float ph = (float)s * (2.0f * (float)M_PI / (float)segs);
            float xx = 0.0f, yy = cosf(ph), zz = sinf(ph);
            float sxp, syp;
            if (!project_pt(xx, yy, zz, cy, sy, cp, sp, cam_d, f, ox, oo, &sxp, &syp)) {
                have_prev = false; continue;
            }
            if (have_prev) draw_line_pix(pixels, w, h, pp_x, pp_y, sxp, syp, wire_col);
            pp_x = sxp; pp_y = syp; have_prev = true;
        }
        /* meridian theta = pi/2 -> circle in X-Y plane */
        have_prev = false;
        for (int s = 0; s <= segs; s++) {
            float ph = (float)s * (2.0f * (float)M_PI / (float)segs);
            float xx = sinf(ph), yy = cosf(ph), zz = 0.0f;
            float sxp, syp;
            if (!project_pt(xx, yy, zz, cy, sy, cp, sp, cam_d, f, ox, oo, &sxp, &syp)) {
                have_prev = false; continue;
            }
            if (have_prev) draw_line_pix(pixels, w, h, pp_x, pp_y, sxp, syp, wire_col);
            pp_x = sxp; pp_y = syp; have_prev = true;
        }
    }

    if (mode != RENDER_DECOR_ONLY) {
        uint32_t *counts = (uint32_t *)calloc((size_t)w * (size_t)h, sizeof(uint32_t));
        if (counts) {
            /* sample triples */
            size_t step = 1;
            size_t cap = 400000;
            size_t triples = size - 2;
            if (triples > cap) step = triples / cap;

            uint32_t max_c = 0;
            for (size_t i = 0; i + 2 < size; i += step) {
                uint8_t a = data[i];
                uint8_t b = data[i + 1];
                uint8_t c = data[i + 2];
                float r = (float)c * (1.0f / 255.0f);
                float xx = r * sin_ph[b] * cos_th[a];
                float yy = r * cos_ph[b];
                float zz = r * sin_ph[b] * sin_th[a];
                float sxp, syp;
                if (!project_pt(xx, yy, zz, cy, sy, cp, sp, cam_d, f, ox, oo, &sxp, &syp)) continue;
                plot_pt(counts, w, h, (int)sxp, (int)syp, &max_c);
            }

            if (max_c > 0) {
                double log_max = log((double)max_c + 1.0);
                if (log_max < 1e-9) log_max = 1.0;
                uint64_t n = (uint64_t)w * (uint64_t)h;
                for (uint64_t i = 0; i < n; i++) {
                    uint32_t c = counts[i];
                    if (c == 0) continue;
                    double t = log((double)c + 1.0) / log_max;
                    pixels[i] = heat_color(t);
                }
            }
            free(counts);
        }
    }

    if (mode == RENDER_DATA_ONLY) return;

    /* anchor labels at sphere axes */
    {
        struct { float x, y, z; const char *label; } anchors[] = {
            { 1.0f,  0.0f,  0.0f, "A=0X00" },   /* theta=0    */
            {-1.0f,  0.0f,  0.0f, "A=0X80" },   /* theta=pi   */
            { 0.0f,  1.0f,  0.0f, "B=0X00" },   /* phi=0      */
            { 0.0f, -1.0f,  0.0f, "B=0XFF" },   /* phi=pi     */
            { 0.0f,  0.0f,  1.0f, "C=0XFF" },   /* rim r=1    */
        };
        for (size_t k = 0; k < sizeof(anchors) / sizeof(anchors[0]); k++) {
            float sxp, syp;
            if (!project_pt(anchors[k].x, anchors[k].y, anchors[k].z,
                            cy, sy, cp, sp, cam_d, f, ox, oo, &sxp, &syp))
                continue;
            int scale = 1;
            int tw = text_width(scale, anchors[k].label);
            int th = FONT_H * scale;
            int lx = (int)sxp + 4;
            int ly = (int)syp - th / 2;
            if (lx + tw >= w) lx = (int)sxp - tw - 4;
            if (lx < 0) lx = 0;
            if (ly < 0) ly = 0;
            if (lx + tw >= w) lx = w - tw - 1;
            if (ly + th >= h) ly = h - th - 1;
            blit_text(pixels, w, h, lx, ly, scale, OVERLAY_LABEL_COLOR, anchors[k].label);
        }
    }

    if (mode == RENDER_FULL) {
        blit_text(pixels, w, h, 8, 4, 2, OVERLAY_TITLE_COLOR,
                  "SPHERICAL TRIGRAPH (THETA=A PHI=B R=C)");
    }
}


