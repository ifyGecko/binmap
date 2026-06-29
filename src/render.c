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
        /* faint shadow background so text reads over any palette */
        for (int by = ty - 1; by < ty + label_h + 1; by++) {
            for (int bx = tx - 1; bx < tx + tw + 1; bx++) {
                if (bx < 0 || bx >= w || by < 0 || by >= h) continue;
                uint32_t *p = &pixels[by * w + bx];
                uint32_t c = *p;
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
                       size_t base_offset, size_t file_size)
{
    const uint32_t bg = rgb(15, 15, 20);
    uint64_t total = (uint64_t)w * (uint64_t)h;
    if (size == 0 || total == 0) { fill(pixels, w, h, bg); return; }
    for (uint64_t i = 0; i < total; i++) {
        uint64_t bi = (i * size) / total;
        if (bi >= size) { pixels[i] = bg; continue; }
        pixels[i] = byte_class_color(data[bi]);
    }
    /* offset rail is meaningless on the 1-pixel-tall minimap strip */
    if (h >= 32) {
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
    /* shadow */
    for (int by = y - 1; by < y + th + 1; by++) {
        for (int bx = x - 1; bx < x + tw + 1; bx++) {
            if (bx < 0 || bx >= w || by < 0 || by >= h) continue;
            uint32_t *p = &pixels[by * w + bx];
            uint32_t c = *p;
            uint8_t rr = (uint8_t)(((c >> 16) & 0xFF) / 3);
            uint8_t gg = (uint8_t)(((c >> 8)  & 0xFF) / 3);
            uint8_t bb = (uint8_t)(( c        & 0xFF) / 3);
            *p = rgb(rr, gg, bb);
        }
    }
    blit_text(pixels, w, h, x, y, scale, OVERLAY_LABEL_COLOR, buf);
}

static void render_curve(uint32_t *pixels, int w, int h,
                         const uint8_t *data, size_t size,
                         size_t base_offset, size_t file_size,
                         void (*d2xy)(int, int, int *, int *))
{
    const uint32_t bg = rgb(8, 8, 12);
    fill(pixels, w, h, bg);
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

    /* corner offset labels */
    for (int iy = 0; iy < 2; iy++) {
        for (int ix = 0; ix < 2; ix++) {
            if (!corner_found[iy][ix]) continue;
            uint64_t d = corner_d[iy][ix];
            size_t off = base_offset + (size_t)(d * bpc);
            if (off > base_offset + size) off = base_offset + size;
            draw_corner_label(pixels, w, h, ix, iy, ox, oy, img, off, file_size);
        }
    }
}

void render_hilbert(uint32_t *pixels, int w, int h, const uint8_t *data, size_t size,
                    size_t base_offset, size_t file_size)
{
    render_curve(pixels, w, h, data, size, base_offset, file_size, d2xy_hilbert);
}

/* ---------- Z-order (Morton) curve ---------- */

static void d2xy_morton(int n, int d, int *x, int *y)
{
    (void)n;
    int xv = 0, yv = 0;
    for (int i = 0; i < 16; i++) {
        xv |= ((d >> (2 * i))     & 1) << i;
        yv |= ((d >> (2 * i + 1)) & 1) << i;
    }
    *x = xv;
    *y = yv;
}

void render_morton(uint32_t *pixels, int w, int h, const uint8_t *data, size_t size,
                   size_t base_offset, size_t file_size)
{
    render_curve(pixels, w, h, data, size, base_offset, file_size, d2xy_morton);
}

/* ---------- Digraph dot plot ---------- */

void render_digraph(uint32_t *pixels, int w, int h, const uint8_t *data, size_t size,
                    size_t base_offset, size_t file_size)
{
    (void)base_offset; (void)file_size;
    const uint32_t bg = rgb(5, 5, 10);
    fill(pixels, w, h, bg);
    if (size < 2) return;

    static uint32_t counts[256 * 256];
    memset(counts, 0, sizeof(counts));
    uint32_t max_count = 0;
    for (size_t i = 0; i + 1 < size; i++) {
        uint32_t idx = ((uint32_t)data[i] << 8) | data[i + 1];
        counts[idx]++;
        if (counts[idx] > max_count) max_count = counts[idx];
    }

    int side = (w < h) ? w : h;
    int cell = side / 256;
    if (cell < 1) cell = 1;
    int img = 256 * cell;
    int ox = (w - img) / 2;
    int oy = (h - img) / 2;

    double log_max = log((double)max_count + 1.0);
    if (log_max < 1e-9) log_max = 1.0;

    /* faint grid every 0x10 = 16 byte values, drawn beneath the dots */
    for (int g = 16; g < 256; g += 16) {
        int gx = ox + g * cell;
        int gy = oy + g * cell;
        if (gx >= 0 && gx < w) draw_vline(pixels, w, h, gx, oy, oy + img - 1, OVERLAY_GRID_COLOR);
        if (gy >= 0 && gy < h) draw_hline(pixels, w, h, ox, ox + img - 1, gy, OVERLAY_GRID_COLOR);
    }

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

    blit_text(pixels, w, h, ox, oy - label_h - 14, 2, OVERLAY_TITLE_COLOR,
              "DIGRAPH  X=BYTE[I+1]  Y=BYTE[I]");
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
                    size_t base_offset, size_t file_size)
{
    const uint32_t bg = rgb(8, 8, 12);
    uint64_t total_px = (uint64_t)w * (uint64_t)h;
    fill(pixels, w, h, bg);
    if (size == 0) return;

    size_t chunk = total_px ? size / total_px : 0;
    if (chunk < 64) chunk = 64;
    size_t n_chunks = size / chunk;
    if (n_chunks == 0) return;

    double *e = (double *)malloc(n_chunks * sizeof(double));
    if (!e) return;

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
        if (ci >= n_chunks) { pixels[i] = bg; continue; }
        pixels[i] = entropy_color(e[ci]);
    }
    free(e);

    if (h >= 32) {
        draw_offset_rail_right(pixels, w, h, w - 2, 2, h - 2,
                               base_offset, size, file_size);
    }
}

/* ---------- Bit-plane view ----------
 * 8 sub-images in a 4x2 grid, one per bit position. */

void render_bit_plane(uint32_t *pixels, int w, int h, const uint8_t *data, size_t size,
                      size_t base_offset, size_t file_size)
{
    const uint32_t bg     = rgb(15, 15, 20);
    const uint32_t border = rgb(60, 60, 75);
    const uint32_t off    = rgb(20, 20, 28);
    const uint32_t on     = rgb(220, 220, 235);
    fill(pixels, w, h, bg);
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

        uint64_t cell_px = (uint64_t)sw * (uint64_t)sh;
        for (int py = 0; py < sh; py++) {
            for (int px = 0; px < sw; px++) {
                uint64_t i = (uint64_t)py * (uint64_t)sw + (uint64_t)px;
                uint64_t bi = (i * size) / cell_px;
                if (bi >= size) {
                    pixels[(y0 + py) * w + (x0 + px)] = bg;
                    continue;
                }
                uint8_t b = data[bi];
                pixels[(y0 + py) * w + (x0 + px)] = ((b >> bit) & 1) ? on : off;
            }
        }

        char label[16];
        snprintf(label, sizeof(label), "BIT %d", bit);
        blit_text(pixels, w, h, x0 + 2, y0 + sh + 4, 2, rgb(255, 200, 100), label);

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

/* ---------- Byte histogram ---------- */

void render_histogram(uint32_t *pixels, int w, int h, const uint8_t *data, size_t size,
                      size_t base_offset, size_t file_size)
{
    (void)base_offset; (void)file_size;
    const uint32_t bg     = rgb(12, 12, 18);
    const uint32_t grid   = rgb(35, 35, 45);
    const uint32_t axis   = rgb(110, 110, 130);
    fill(pixels, w, h, bg);
    if (size == 0 || w < 32 || h < 32) return;

    int margin_l = 56;
    int margin_b = 28;
    int margin_t = 18;
    int margin_r = 14;
    int plot_w = w - margin_l - margin_r;
    int plot_h = h - margin_t - margin_b;
    if (plot_w < 64 || plot_h < 32) return;

    uint64_t counts[256] = {0};
    for (size_t i = 0; i < size; i++) counts[data[i]]++;

    uint64_t max_c = 0;
    for (int i = 0; i < 256; i++) if (counts[i] > max_c) max_c = counts[i];
    if (max_c == 0) return;
    double log_max = log((double)max_c + 1.0);

    /* horizontal grid lines (log-scale, decade boundaries) */
    for (double v = 1; v <= (double)max_c; v *= 10) {
        double t = log(v + 1.0) / log_max;
        int gy = margin_t + plot_h - (int)(t * plot_h);
        if (gy < margin_t || gy >= margin_t + plot_h) continue;
        for (int x = margin_l; x < margin_l + plot_w; x++) {
            pixels[gy * w + x] = grid;
        }
    }

    /* axis */
    for (int x = margin_l; x < margin_l + plot_w; x++) {
        int y = margin_t + plot_h;
        if (y < h) pixels[y * w + x] = axis;
    }
    for (int y = margin_t; y < margin_t + plot_h; y++) {
        pixels[y * w + margin_l - 1] = axis;
    }

    /* bars */
    for (int bin = 0; bin < 256; bin++) {
        uint64_t c = counts[bin];
        int x0 = margin_l + (bin * plot_w) / 256;
        int x1 = margin_l + ((bin + 1) * plot_w) / 256;
        if (x1 <= x0) x1 = x0 + 1;
        if (x1 > margin_l + plot_w) x1 = margin_l + plot_w;
        double t = c ? log((double)c + 1.0) / log_max : 0.0;
        int bar_h = (int)(t * plot_h);
        int y_top = margin_t + plot_h - bar_h;
        uint32_t bc = byte_class_color((uint8_t)bin);
        for (int y = y_top; y < margin_t + plot_h; y++) {
            for (int x = x0; x < x1 - 1; x++) {
                pixels[y * w + x] = bc;
            }
        }
    }

    /* labels */
    uint32_t label_color = rgb(220, 220, 235);
    blit_text(pixels, w, h, margin_l, 4, 2, rgb(255, 200, 100), "BYTE HISTOGRAM (LOG SCALE)");

    /* x-axis ticks every 0x20 + endpoints */
    static const int x_ticks[] = {0x00, 0x20, 0x40, 0x60, 0x80, 0xA0, 0xC0, 0xE0, 0xFF};
    int label_scale = 2;
    int label_pix_h = FONT_H * label_scale;
    int label_y = margin_t + plot_h + 8;
    for (size_t i = 0; i < sizeof(x_ticks) / sizeof(x_ticks[0]); i++) {
        int v = x_ticks[i];
        int x_center = margin_l + (v * plot_w) / 256;
        char tb[8];
        snprintf(tb, sizeof(tb), "%02X", v);
        int tw = text_width(label_scale, tb);
        int lx = x_center - tw / 2;
        if (lx < margin_l) lx = margin_l;
        if (lx + tw > margin_l + plot_w) lx = margin_l + plot_w - tw;
        blit_text(pixels, w, h, lx, label_y, label_scale, label_color, tb);
        /* tick mark on axis */
        int tick_y = margin_t + plot_h;
        if (x_center >= margin_l && x_center < margin_l + plot_w) {
            draw_vline(pixels, w, h, x_center, tick_y, tick_y + 3, axis);
        }
    }
    (void)label_pix_h;

    char buf[48];
    snprintf(buf, sizeof(buf), "MAX %llu", (unsigned long long)max_c);
    blit_text(pixels, w, h, 4, margin_t, 2, label_color, buf);
}

/* ---------- Polar spiral ----------
 * Uniform-area Archimedean spiral filling a disk:
 *   t = byte_index / size in [0, 1]
 *   r(t) = R * sqrt(t)                 (equal-area per byte)
 *   theta(t) = N * 2*pi * t            (N = revolution count)
 * Implemented by pixel iteration with inverse mapping so the disk is filled
 * with no aliasing gaps. Byte 0 is at the center; the last byte is at the
 * outer edge.
 */

/* Draw a partial outline (just an arc from a0 to a1 in radians) of radius R. */
static void disk_arc(uint32_t *pixels, int w, int h,
                     float cx_f, float cy_f, float R,
                     float a0, float a1, uint32_t color)
{
    int segs = 64;
    float prev_x = 0, prev_y = 0;
    bool have_prev = false;
    for (int s = 0; s <= segs; s++) {
        float a = a0 + (a1 - a0) * ((float)s / (float)segs);
        float xx = cx_f + R * cosf(a);
        float yy = cy_f + R * sinf(a);
        if (have_prev) draw_line_pix(pixels, w, h, prev_x, prev_y, xx, yy, color);
        prev_x = xx; prev_y = yy; have_prev = true;
    }
}

/* Draw a quartile ring at radius r_frac (0..1 of R) labeled with the absolute
 * file offset corresponding to that fraction of the slice. */
static void draw_quartile_ring(uint32_t *pixels, int w, int h,
                               float cx_f, float cy_f, float R,
                               float r_frac, size_t off, size_t file_size,
                               uint32_t color)
{
    float Rq = R * r_frac;
    /* dashed-ish ring: skip a small arc near 3 o'clock for the label */
    disk_arc(pixels, w, h, cx_f, cy_f, Rq,
             0.18f, 2.0f * (float)M_PI - 0.18f, color);
    char buf[40];
    format_hex_offset(buf, sizeof(buf), off, file_size);
    int scale = 1;
    int tw = text_width(scale, buf);
    int th = FONT_H * scale;
    int lx = (int)(cx_f + Rq) + 6;
    int ly = (int)cy_f - th / 2;
    if (lx + tw >= w) lx = w - tw - 2;
    if (ly < 0) ly = 0;
    if (ly + th >= h) ly = h - th - 1;
    blit_text(pixels, w, h, lx, ly, scale, OVERLAY_LABEL_COLOR, buf);
}

void render_polar(uint32_t *pixels, int w, int h, const uint8_t *data, size_t size,
                  size_t base_offset, size_t file_size)
{
    const uint32_t bg     = rgb(8, 8, 14);
    const uint32_t border = rgb(80, 80, 110);
    const uint32_t center = rgb(255, 200, 100);
    fill(pixels, w, h, bg);
    if (size == 0 || w < 32 || h < 32) return;

    float cx_f = w * 0.5f;
    float cy_f = h * 0.5f;
    float R = (float)((w < h) ? w : h) * 0.5f - 14.0f;
    if (R < 8.0f) return;
    float R2 = R * R;
    float two_pi = 2.0f * (float)M_PI;

    /* Pick N so each revolution is roughly two pixels thick at the edge.
     * dr/d(rev) = R / (2*N*sqrt(t)); at t=1: dr ~ R/(2N). Want dr ~ 1.5 px. */
    float N = R / 3.0f;
    if (N < 1.0f) N = 1.0f;
    float cap = sqrtf((float)size / 3.14159265f);
    if (N > cap && cap > 1.0f) N = cap;
    float total_theta = N * two_pi;

    for (int py = 0; py < h; py++) {
        float dy = (float)py - cy_f;
        uint32_t *row = pixels + (size_t)py * (size_t)w;
        for (int px = 0; px < w; px++) {
            float dx = (float)px - cx_f;
            float r2 = dx * dx + dy * dy;
            if (r2 > R2) continue;

            float t_rad = r2 / R2;                /* (r/R)^2, uniform area */
            float theta_exp = total_theta * t_rad;
            float theta_n = atan2f(dy, dx);
            if (theta_n < 0.0f) theta_n += two_pi;

            /* find revolution k so theta_n + 2*pi*k closest to theta_exp */
            float kf = (theta_exp - theta_n) / two_pi;
            int   k  = (int)floorf(kf + 0.5f);
            float adj = theta_n + (float)k * two_pi;
            if (adj < 0.0f || adj > total_theta) continue;

            float t = adj / total_theta;
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            size_t bi = (size_t)(t * (float)size);
            if (bi >= size) bi = size - 1;
            row[px] = byte_class_color(data[bi]);
        }
    }

    /* outer ring */
    int segs = 256;
    float prev_x = 0, prev_y = 0;
    bool have_prev = false;
    for (int s = 0; s <= segs; s++) {
        float a = (float)s * (two_pi / (float)segs);
        float xx = cx_f + R * cosf(a);
        float yy = cy_f + R * sinf(a);
        if (have_prev) draw_line_pix(pixels, w, h, prev_x, prev_y, xx, yy, border);
        prev_x = xx; prev_y = yy; have_prev = true;
    }
    /* center marker */
    int cx_i = (int)cx_f;
    int cy_i = (int)cy_f;
    for (int dy = -2; dy <= 2; dy++) {
        for (int dx = -2; dx <= 2; dx++) {
            int x = cx_i + dx, y = cy_i + dy;
            if (x < 0 || x >= w || y < 0 || y >= h) continue;
            if (dx * dx + dy * dy <= 4) pixels[y * w + x] = center;
        }
    }

    /* equal-area quartile rings: t = (r/R)^2 → r/R = sqrt(t) */
    for (int q = 1; q <= 3; q++) {
        float frac = (float)q * 0.25f;
        size_t off = base_offset + (size_t)(frac * (double)size);
        draw_quartile_ring(pixels, w, h, cx_f, cy_f, R, sqrtf(frac),
                           off, file_size, border);
    }
    /* anchor labels for center (start offset) and rim (end offset) */
    char start_buf[40], end_buf[40];
    format_hex_offset(start_buf, sizeof(start_buf), base_offset, file_size);
    format_hex_offset(end_buf,   sizeof(end_buf),   base_offset + size, file_size);
    blit_text(pixels, w, h, (int)cx_f + 6, (int)cy_f + 6, 1, OVERLAY_LABEL_COLOR, start_buf);
    {
        int tw = text_width(1, end_buf);
        int lx = (int)(cx_f + R) - tw - 4;
        int ly = (int)cy_f + 4;
        if (lx + tw >= w) lx = w - tw - 2;
        if (ly < 0) ly = 0;
        if (ly + FONT_H >= h) ly = h - FONT_H - 1;
        blit_text(pixels, w, h, lx, ly, 1, OVERLAY_LABEL_COLOR, end_buf);
    }

    blit_text(pixels, w, h, 8, 4, 2, rgb(255, 200, 100),
              "POLAR SPIRAL (CENTER=BYTE 0, EDGE=END)");
}

/* ---------- Concentric rings ----------
 * Like polar spiral but with sharp ring boundaries (no spiral continuity).
 * Each ring covers a fixed slice of bytes; angular position within a ring
 * is the offset inside that slice. Reveals block-aligned structure cleanly. */

static void disk_outline(uint32_t *pixels, int w, int h,
                         float cx_f, float cy_f, float R, uint32_t color)
{
    const int segs = 256;
    float two_pi = 2.0f * (float)M_PI;
    float prev_x = 0, prev_y = 0;
    bool have_prev = false;
    for (int s = 0; s <= segs; s++) {
        float a = (float)s * (two_pi / (float)segs);
        float xx = cx_f + R * cosf(a);
        float yy = cy_f + R * sinf(a);
        if (have_prev) draw_line_pix(pixels, w, h, prev_x, prev_y, xx, yy, color);
        prev_x = xx; prev_y = yy; have_prev = true;
    }
}

static void disk_center_marker(uint32_t *pixels, int w, int h, int cx_i, int cy_i,
                               uint32_t color)
{
    for (int dy = -2; dy <= 2; dy++) {
        for (int dx = -2; dx <= 2; dx++) {
            int x = cx_i + dx, y = cy_i + dy;
            if (x < 0 || x >= w || y < 0 || y >= h) continue;
            if (dx * dx + dy * dy <= 4) pixels[y * w + x] = color;
        }
    }
}

void render_concentric_rings(uint32_t *pixels, int w, int h,
                             const uint8_t *data, size_t size,
                             size_t base_offset, size_t file_size)
{
    const uint32_t bg     = rgb(8, 8, 14);
    const uint32_t border = rgb(80, 80, 110);
    const uint32_t center = rgb(255, 200, 100);
    fill(pixels, w, h, bg);
    if (size == 0 || w < 32 || h < 32) return;

    float cx_f = w * 0.5f;
    float cy_f = h * 0.5f;
    float R = (float)((w < h) ? w : h) * 0.5f - 14.0f;
    if (R < 8.0f) return;
    float R2 = R * R;
    float two_pi = 2.0f * (float)M_PI;

    int n_rings = (int)(R / 4.0f);
    if (n_rings < 8) n_rings = 8;
    if ((size_t)n_rings > size) n_rings = (int)size;

    size_t bytes_per_ring = size / (size_t)n_rings;
    if (bytes_per_ring == 0) bytes_per_ring = 1;

    for (int py = 0; py < h; py++) {
        float dy = (float)py - cy_f;
        uint32_t *row = pixels + (size_t)py * (size_t)w;
        for (int px = 0; px < w; px++) {
            float dx = (float)px - cx_f;
            float r2 = dx * dx + dy * dy;
            if (r2 > R2) continue;

            float t_rad = r2 / R2;
            int ring_k = (int)(t_rad * (float)n_rings);
            if (ring_k >= n_rings) ring_k = n_rings - 1;

            float theta = atan2f(dy, dx);
            if (theta < 0.0f) theta += two_pi;
            float t_ang = theta / two_pi;
            size_t pos = (size_t)(t_ang * (float)bytes_per_ring);
            if (pos >= bytes_per_ring) pos = bytes_per_ring - 1;

            size_t bi = (size_t)ring_k * bytes_per_ring + pos;
            if (bi >= size) bi = size - 1;
            row[px] = byte_class_color(data[bi]);
        }
    }

    disk_outline(pixels, w, h, cx_f, cy_f, R, border);
    disk_center_marker(pixels, w, h, (int)cx_f, (int)cy_f, center);

    /* equal-area quartile rings (rings view uses t = (r/R)^2 too) */
    for (int q = 1; q <= 3; q++) {
        float frac = (float)q * 0.25f;
        size_t off = base_offset + (size_t)(frac * (double)size);
        draw_quartile_ring(pixels, w, h, cx_f, cy_f, R, sqrtf(frac),
                           off, file_size, border);
    }
    {
        char start_buf[40], end_buf[40];
        format_hex_offset(start_buf, sizeof(start_buf), base_offset, file_size);
        format_hex_offset(end_buf,   sizeof(end_buf),   base_offset + size, file_size);
        blit_text(pixels, w, h, (int)cx_f + 6, (int)cy_f + 6, 1,
                  OVERLAY_LABEL_COLOR, start_buf);
        int tw = text_width(1, end_buf);
        int lx = (int)(cx_f + R) - tw - 4;
        int ly = (int)cy_f + 4;
        if (lx + tw >= w) lx = w - tw - 2;
        if (ly + FONT_H >= h) ly = h - FONT_H - 1;
        blit_text(pixels, w, h, lx, ly, 1, OVERLAY_LABEL_COLOR, end_buf);
    }

    blit_text(pixels, w, h, 8, 4, 2, rgb(255, 200, 100),
              "CONCENTRIC RINGS (CENTER=BYTE 0)");
}

/* ---------- Circular Hilbert (Shirley-Chiu) ----------
 * Hilbert curve on an n x n grid, with the grid mapped onto the disk by the
 * Shirley-Chiu concentric (square -> disk) equal-area map. We invert the map
 * per disk pixel to look up which Hilbert cell we belong to.
 */

/* disk unit (a, b) with a^2+b^2 <= 1 -> square (u, v) in [-1, 1]^2 */
static void disk_to_square(float a, float b, float *u, float *v)
{
    float r = sqrtf(a * a + b * b);
    if (r < 1e-6f) { *u = 0.0f; *v = 0.0f; return; }
    float phi = atan2f(b, a);
    if (phi < -(float)M_PI / 4.0f) phi += 2.0f * (float)M_PI;

    const float k = 4.0f / (float)M_PI;
    if (phi < (float)M_PI / 4.0f) {                 /* right */
        *u = r;
        *v = phi * k * r;
    } else if (phi < 3.0f * (float)M_PI / 4.0f) {   /* top */
        *v = r;
        *u = ((float)M_PI / 2.0f - phi) * k * r;
    } else if (phi < 5.0f * (float)M_PI / 4.0f) {   /* left */
        *u = -r;
        *v = ((float)M_PI - phi) * k * r;
    } else {                                        /* bottom */
        *v = -r;
        *u = (phi - 3.0f * (float)M_PI / 2.0f) * k * r;
    }
}

void render_circular_hilbert(uint32_t *pixels, int w, int h,
                             const uint8_t *data, size_t size,
                             size_t base_offset, size_t file_size)
{
    const uint32_t bg     = rgb(8, 8, 14);
    const uint32_t border = rgb(80, 80, 110);
    fill(pixels, w, h, bg);
    if (size == 0 || w < 32 || h < 32) return;

    float cx_f = w * 0.5f;
    float cy_f = h * 0.5f;
    float R = (float)((w < h) ? w : h) * 0.5f - 14.0f;
    if (R < 8.0f) return;
    float R2 = R * R;

    int side = (int)R;
    int n = 1;
    while ((n * 2) <= side) n *= 2;
    if (n < 4) n = 4;
    if (n > 512) n = 512;

    uint64_t total_cells = (uint64_t)n * (uint64_t)n;
    uint64_t bpc = (size + total_cells - 1) / total_cells;
    if (bpc < 1) bpc = 1;

    uint32_t *cell_colors = (uint32_t *)malloc(total_cells * sizeof(uint32_t));
    if (!cell_colors) return;
    for (uint64_t d = 0; d < total_cells; d++) cell_colors[d] = bg;

    for (uint64_t d = 0; d < total_cells; d++) {
        int cx, cy;
        d2xy_hilbert(n, (int)d, &cx, &cy);
        uint64_t bs = d * bpc;
        if (bs >= size) continue;
        uint64_t be = bs + bpc;
        if (be > size) be = size;
        uint32_t sum = 0;
        uint32_t count = (uint32_t)(be - bs);
        for (uint64_t bb = bs; bb < be; bb++) sum += data[bb];
        uint8_t avg = (uint8_t)(sum / count);
        cell_colors[(uint64_t)cy * (uint64_t)n + (uint64_t)cx] = curve_color(avg);
    }

    for (int py = 0; py < h; py++) {
        float dy = (float)py - cy_f;
        uint32_t *row = pixels + (size_t)py * (size_t)w;
        for (int px = 0; px < w; px++) {
            float dx = (float)px - cx_f;
            float r2 = dx * dx + dy * dy;
            if (r2 > R2) continue;
            float a = dx / R, b = dy / R;
            float u, v;
            disk_to_square(a, b, &u, &v);
            int gx = (int)(((u + 1.0f) * 0.5f) * (float)n);
            int gy = (int)(((v + 1.0f) * 0.5f) * (float)n);
            if (gx < 0) gx = 0; else if (gx >= n) gx = n - 1;
            if (gy < 0) gy = 0; else if (gy >= n) gy = n - 1;
            row[px] = cell_colors[(size_t)gy * (size_t)n + (size_t)gx];
        }
    }
    free(cell_colors);

    disk_outline(pixels, w, h, cx_f, cy_f, R, border);

    /* start / end offset anchors (placement is for orientation, not precise) */
    char start_buf[40], end_buf[40];
    format_hex_offset(start_buf, sizeof(start_buf), base_offset, file_size);
    format_hex_offset(end_buf,   sizeof(end_buf),   base_offset + size, file_size);
    blit_text(pixels, w, h, (int)(cx_f - R) + 6, (int)(cy_f - R) + 6, 1,
              OVERLAY_LABEL_COLOR, start_buf);
    {
        int tw = text_width(1, end_buf);
        int lx = (int)(cx_f + R) - tw - 6;
        int ly = (int)(cy_f + R) - FONT_H - 6;
        if (lx < 0) lx = 0;
        if (ly < 0) ly = 0;
        if (lx + tw >= w) lx = w - tw - 2;
        if (ly + FONT_H >= h) ly = h - FONT_H - 1;
        blit_text(pixels, w, h, lx, ly, 1, OVERLAY_LABEL_COLOR, end_buf);
    }

    blit_text(pixels, w, h, 8, 4, 2, rgb(255, 200, 100),
              "CIRCULAR HILBERT (SHIRLEY-CHIU EQUAL-AREA)");
}

/* ---------- Autocorrelation strip ----------
 * Plot Pearson correlation between data and data[lag] for lag = 1..plot_w.
 * Spikes at multiples of a period reveal record sizes / block alignments.
 * Subsampled for large files; variance is computed once. */

void render_autocorrelation(uint32_t *pixels, int w, int h,
                            const uint8_t *data, size_t size,
                            size_t base_offset, size_t file_size)
{
    (void)base_offset; (void)file_size;
    const uint32_t bg     = rgb(10, 10, 16);
    const uint32_t grid   = rgb(35, 35, 45);
    const uint32_t axis   = rgb(110, 110, 130);
    const uint32_t tick   = rgb(60, 60, 80);
    const uint32_t pos    = rgb(255, 100, 100);
    const uint32_t neg    = rgb(100, 150, 255);
    fill(pixels, w, h, bg);
    if (size < 8 || w < 80 || h < 80) return;

    int margin_l = 50, margin_r = 16, margin_t = 22, margin_b = 28;
    int plot_w = w - margin_l - margin_r;
    int plot_h = h - margin_t - margin_b;
    if (plot_w < 16 || plot_h < 16) return;
    int mid_y = margin_t + plot_h / 2;

    int max_lag = plot_w;
    if ((size_t)max_lag > size / 2) max_lag = (int)(size / 2);
    if (max_lag < 4) return;

    double mean = 0;
    for (size_t i = 0; i < size; i++) mean += (double)data[i];
    mean /= (double)size;

    double var = 0;
    size_t var_stride = (size > 200000) ? (size / 200000) : 1;
    size_t var_n = 0;
    for (size_t i = 0; i < size; i += var_stride) {
        double d = (double)data[i] - mean;
        var += d * d;
        var_n++;
    }
    var = (var_n > 0) ? (var / (double)var_n) : 1.0;
    if (var < 1e-9) var = 1.0;

    /* horizontal grid (±0.25, ±0.5, ±0.75, ±1.0) */
    for (int g = -4; g <= 4; g++) {
        if (g == 0) continue;
        int gy = mid_y - g * (plot_h / 2) / 4;
        if (gy < margin_t || gy >= margin_t + plot_h) continue;
        for (int x = margin_l; x < margin_l + plot_w; x++) pixels[gy * w + x] = grid;
    }
    /* power-of-2 lag ticks */
    for (int lag = 16; lag <= max_lag; lag *= 2) {
        int x = margin_l + lag - 1;
        if (x >= margin_l + plot_w) break;
        for (int y = margin_t; y < margin_t + plot_h; y++) pixels[y * w + x] = tick;
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", lag);
        blit_text(pixels, w, h, x - text_width(2, buf) / 2,
                  margin_t + plot_h + 6, 2, axis, buf);
    }
    /* zero axis */
    for (int x = margin_l; x < margin_l + plot_w; x++) pixels[mid_y * w + x] = axis;
    for (int y = margin_t; y < margin_t + plot_h; y++) pixels[y * w + margin_l - 1] = axis;

    size_t max_pairs = 30000;

    for (int lag = 1; lag <= max_lag; lag++) {
        size_t span = size - (size_t)lag;
        size_t stride = (span > max_pairs) ? (span / max_pairs) : 1;
        double sum_prod = 0.0;
        size_t pairs = 0;
        for (size_t i = 0; i + (size_t)lag < size; i += stride) {
            double a = (double)data[i] - mean;
            double b = (double)data[i + lag] - mean;
            sum_prod += a * b;
            pairs++;
        }
        double corr = (pairs > 0) ? (sum_prod / (double)pairs / var) : 0.0;
        if (corr > 1.0) corr = 1.0;
        if (corr < -1.0) corr = -1.0;

        int x = margin_l + lag - 1;
        if (x >= margin_l + plot_w) break;
        int bar_h = (int)((corr < 0 ? -corr : corr) * (double)(plot_h / 2));
        if (corr > 0) {
            for (int y = mid_y - bar_h; y < mid_y; y++) pixels[y * w + x] = pos;
        } else if (corr < 0) {
            for (int y = mid_y; y < mid_y + bar_h; y++) pixels[y * w + x] = neg;
        }
    }

    blit_text(pixels, w, h, margin_l, 4, 2, rgb(255, 200, 100),
              "AUTOCORRELATION (LAG 1..N)");
    blit_text(pixels, w, h, 6, margin_t - 2, 2, axis, "+1");
    blit_text(pixels, w, h, 8, mid_y - FONT_H, 2, axis, "0");
    blit_text(pixels, w, h, 6, margin_t + plot_h - FONT_H * 2 - 2, 2, axis, "-1");
}

/* ---------- Strings density ----------
 * Per-chunk fraction of bytes that participate in a printable-ASCII run >= 4.
 * Reveals where strings cluster in firmware / binaries. */

static inline bool is_printable(uint8_t b) { return b >= 0x20 && b < 0x7F; }

void render_strings_density(uint32_t *pixels, int w, int h,
                            const uint8_t *data, size_t size,
                            size_t base_offset, size_t file_size)
{
    const uint32_t bg = rgb(8, 8, 14);
    fill(pixels, w, h, bg);
    if (size == 0) return;

    uint64_t total = (uint64_t)w * (uint64_t)h;
    size_t chunk = total ? size / total : 0;
    if (chunk < 64) chunk = 64;
    size_t n_chunks = size / chunk;
    if (n_chunks == 0) return;

    double *density = (double *)malloc(n_chunks * sizeof(double));
    if (!density) return;

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

    if (h >= 32) {
        draw_offset_rail_right(pixels, w, h, w - 2, 2, h - 2,
                               base_offset, size, file_size);
    }

    blit_text(pixels, w, h, 8, 4, 2, rgb(255, 200, 100),
              "STRINGS DENSITY (ASCII RUNS >= 4)");
}

/* ---------- Self-similarity matrix ----------
 * Divide the file into N chunks; render the N x N matrix of histogram
 * distances (L1 over 256-bin byte histograms). Bright = similar.
 * Repeated regions appear as off-diagonal stripes / blocks. */

void render_self_similarity(uint32_t *pixels, int w, int h,
                            const uint8_t *data, size_t size,
                            size_t base_offset, size_t file_size)
{
    const uint32_t bg = rgb(8, 8, 14);
    fill(pixels, w, h, bg);
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

    uint32_t (*hists)[256] = (uint32_t (*)[256])
                              calloc((size_t)N, sizeof(uint32_t[256]));
    if (!hists) return;
    for (int c = 0; c < N; c++) {
        const uint8_t *p = data + (size_t)c * chunk_size;
        for (size_t i = 0; i < chunk_size; i++) hists[c][p[i]]++;
    }

    uint32_t *dist = (uint32_t *)malloc((size_t)N * (size_t)N * sizeof(uint32_t));
    if (!dist) { free(hists); return; }
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

    int cell = side / N;
    if (cell < 1) cell = 1;
    int img = N * cell;
    int ox = (w - img) / 2;
    int oy = (h - img) / 2;

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
    free(hists);

    /* title + offset ticks on top (x axis) and left (y axis) */
    blit_text(pixels, w, h, ox, oy - FONT_H * 2 - 6, 2, rgb(255, 200, 100),
              "SELF-SIMILARITY (HISTOGRAM L1 DISTANCE)");

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

/* ---------- Markov / digraph chord diagram ----------
 * 256 byte values around a circle, byte-class colored ticks at the rim.
 * Quadratic Bezier arcs connect (a, b) pairs whose transition count
 * exceeds an adaptive threshold so we cap rendered curves to ~2k. */

void render_markov_chord(uint32_t *pixels, int w, int h,
                         const uint8_t *data, size_t size,
                         size_t base_offset, size_t file_size)
{
    (void)base_offset; (void)file_size;
    const uint32_t bg = rgb(5, 5, 12);
    fill(pixels, w, h, bg);
    if (size < 2 || w < 80 || h < 80) return;

    uint32_t *counts = (uint32_t *)calloc(256 * 256, sizeof(uint32_t));
    if (!counts) return;
    uint32_t max_c = 0;
    for (size_t i = 0; i + 1 < size; i++) {
        uint32_t idx = ((uint32_t)data[i] << 8) | data[i + 1];
        counts[idx]++;
        if (counts[idx] > max_c) max_c = counts[idx];
    }
    if (max_c == 0) { free(counts); return; }
    double log_max = log((double)max_c + 1.0);

    float cx = w * 0.5f;
    float cy = h * 0.5f;
    float R = (float)((w < h) ? w : h) * 0.5f - 26.0f;
    if (R < 12.0f) { free(counts); return; }

    float two_pi = 2.0f * (float)M_PI;
    float cos_t[256], sin_t[256];
    for (int i = 0; i < 256; i++) {
        float a = (float)i * (two_pi / 256.0f);
        cos_t[i] = cosf(a);
        sin_t[i] = sinf(a);
    }

    /* threshold to limit arc count */
    int max_arcs = 2000;
    uint32_t threshold = 1;
    int nz = 0;
    for (int i = 0; i < 256 * 256; i++) if (counts[i] > 0) nz++;
    if (nz > max_arcs) {
        for (uint32_t t = 1; t <= max_c; t *= 2) {
            int kept = 0;
            for (int i = 0; i < 256 * 256; i++) if (counts[i] >= t) kept++;
            if (kept <= max_arcs) { threshold = t; break; }
            if (t > max_c / 2) break;
        }
    }

    for (int idx = 0; idx < 256 * 256; idx++) {
        uint32_t c = counts[idx];
        if (c < threshold) continue;
        int a = (idx >> 8) & 0xFF;
        int b = idx & 0xFF;
        if (a == b) continue;

        double tn = log((double)c + 1.0) / log_max;
        uint32_t color = heat_color(tn * 0.85 + 0.1);

        float ax = cx + R * cos_t[a];
        float ay = cy + R * sin_t[a];
        float bx = cx + R * cos_t[b];
        float by = cy + R * sin_t[b];

        float midx = (ax + bx) * 0.5f;
        float midy = (ay + by) * 0.5f;
        float ccx = midx + (cx - midx) * 0.75f;
        float ccy = midy + (cy - midy) * 0.75f;

        const int steps = 28;
        float prev_x = ax, prev_y = ay;
        for (int s = 1; s <= steps; s++) {
            float u = (float)s / (float)steps;
            float omu = 1.0f - u;
            float px = omu * omu * ax + 2.0f * omu * u * ccx + u * u * bx;
            float py = omu * omu * ay + 2.0f * omu * u * ccy + u * u * by;
            draw_line_pix(pixels, w, h, prev_x, prev_y, px, py, color);
            prev_x = px; prev_y = py;
        }
    }

    /* rim ticks colored by byte class */
    for (int i = 0; i < 256; i++) {
        uint32_t col = byte_class_color((uint8_t)i);
        for (int t = 0; t < 5; t++) {
            int xx = (int)(cx + (R + (float)t) * cos_t[i]);
            int yy = (int)(cy + (R + (float)t) * sin_t[i]);
            if (xx < 0 || xx >= w || yy < 0 || yy >= h) continue;
            pixels[yy * w + xx] = col;
        }
    }
    free(counts);

    /* hex labels every 0x20 just outside the rim */
    static const int tick_vals[] = {0x00, 0x20, 0x40, 0x60, 0x80, 0xA0, 0xC0, 0xE0};
    int scale = 1;
    int th = FONT_H * scale;
    float R_label = R + 10.0f;
    for (size_t i = 0; i < sizeof(tick_vals) / sizeof(tick_vals[0]); i++) {
        int v = tick_vals[i];
        char buf2[8];
        snprintf(buf2, sizeof(buf2), "%02X", v);
        int tw = text_width(scale, buf2);
        int lx = (int)(cx + R_label * cos_t[v]) - tw / 2;
        int ly = (int)(cy + R_label * sin_t[v]) - th / 2;
        if (lx < 0) lx = 0;
        if (ly < 0) ly = 0;
        if (lx + tw >= w) lx = w - tw - 1;
        if (ly + th >= h) ly = h - th - 1;
        blit_text(pixels, w, h, lx, ly, scale, OVERLAY_LABEL_COLOR, buf2);
    }

    char buf[64];
    snprintf(buf, sizeof(buf),
             "MARKOV CHORD  ARCS=%d (THRESHOLD>=%u)", nz, threshold);
    blit_text(pixels, w, h, 8, 4, 2, rgb(255, 200, 100), buf);
}

/* ---------- RGB raw view ----------
 * Each pixel = one (R, G, B) triple of consecutive bytes.
 * Files containing embedded bitmaps (raw textures, dumps) literally show up. */

void render_rgb_raw(uint32_t *pixels, int w, int h, const uint8_t *data, size_t size,
                    size_t base_offset, size_t file_size)
{
    const uint32_t bg = rgb(8, 8, 14);
    fill(pixels, w, h, bg);
    if (size < 3) return;

    uint64_t total = (uint64_t)w * (uint64_t)h;
    size_t n_triples = size / 3;
    if (n_triples == 0) return;

    for (uint64_t i = 0; i < total; i++) {
        uint64_t ti = (i * n_triples) / total;
        if (ti >= n_triples) { pixels[i] = bg; continue; }
        size_t off = ti * 3;
        pixels[i] = rgb(data[off], data[off + 1], data[off + 2]);
    }

    if (h >= 32) {
        draw_offset_rail_right(pixels, w, h, w - 2, 2, h - 2,
                               base_offset, size, file_size);
    }

    blit_text(pixels, w, h, 8, 4, 2, rgb(255, 200, 100),
              "RGB RAW (3 BYTES PER PIXEL)");
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

/* ---------- Cylindrical ---------- */

void render_cylindrical(uint32_t *pixels, int w, int h, const uint8_t *data, size_t size,
                        size_t base_offset, size_t file_size,
                        float yaw, float pitch, float zoom)
{
    const uint32_t bg = rgb(6, 6, 14);
    fill(pixels, w, h, bg);
    if (size == 0 || w < 16 || h < 16) return;

    float cy = cosf(yaw),   sy = sinf(yaw);
    float cp = cosf(pitch), sp = sinf(pitch);
    float cam_d = 4.2f;
    float f = (float)((w < h) ? w : h) * 0.42f * zoom;
    float ox = w * 0.5f, oo = h * 0.5f;

    const int period = 256;
    float radius = 1.1f;
    float height = 3.2f;

    float cos_tbl[256], sin_tbl[256];
    for (int t = 0; t < period; t++) {
        float a = (float)t * (2.0f * (float)M_PI / (float)period);
        cos_tbl[t] = cosf(a);
        sin_tbl[t] = sinf(a);
    }

    /* dense wireframe: parallels (horizontal rings) + meridians (vertical lines)
     * drawn first so byte-class data lines paint on top. */
    const int n_parallels = 8;
    const int n_meridians = 24;
    uint32_t ring_edge_col = rgb(80, 80, 110);
    uint32_t ring_mid_col  = rgb(45, 45, 65);
    uint32_t meridian_col  = rgb(50, 50, 75);

    for (int ring_i = 0; ring_i < n_parallels; ring_i++) {
        float tr = (n_parallels > 1) ? (float)ring_i / (float)(n_parallels - 1) : 0.5f;
        float ry = (tr - 0.5f) * height;
        bool edge = (ring_i == 0 || ring_i == n_parallels - 1);
        uint32_t col = edge ? ring_edge_col : ring_mid_col;
        float prev_x = 0, prev_y = 0;
        bool have_prev = false;
        for (int t = 0; t <= period; t++) {
            int idx = t % period;
            float xx = cos_tbl[idx] * radius;
            float zz = sin_tbl[idx] * radius;
            float sxp, syp;
            if (!project_pt(xx, ry, zz, cy, sy, cp, sp, cam_d, f, ox, oo, &sxp, &syp)) {
                have_prev = false; continue;
            }
            if (have_prev) draw_line_pix(pixels, w, h, prev_x, prev_y, sxp, syp, col);
            prev_x = sxp; prev_y = syp; have_prev = true;
        }
    }
    for (int m = 0; m < n_meridians; m++) {
        int idx = (m * period) / n_meridians;
        float xx = cos_tbl[idx] * radius;
        float zz = sin_tbl[idx] * radius;
        float tx, ty, bx, by;
        if (!project_pt(xx, -height * 0.5f, zz, cy, sy, cp, sp, cam_d, f, ox, oo, &tx, &ty)) continue;
        if (!project_pt(xx,  height * 0.5f, zz, cy, sy, cp, sp, cam_d, f, ox, oo, &bx, &by)) continue;
        draw_line_pix(pixels, w, h, tx, ty, bx, by, meridian_col);
    }

    size_t step = 1;
    size_t cap = 400000;
    if (size > cap) step = size / cap;

    /* data trace: connect consecutive samples with byte-class-colored segments;
     * skip the chord that would jump across the cylinder when angle wraps. */
    float prev_sx = 0, prev_sy = 0;
    int prev_ti = -1;
    bool have_prev_pt = false;
    for (size_t i = 0; i < size; i += step) {
        int  ti = (int)(i & 0xFF);
        float yy = ((float)i / (float)size - 0.5f) * height;
        float xx = cos_tbl[ti] * radius;
        float zz = sin_tbl[ti] * radius;
        float sxp, syp;
        if (!project_pt(xx, yy, zz, cy, sy, cp, sp, cam_d, f, ox, oo, &sxp, &syp)) {
            have_prev_pt = false;
            continue;
        }
        uint32_t col = byte_class_color(data[i]);
        if (have_prev_pt && ti >= prev_ti) {
            draw_line_pix(pixels, w, h, prev_sx, prev_sy, sxp, syp, col);
        } else {
            int px = (int)sxp, py = (int)syp;
            if (px >= 0 && px < w && py >= 0 && py < h)
                pixels[py * w + px] = col;
        }
        prev_sx = sxp; prev_sy = syp; prev_ti = ti; have_prev_pt = true;
    }

    /* axial offset ladder: 5 ticks projected at (x=-radius, z=0) */
    for (int q = 0; q <= 4; q++) {
        float t = (float)q * 0.25f;
        float yy = (t - 0.5f) * height;
        float sxp, syp;
        if (!project_pt(-radius, yy, 0, cy, sy, cp, sp, cam_d, f, ox, oo, &sxp, &syp))
            continue;
        size_t off = base_offset + (size_t)((double)t * (double)size);
        if (q == 4) off = base_offset + size;
        char buf[40];
        format_hex_offset(buf, sizeof(buf), off, file_size);
        int scale = 1;
        int tw = text_width(scale, buf);
        int th = FONT_H * scale;
        int lx = (int)sxp - tw - 6;
        int ly = (int)syp - th / 2;
        if (lx < 2) lx = 2;
        if (ly < 0) ly = 0;
        if (ly + th >= h) ly = h - th - 1;
        draw_hline(pixels, w, h, lx + tw + 1, (int)sxp - 1, (int)syp, OVERLAY_TICK_COLOR);
        blit_text(pixels, w, h, lx, ly, scale, OVERLAY_LABEL_COLOR, buf);
    }
    /* angular reference marker: 0x00 / 0x80 on the top ring */
    {
        float ry = -height * 0.5f;
        float sxp, syp;
        if (project_pt(cos_tbl[0] * radius, ry, sin_tbl[0] * radius,
                       cy, sy, cp, sp, cam_d, f, ox, oo, &sxp, &syp)) {
            blit_text(pixels, w, h, (int)sxp + 4, (int)syp - FONT_H, 1,
                      OVERLAY_LABEL_COLOR, "0X00");
        }
        if (project_pt(cos_tbl[128] * radius, ry, sin_tbl[128] * radius,
                       cy, sy, cp, sp, cam_d, f, ox, oo, &sxp, &syp)) {
            blit_text(pixels, w, h, (int)sxp + 4, (int)syp - FONT_H, 1,
                      OVERLAY_LABEL_COLOR, "0X80");
        }
    }

    blit_text(pixels, w, h, 8, 4, 2, rgb(255, 200, 100),
              "CYLINDRICAL (WRAP=256B PER REVOLUTION)");
}

/* ---------- 3D trigraph point cloud ---------- */

void render_trigraph(uint32_t *pixels, int w, int h, const uint8_t *data, size_t size,
                     size_t base_offset, size_t file_size,
                     float yaw, float pitch, float zoom)
{
    (void)base_offset; (void)file_size;
    const uint32_t bg = rgb(4, 4, 10);
    fill(pixels, w, h, bg);
    if (size < 3 || w < 16 || h < 16) return;

    uint32_t *counts = (uint32_t *)calloc((size_t)w * (size_t)h, sizeof(uint32_t));
    if (!counts) return;

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
    uint32_t cube_col = rgb(60, 60, 90);
    for (int i = 0; i < 12; i++) {
        if (!ok[ce[i][0]] || !ok[ce[i][1]]) continue;
        draw_line_pix(pixels, w, h,
                      pv[ce[i][0]][0], pv[ce[i][0]][1],
                      pv[ce[i][1]][0], pv[ce[i][1]][1],
                      cube_col);
    }

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

    blit_text(pixels, w, h, 8, 4, 2, rgb(255, 200, 100),
              "TRIGRAPH (BYTE TRIPLES IN 0..255 CUBE)");
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
                               float yaw, float pitch, float zoom)
{
    (void)base_offset; (void)file_size;
    const uint32_t bg = rgb(4, 4, 10);
    fill(pixels, w, h, bg);
    if (size < 3 || w < 16 || h < 16) return;

    uint32_t *counts = (uint32_t *)calloc((size_t)w * (size_t)h, sizeof(uint32_t));
    if (!counts) return;

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

    blit_text(pixels, w, h, 8, 4, 2, rgb(255, 200, 100),
              "SPHERICAL TRIGRAPH (THETA=A PHI=B R=C)");
}

/* ---------- Helical (3D) ----------
 * Bytes laid along a 3D helix:
 *   theta(i) = i / pitch * 2pi    (continuous, not wrapped)
 *   y(i)     = (i/size - 0.5) * H
 *   x, z     = R * cos/sin(theta)
 * Unlike the cylinder (which wraps angle by i % period and overdraws), every
 * byte gets a unique 3D position. Vertical alignment of structures snaps when
 * pitch matches a record size.
 */

void render_helical(uint32_t *pixels, int w, int h, const uint8_t *data, size_t size,
                    size_t base_offset, size_t file_size,
                    float yaw, float pitch, float zoom)
{
    const uint32_t bg = rgb(6, 6, 14);
    fill(pixels, w, h, bg);
    if (size == 0 || w < 16 || h < 16) return;

    float cy = cosf(yaw),   sy = sinf(yaw);
    float cp = cosf(pitch), sp = sinf(pitch);
    float cam_d = 4.6f;
    float f = (float)((w < h) ? w : h) * 0.42f * zoom;
    float ox = w * 0.5f, oo = h * 0.5f;

    const float helix_pitch = 256.0f;
    const float radius = 1.0f;
    const float height = 3.4f;

    size_t step = 1;
    size_t cap = 400000;
    if (size > cap) step = size / cap;

    /* incremental rotation per sample step */
    float two_pi = 2.0f * (float)M_PI;
    float dtheta = (float)step * (two_pi / helix_pitch);
    float dc = cosf(dtheta), ds = sinf(dtheta);
    float c = 1.0f, s = 0.0f;     /* theta = 0 at i = 0 */

    float inv_size = 1.0f / (float)size;

    /* axis line as faint wireframe */
    uint32_t axis_col = rgb(60, 60, 90);
    float ax_top_x, ax_top_y, ax_bot_x, ax_bot_y;
    bool ok_t = project_pt(0, -height * 0.5f, 0, cy, sy, cp, sp, cam_d, f, ox, oo,
                           &ax_top_x, &ax_top_y);
    bool ok_b = project_pt(0,  height * 0.5f, 0, cy, sy, cp, sp, cam_d, f, ox, oo,
                           &ax_bot_x, &ax_bot_y);
    if (ok_t && ok_b)
        draw_line_pix(pixels, w, h, ax_top_x, ax_top_y, ax_bot_x, ax_bot_y, axis_col);

    /* dense wireframe of the implied cylinder: parallels + meridians.
     * The helix winds along this surface, so the lattice anchors it visually. */
    const int n_parallels = 8;
    const int n_meridians = 24;
    const int wf_segs = 96;
    uint32_t ring_edge_col = rgb(70, 70, 100);
    uint32_t ring_mid_col  = rgb(40, 40, 60);
    uint32_t meridian_col  = rgb(50, 50, 75);

    for (int ring_i = 0; ring_i < n_parallels; ring_i++) {
        float tr = (n_parallels > 1) ? (float)ring_i / (float)(n_parallels - 1) : 0.5f;
        float ry = (tr - 0.5f) * height;
        bool edge = (ring_i == 0 || ring_i == n_parallels - 1);
        uint32_t col = edge ? ring_edge_col : ring_mid_col;
        float prev_x = 0, prev_y = 0;
        bool have_prev = false;
        for (int ti = 0; ti <= wf_segs; ti++) {
            float a = (float)ti * (two_pi / (float)wf_segs);
            float sxp, syp;
            if (!project_pt(cosf(a) * radius, ry, sinf(a) * radius,
                            cy, sy, cp, sp, cam_d, f, ox, oo, &sxp, &syp)) {
                have_prev = false; continue;
            }
            if (have_prev) draw_line_pix(pixels, w, h, prev_x, prev_y, sxp, syp, col);
            prev_x = sxp; prev_y = syp; have_prev = true;
        }
    }
    for (int m = 0; m < n_meridians; m++) {
        float a = (float)m * (two_pi / (float)n_meridians);
        float mxx = cosf(a) * radius;
        float mzz = sinf(a) * radius;
        float tx, ty, bx, by;
        if (!project_pt(mxx, -height * 0.5f, mzz, cy, sy, cp, sp, cam_d, f, ox, oo, &tx, &ty)) continue;
        if (!project_pt(mxx,  height * 0.5f, mzz, cy, sy, cp, sp, cam_d, f, ox, oo, &bx, &by)) continue;
        draw_line_pix(pixels, w, h, tx, ty, bx, by, meridian_col);
    }

    /* data spiral: connect each sample to the previous with a byte-class colored
     * segment. The helix is continuous, so no wrap detection is needed. */
    float prev_sx = 0, prev_sy = 0;
    bool have_prev_pt = false;
    for (size_t i = 0; i < size; i += step) {
        float yy = ((float)i * inv_size - 0.5f) * height;
        float xx = c * radius;
        float zz = s * radius;
        float sxp, syp;
        bool ok = project_pt(xx, yy, zz, cy, sy, cp, sp, cam_d, f, ox, oo, &sxp, &syp);
        if (ok) {
            uint32_t col = byte_class_color(data[i]);
            if (have_prev_pt) {
                draw_line_pix(pixels, w, h, prev_sx, prev_sy, sxp, syp, col);
            } else {
                int px = (int)sxp, py = (int)syp;
                if (px >= 0 && px < w && py >= 0 && py < h)
                    pixels[py * w + px] = col;
            }
            prev_sx = sxp; prev_sy = syp; have_prev_pt = true;
        } else {
            have_prev_pt = false;
        }
        float nc = c * dc - s * ds;
        float ns = s * dc + c * ds;
        c = nc; s = ns;
    }

    /* axial offset ladder, 5 ticks projected at (x=-radius, z=0) */
    for (int q = 0; q <= 4; q++) {
        float t = (float)q * 0.25f;
        float yy = (t - 0.5f) * height;
        float sxp, syp;
        if (!project_pt(-radius, yy, 0, cy, sy, cp, sp, cam_d, f, ox, oo, &sxp, &syp))
            continue;
        size_t off = base_offset + (size_t)((double)t * (double)size);
        if (q == 4) off = base_offset + size;
        char buf[40];
        format_hex_offset(buf, sizeof(buf), off, file_size);
        int scale = 1;
        int tw = text_width(scale, buf);
        int th = FONT_H * scale;
        int lx = (int)sxp - tw - 6;
        int ly = (int)syp - th / 2;
        if (lx < 2) lx = 2;
        if (ly < 0) ly = 0;
        if (ly + th >= h) ly = h - th - 1;
        draw_hline(pixels, w, h, lx + tw + 1, (int)sxp - 1, (int)syp, OVERLAY_TICK_COLOR);
        blit_text(pixels, w, h, lx, ly, scale, OVERLAY_LABEL_COLOR, buf);
    }

    blit_text(pixels, w, h, 8, 4, 2, rgb(255, 200, 100),
              "HELICAL (PITCH=256B PER REV)");
}

/* ---------- Toroidal (3D) ----------
 * Two periodicities: outer angle u = (i % outer) / outer * 2pi,
 *                    inner angle v = ((i / outer) % inner) / inner * 2pi.
 * Reveals nested periodic structure (e.g. 16 B record inside 4 KB page).
 */

void render_torus(uint32_t *pixels, int w, int h, const uint8_t *data, size_t size,
                  size_t base_offset, size_t file_size,
                  float yaw, float pitch, float zoom)
{
    (void)base_offset; (void)file_size;
    const uint32_t bg = rgb(6, 6, 14);
    fill(pixels, w, h, bg);
    if (size == 0 || w < 16 || h < 16) return;

    float cy = cosf(yaw),   sy = sinf(yaw);
    float cp = cosf(pitch), sp = sinf(pitch);
    float cam_d = 4.6f;
    float f = (float)((w < h) ? w : h) * 0.42f * zoom;
    float ox = w * 0.5f, oo = h * 0.5f;

    const float R_major = 1.3f;
    const float R_minor = 0.5f;
    const int period_outer = 256;
    const int period_inner = 256;

    float two_pi = 2.0f * (float)M_PI;
    float cos_u[256], sin_u[256];
    for (int i = 0; i < period_outer; i++) {
        float a = (float)i * (two_pi / (float)period_outer);
        cos_u[i] = cosf(a); sin_u[i] = sinf(a);
    }
    float cos_v[256], sin_v[256];
    for (int i = 0; i < period_inner; i++) {
        float a = (float)i * (two_pi / (float)period_inner);
        cos_v[i] = cosf(a); sin_v[i] = sinf(a);
    }

    /* dense u x v wireframe lattice so the toroidal shape reads clearly. */
    uint32_t wire_edge_col = rgb(70, 70, 100);
    uint32_t wire_mid_col  = rgb(40, 40, 60);

    const int n_v_isolines = 8;    /* big rings at different tube heights */
    const int n_u_isolines = 16;   /* small cross-section circles around the donut */
    const int inner_segs   = 96;

    /* v-isolines: loop in u, at each fixed v_idx draw a big ring */
    for (int vi = 0; vi < n_v_isolines; vi++) {
        int v_idx = (vi * period_inner) / n_v_isolines;
        float cvv = cos_v[v_idx], svv = sin_v[v_idx];
        float ring_r = R_major + R_minor * cvv;
        float y_ring = R_minor * svv;
        uint32_t col = (v_idx == 0) ? wire_edge_col : wire_mid_col;
        float prev_x = 0, prev_y = 0;
        bool have_prev = false;
        for (int t = 0; t <= period_outer; t++) {
            int idx = t % period_outer;
            float xx = ring_r * cos_u[idx];
            float zz = ring_r * sin_u[idx];
            float sxp, syp;
            if (!project_pt(xx, y_ring, zz, cy, sy, cp, sp, cam_d, f, ox, oo, &sxp, &syp)) {
                have_prev = false; continue;
            }
            if (have_prev) draw_line_pix(pixels, w, h, prev_x, prev_y, sxp, syp, col);
            prev_x = sxp; prev_y = syp; have_prev = true;
        }
    }

    /* u-isolines: loop in v, at each fixed u_idx draw a small cross-section circle */
    for (int ui = 0; ui < n_u_isolines; ui++) {
        int u_idx = (ui * period_outer) / n_u_isolines;
        float cu = cos_u[u_idx], su = sin_u[u_idx];
        uint32_t col = (u_idx == 0) ? wire_edge_col : wire_mid_col;
        float prev_x = 0, prev_y = 0;
        bool have_prev = false;
        for (int t = 0; t <= inner_segs; t++) {
            float a = (float)t * (two_pi / (float)inner_segs);
            float ring_r = R_major + R_minor * cosf(a);
            float yy = R_minor * sinf(a);
            float xx = ring_r * cu;
            float zz = ring_r * su;
            float sxp, syp;
            if (!project_pt(xx, yy, zz, cy, sy, cp, sp, cam_d, f, ox, oo, &sxp, &syp)) {
                have_prev = false; continue;
            }
            if (have_prev) draw_line_pix(pixels, w, h, prev_x, prev_y, sxp, syp, col);
            prev_x = sxp; prev_y = syp; have_prev = true;
        }
    }

    size_t step = 1;
    size_t cap = 400000;
    if (size > cap) step = size / cap;

    /* data trace: connect consecutive samples with byte-class colored segments,
     * skipping chords across the donut on u or v wrap boundaries. */
    float prev_sx = 0, prev_sy = 0;
    int prev_ui = -1, prev_vi = -1;
    bool have_prev_pt = false;
    for (size_t i = 0; i < size; i += step) {
        int ui = (int)(i & 0xFF);                       /* i % 256       */
        int vi = (int)((i >> 8) & 0xFF);                /* (i / 256) % 256 */
        float cu = cos_u[ui], su = sin_u[ui];
        float cvv = cos_v[vi], svv = sin_v[vi];
        float ring_r = R_major + R_minor * cvv;
        float xx = ring_r * cu;
        float yy = R_minor * svv;
        float zz = ring_r * su;
        float sxp, syp;
        if (!project_pt(xx, yy, zz, cy, sy, cp, sp, cam_d, f, ox, oo, &sxp, &syp)) {
            have_prev_pt = false;
            continue;
        }
        uint32_t col = byte_class_color(data[i]);
        bool wrapped = (vi != prev_vi) || (ui < prev_ui);
        if (have_prev_pt && !wrapped) {
            draw_line_pix(pixels, w, h, prev_sx, prev_sy, sxp, syp, col);
        } else {
            int px = (int)sxp, py = (int)syp;
            if (px >= 0 && px < w && py >= 0 && py < h)
                pixels[py * w + px] = col;
        }
        prev_sx = sxp; prev_sy = syp;
        prev_ui = ui; prev_vi = vi;
        have_prev_pt = true;
    }

    /* u/v anchor labels at strategic points on the two wireframes */
    {
        struct { float x, y, z; const char *label; } anchors[] = {
            { R_major + R_minor, 0.0f,  0.0f,            "U=00" },
            {-R_major - R_minor, 0.0f,  0.0f,            "U=80" },
            { R_major,           R_minor, 0.0f,          "V=40" },
            { R_major,          -R_minor, 0.0f,          "V=C0" },
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

    blit_text(pixels, w, h, 8, 4, 2, rgb(255, 200, 100),
              "TOROIDAL (256x256 BYTES PER FULL WRAP)");
}
