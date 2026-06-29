#include "binmap.h"
#include "font.h"

#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

const char *view_names[VIEW_COUNT] = {
    "BYTE CLASS STRIPE",
    "HILBERT CURVE",
    "DIGRAPH DOT PLOT",
    "ENTROPY HEATMAP",
    "STRINGS DENSITY",
    "SELF-SIMILARITY",
    "BIT-PLANE VIEW",
    "RGB RAW",
    "TRIGRAPH 3D",
    "SPHERICAL TRIGRAPH 3D",
};

bool view_is_3d(view_id_t v)
{
    return v == VIEW_TRIGRAPH
        || v == VIEW_TRIGRAPH_SPHERICAL;
}

static const struct {
    const char *key;
    const char *desc;
} ctrl_keys[] = {
    {"TAB",     "NEXT VIEW"},
    {"SHF-TAB", "PREV VIEW"},
    {"L",       "TOGGLE LEGEND"},
    {"A",       "AUTO-ROTATE (3D)"},
    {"ARROWS",  "ROTATE (3D)"},
    {"+/-/WHL", "ZOOM (3D)"},
    {"R",       "RESET 3D VIEW"},
    {"[ / ]",   "RANGE START -/+"},
    {", / .",   "RANGE END -/+"},
    {"SHF+KEY", "COARSE STEP"},
    {"0",       "RESET RANGE"},
    {"DRAG",    "RANGE SLIDER"},
    {"ESC",     "QUIT"},
};
#define CTRL_KEYS_COUNT (sizeof(ctrl_keys) / sizeof(ctrl_keys[0]))

#define STATUS_BAR_H 30
#define SLIDER_BAR_H 44
#define MIN_RANGE_BYTES 256
#define HANDLE_GRAB_PX  8
#define TRACK_MARGIN_X  12

static bool load_file(binmap_file_t *f, const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("open"); return false; }
    struct stat st;
    if (fstat(fd, &st) < 0) { perror("fstat"); close(fd); return false; }
    if (st.st_size <= 0) {
        fprintf(stderr, "binmap: '%s' is empty\n", path);
        close(fd); return false;
    }
    void *mem = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (mem == MAP_FAILED) { perror("mmap"); return false; }
    f->data = (const uint8_t *)mem;
    f->size = (size_t)st.st_size;
    f->path = path;
    return true;
}

static void invalidate_caches(binmap_app_t *app)
{
    for (int i = 0; i < VIEW_COUNT; i++) {
        if (app->cache[i]) { SDL_DestroyTexture(app->cache[i]); app->cache[i] = NULL; }
    }
    app->cache_w = app->cache_h = 0;
}

static void invalidate_3d_caches(binmap_app_t *app)
{
    for (int i = 0; i < VIEW_COUNT; i++) {
        if (view_is_3d((view_id_t)i) && app->cache[i]) {
            SDL_DestroyTexture(app->cache[i]);
            app->cache[i] = NULL;
        }
    }
}

static SDL_Texture *ensure_view_texture(binmap_app_t *app, view_id_t view, int cw, int ch)
{
    if (cw < 1 || ch < 1) return NULL;
    if (app->cache_w != cw || app->cache_h != ch) invalidate_caches(app);

    bool is_3d = view_is_3d(view);
    if (!is_3d && app->cache[view]) return app->cache[view];

    uint32_t *pixels = (uint32_t *)calloc((size_t)cw * (size_t)ch, sizeof(uint32_t));
    if (!pixels) return NULL;

    const uint8_t *d = app->file.data + app->range_start;
    size_t         s = app->range_end - app->range_start;
    size_t         bo = app->range_start;
    size_t         fs = app->file.size;

    switch (view) {
    case VIEW_BYTE_CLASS:  render_byte_class(pixels, cw, ch, d, s, bo, fs); break;
    case VIEW_HILBERT:     render_hilbert   (pixels, cw, ch, d, s, bo, fs); break;
    case VIEW_DIGRAPH:     render_digraph   (pixels, cw, ch, d, s, bo, fs); break;
    case VIEW_ENTROPY:     render_entropy   (pixels, cw, ch, d, s, bo, fs); break;
    case VIEW_STRINGS_DENSITY:
        render_strings_density(pixels, cw, ch, d, s, bo, fs); break;
    case VIEW_SELF_SIMILARITY:
        render_self_similarity(pixels, cw, ch, d, s, bo, fs); break;
    case VIEW_BIT_PLANE:   render_bit_plane (pixels, cw, ch, d, s, bo, fs); break;
    case VIEW_RGB_RAW:     render_rgb_raw   (pixels, cw, ch, d, s, bo, fs); break;
    case VIEW_TRIGRAPH:    render_trigraph  (pixels, cw, ch, d, s, bo, fs,
                                             app->yaw, app->pitch, app->zoom); break;
    case VIEW_TRIGRAPH_SPHERICAL:
        render_trigraph_spherical(pixels, cw, ch, d, s, bo, fs,
                                  app->yaw, app->pitch, app->zoom); break;
    default: free(pixels); return NULL;
    }

    if (!app->cache[view]) {
        app->cache[view] = SDL_CreateTexture(app->renderer,
                                             SDL_PIXELFORMAT_ARGB8888,
                                             SDL_TEXTUREACCESS_STATIC,
                                             cw, ch);
        if (!app->cache[view]) { free(pixels); return NULL; }
    }
    SDL_UpdateTexture(app->cache[view], NULL, pixels, cw * (int)sizeof(uint32_t));
    free(pixels);
    app->cache_w = cw;
    app->cache_h = ch;
    return app->cache[view];
}

static void format_size(char *buf, size_t bufsz, size_t s);

/* ---------- range slider ---------- */

static int slider_track_x(const binmap_app_t *app) { (void)app; return TRACK_MARGIN_X; }
static int slider_track_w(const binmap_app_t *app)
{
    int w = app->win_w - 2 * TRACK_MARGIN_X;
    return w < 1 ? 1 : w;
}
static int slider_track_h(void) { return 14; }
static int slider_track_y(const binmap_app_t *app)
{
    return app->win_h - SLIDER_BAR_H + 20;
}

static int byte_to_px(const binmap_app_t *app, size_t b)
{
    if (app->file.size == 0) return slider_track_x(app);
    double t = (double)b / (double)app->file.size;
    if (t < 0) t = 0; else if (t > 1) t = 1;
    return slider_track_x(app) + (int)(t * slider_track_w(app) + 0.5);
}

static size_t px_to_byte(const binmap_app_t *app, int x)
{
    int tx = slider_track_x(app);
    int tw = slider_track_w(app);
    if (x <= tx) return 0;
    if (x >= tx + tw) return app->file.size;
    double t = (double)(x - tx) / (double)tw;
    size_t b = (size_t)(t * (double)app->file.size + 0.5);
    if (b > app->file.size) b = app->file.size;
    return b;
}

static SDL_Texture *ensure_minimap_texture(binmap_app_t *app, int w)
{
    if (w < 1) return NULL;
    if (app->minimap_tex && app->minimap_w == w) return app->minimap_tex;
    if (app->minimap_tex) { SDL_DestroyTexture(app->minimap_tex); app->minimap_tex = NULL; }
    uint32_t *row = (uint32_t *)calloc((size_t)w, sizeof(uint32_t));
    if (!row) return NULL;
    render_byte_class(row, w, 1, app->file.data, app->file.size, 0, app->file.size);
    app->minimap_tex = SDL_CreateTexture(app->renderer,
                                         SDL_PIXELFORMAT_ARGB8888,
                                         SDL_TEXTUREACCESS_STATIC, w, 1);
    if (!app->minimap_tex) { free(row); return NULL; }
    SDL_UpdateTexture(app->minimap_tex, NULL, row, w * (int)sizeof(uint32_t));
    free(row);
    app->minimap_w = w;
    return app->minimap_tex;
}

static void format_offset(char *buf, size_t bufsz, size_t off)
{
    snprintf(buf, bufsz, "0x%zX", off);
}

static void draw_range_slider(binmap_app_t *app)
{
    int strip_y = app->win_h - SLIDER_BAR_H;

    SDL_Rect strip = {0, strip_y, app->win_w, SLIDER_BAR_H};
    SDL_SetRenderDrawColor(app->renderer, 22, 22, 28, 255);
    SDL_RenderFillRect(app->renderer, &strip);
    SDL_Rect sep = {0, strip_y, app->win_w, 1};
    SDL_SetRenderDrawColor(app->renderer, 70, 70, 90, 255);
    SDL_RenderFillRect(app->renderer, &sep);

    int tx = slider_track_x(app);
    int tw = slider_track_w(app);
    int ty = slider_track_y(app);
    int th = slider_track_h();

    SDL_Texture *mm = ensure_minimap_texture(app, tw);
    if (mm) {
        SDL_Rect dst = {tx, ty, tw, th};
        SDL_RenderCopy(app->renderer, mm, NULL, &dst);
    } else {
        SDL_Rect dst = {tx, ty, tw, th};
        SDL_SetRenderDrawColor(app->renderer, 40, 40, 50, 255);
        SDL_RenderFillRect(app->renderer, &dst);
    }

    int sx = byte_to_px(app, app->range_start);
    int ex = byte_to_px(app, app->range_end);
    if (ex < sx) ex = sx;

    SDL_SetRenderDrawBlendMode(app->renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(app->renderer, 0, 0, 0, 160);
    if (sx > tx) {
        SDL_Rect lhs = {tx, ty, sx - tx, th};
        SDL_RenderFillRect(app->renderer, &lhs);
    }
    if (ex < tx + tw) {
        SDL_Rect rhs = {ex, ty, tx + tw - ex, th};
        SDL_RenderFillRect(app->renderer, &rhs);
    }

    SDL_Rect border = {tx, ty, tw, th};
    SDL_SetRenderDrawColor(app->renderer, 90, 90, 115, 255);
    SDL_RenderDrawRect(app->renderer, &border);

    int handle_w = 6;
    int handle_h = th + 8;
    int handle_y = ty - 4;
    SDL_Color start_col = (app->drag_mode == DRAG_START)
                          ? (SDL_Color){255, 240, 160, 255}
                          : (SDL_Color){255, 200, 100, 255};
    SDL_Color end_col   = (app->drag_mode == DRAG_END)
                          ? (SDL_Color){255, 240, 160, 255}
                          : (SDL_Color){255, 200, 100, 255};
    SDL_Rect hs = {sx - handle_w/2, handle_y, handle_w, handle_h};
    SDL_Rect he = {ex - handle_w/2, handle_y, handle_w, handle_h};
    SDL_SetRenderDrawColor(app->renderer, start_col.r, start_col.g, start_col.b, 255);
    SDL_RenderFillRect(app->renderer, &hs);
    SDL_SetRenderDrawColor(app->renderer, end_col.r, end_col.g, end_col.b, 255);
    SDL_RenderFillRect(app->renderer, &he);
    SDL_SetRenderDrawColor(app->renderer, 20, 20, 25, 255);
    SDL_RenderDrawRect(app->renderer, &hs);
    SDL_RenderDrawRect(app->renderer, &he);

    int label_y = strip_y + 4;
    int scale = 2;
    SDL_Color label_col = {200, 200, 215, 255};
    SDL_Color dim_col   = {150, 150, 165, 255};

    char lstart[64], lend[64], lmid[96];
    char sstart[32], send[32], slen[32];
    format_offset(sstart, sizeof(sstart), app->range_start);
    format_offset(send,   sizeof(send),   app->range_end);
    format_size(slen, sizeof(slen), app->range_end - app->range_start);
    snprintf(lstart, sizeof(lstart), "START %s", sstart);
    snprintf(lend,   sizeof(lend),   "END %s",   send);
    double pct = app->file.size
                 ? 100.0 * (double)(app->range_end - app->range_start) / (double)app->file.size
                 : 0.0;
    snprintf(lmid, sizeof(lmid), "LEN %s (%.1f%%)", slen, pct);

    draw_text(app->renderer, tx, label_y, scale, label_col, lstart);
    int mw = text_width(scale, lmid);
    int mx_text = tx + (tw - mw) / 2;
    if (mx_text < tx) mx_text = tx;
    draw_text(app->renderer, mx_text, label_y, scale, dim_col, lmid);
    int rw = text_width(scale, lend);
    int rxp = tx + tw - rw;
    if (rxp < mx_text + mw + 10) rxp = mx_text + mw + 10;
    draw_text(app->renderer, rxp, label_y, scale, label_col, lend);
}

static void format_size(char *buf, size_t bufsz, size_t s)
{
    if (s < 1024)                  snprintf(buf, bufsz, "%zu B",   s);
    else if (s < 1024UL*1024)      snprintf(buf, bufsz, "%.1f KB", s / 1024.0);
    else if (s < 1024UL*1024*1024) snprintf(buf, bufsz, "%.1f MB", s / (1024.0*1024.0));
    else                           snprintf(buf, bufsz, "%.2f GB", s / (1024.0*1024.0*1024.0));
}

static const char *basename_of(const char *p)
{
    const char *b = strrchr(p, '/');
    return b ? b + 1 : p;
}

static void draw_status_bar(binmap_app_t *app)
{
    SDL_Rect bar = {0, 0, app->win_w, STATUS_BAR_H};
    SDL_SetRenderDrawColor(app->renderer, 22, 22, 28, 255);
    SDL_RenderFillRect(app->renderer, &bar);
    SDL_Rect sep = {0, STATUS_BAR_H - 1, app->win_w, 1};
    SDL_SetRenderDrawColor(app->renderer, 70, 70, 90, 255);
    SDL_RenderFillRect(app->renderer, &sep);

    char sz[32];
    format_size(sz, sizeof(sz), app->file.size);

    char left[256];
    snprintf(left, sizeof(left), "[%d/%d] %s",
             app->current_view + 1, VIEW_COUNT,
             view_names[app->current_view]);
    SDL_Color title_color = {255, 200, 100, 255};
    draw_text(app->renderer, 10, 8, 2, title_color, left);

    char range_info[64] = "";
    if (app->range_start != 0 || app->range_end != app->file.size) {
        snprintf(range_info, sizeof(range_info),
                 "  [0x%zX..0x%zX]", app->range_start, app->range_end);
    }

    char right[512];
    if (view_is_3d(app->current_view)) {
        snprintf(right, sizeof(right),
                 "%s (%s)%s  YAW %.0f  PITCH %.0f  ZOOM %.2fX%s",
                 basename_of(app->file.path), sz, range_info,
                 app->yaw   * 180.0 / M_PI,
                 app->pitch * 180.0 / M_PI,
                 app->zoom,
                 app->auto_rotate ? "  AUTO" : "");
    } else {
        snprintf(right, sizeof(right), "%s (%s)%s",
                 basename_of(app->file.path), sz, range_info);
    }
    SDL_Color text_color = {200, 200, 215, 255};
    int rw = text_width(2, right);
    int rx = app->win_w - rw - 10;
    if (rx < text_width(2, left) + 30) rx = text_width(2, left) + 30;
    draw_text(app->renderer, rx, 8, 2, text_color, right);
}

static void draw_legend(binmap_app_t *app)
{
    if (!app->show_legend) return;
    int scale = 2;
    int line_h = (FONT_H + 3) * scale;
    int header_h = line_h + 4;
    int section_gap = line_h;
    int pad = 10;
    int marker_w = text_width(scale, ">") + scale * 2;

    /* widths */
    int view_w = 0;
    for (int i = 0; i < VIEW_COUNT; i++) {
        int dw = text_width(scale, view_names[i]);
        if (dw > view_w) view_w = dw;
    }
    int ctrl_key_w = 0, ctrl_desc_w = 0;
    for (size_t i = 0; i < CTRL_KEYS_COUNT; i++) {
        int kw = text_width(scale, ctrl_keys[i].key);
        int dw = text_width(scale, ctrl_keys[i].desc);
        if (kw > ctrl_key_w)  ctrl_key_w  = kw;
        if (dw > ctrl_desc_w) ctrl_desc_w = dw;
    }

    int col_gap = 28;
    int key_desc_gap = 10;
    int col1_w = marker_w + view_w;
    int col2_w = ctrl_key_w + key_desc_gap + ctrl_desc_w;
    int content_w = col1_w + col_gap + col2_w;
    int header_w = text_width(scale, "VIEWS  (TAB CYCLES)");
    if (header_w > content_w) content_w = header_w;

    int views_rows = VIEW_COUNT;
    int ctrl_rows  = (int)CTRL_KEYS_COUNT;
    int rows = (views_rows > ctrl_rows) ? views_rows : ctrl_rows;

    int legend_w = content_w + pad * 2;
    int legend_h = header_h + rows * line_h + pad + section_gap;

    int x0 = app->win_w - legend_w - 12;
    int y0 = app->win_h - legend_h - SLIDER_BAR_H - 12;
    if (x0 < 0) x0 = 0;
    if (y0 < STATUS_BAR_H) y0 = STATUS_BAR_H;

    SDL_SetRenderDrawBlendMode(app->renderer, SDL_BLENDMODE_BLEND);
    SDL_Rect bg = {x0, y0, legend_w, legend_h};
    SDL_SetRenderDrawColor(app->renderer, 18, 18, 24, 225);
    SDL_RenderFillRect(app->renderer, &bg);
    SDL_SetRenderDrawColor(app->renderer, 90, 90, 115, 255);
    SDL_RenderDrawRect(app->renderer, &bg);

    SDL_Color hdr      = {255, 200, 100, 255};
    SDL_Color key_col  = {120, 220, 255, 255};
    SDL_Color desc_col = {220, 220, 230, 255};
    SDL_Color dim_col  = {150, 150, 165, 255};
    SDL_Color sel      = {255, 240, 160, 255};

    int col1_x = x0 + pad;
    int col2_x = x0 + pad + col1_w + col_gap;
    int y = y0 + pad / 2;

    /* column headers */
    draw_text(app->renderer, col1_x, y, scale, hdr, "VIEWS  (TAB CYCLES)");
    draw_text(app->renderer, col2_x, y, scale, hdr, "CONTROLS");
    SDL_Rect ul = {x0 + pad, y + FONT_H * scale + 2, content_w, 1};
    SDL_SetRenderDrawColor(app->renderer, 90, 90, 115, 255);
    SDL_RenderFillRect(app->renderer, &ul);
    y += header_h;

    for (int i = 0; i < rows; i++) {
        int ry = y + i * line_h;
        if (i < views_rows) {
            bool active = (i == (int)app->current_view);
            if (active) {
                draw_text(app->renderer, col1_x, ry, scale, sel, ">");
                draw_text(app->renderer, col1_x + marker_w, ry, scale, sel, view_names[i]);
            } else {
                draw_text(app->renderer, col1_x + marker_w, ry, scale, dim_col, view_names[i]);
            }
        }
        if (i < ctrl_rows) {
            draw_text(app->renderer, col2_x, ry, scale, key_col, ctrl_keys[i].key);
            draw_text(app->renderer, col2_x + ctrl_key_w + key_desc_gap, ry, scale,
                      desc_col, ctrl_keys[i].desc);
        }
    }
}

static void redraw(binmap_app_t *app)
{
    SDL_SetRenderDrawColor(app->renderer, 10, 10, 12, 255);
    SDL_RenderClear(app->renderer);

    int cw = app->win_w;
    int ch = app->win_h - STATUS_BAR_H - SLIDER_BAR_H;
    if (cw > 0 && ch > 0) {
        SDL_Texture *tex = ensure_view_texture(app, app->current_view, cw, ch);
        if (tex) {
            SDL_Rect dst = {0, STATUS_BAR_H, cw, ch};
            SDL_RenderCopy(app->renderer, tex, NULL, &dst);
        }
    }

    draw_status_bar(app);
    draw_legend(app);
    draw_range_slider(app);
    SDL_RenderPresent(app->renderer);
}

static void switch_view(binmap_app_t *app, view_id_t v)
{
    app->current_view = v;
    app->needs_redraw = true;
}

static void rotate_3d(binmap_app_t *app, float dyaw, float dpitch)
{
    if (!view_is_3d(app->current_view)) return;
    app->yaw   += dyaw;
    app->pitch += dpitch;
    if (app->pitch > 1.5f)  app->pitch = 1.5f;
    if (app->pitch < -1.5f) app->pitch = -1.5f;
    while (app->yaw >  (float)(2 * M_PI)) app->yaw -= (float)(2 * M_PI);
    while (app->yaw < -(float)(2 * M_PI)) app->yaw += (float)(2 * M_PI);
    invalidate_3d_caches(app);
    app->needs_redraw = true;
}

#define ZOOM_MIN 0.25f
#define ZOOM_MAX 12.0f
#define ZOOM_STEP 1.15f

static void zoom_3d(binmap_app_t *app, float factor)
{
    if (!view_is_3d(app->current_view)) return;
    app->zoom *= factor;
    if (app->zoom < ZOOM_MIN) app->zoom = ZOOM_MIN;
    if (app->zoom > ZOOM_MAX) app->zoom = ZOOM_MAX;
    invalidate_3d_caches(app);
    app->needs_redraw = true;
}

static size_t range_min_width(const binmap_app_t *app)
{
    return app->file.size < MIN_RANGE_BYTES ? app->file.size : MIN_RANGE_BYTES;
}

static void set_range(binmap_app_t *app, size_t new_start, size_t new_end)
{
    size_t minw = range_min_width(app);
    if (new_end > app->file.size) new_end = app->file.size;
    if (new_start + minw > new_end) {
        if (new_end >= minw) new_start = new_end - minw;
        else                 new_start = 0;
    }
    if (new_start == app->range_start && new_end == app->range_end) return;
    app->range_start = new_start;
    app->range_end   = new_end;
    invalidate_caches(app);
    app->needs_redraw = true;
}

static void nudge_range_start(binmap_app_t *app, long delta)
{
    long s = (long)app->range_start + delta;
    if (s < 0) s = 0;
    size_t ns = (size_t)s;
    size_t minw = range_min_width(app);
    if (ns + minw > app->range_end) {
        if (app->range_end >= minw) ns = app->range_end - minw; else ns = 0;
    }
    set_range(app, ns, app->range_end);
}

static void nudge_range_end(binmap_app_t *app, long delta)
{
    long e = (long)app->range_end + delta;
    if (e < 0) e = 0;
    size_t ne = (size_t)e;
    if (ne > app->file.size) ne = app->file.size;
    size_t minw = range_min_width(app);
    if (app->range_start + minw > ne) ne = app->range_start + minw;
    if (ne > app->file.size) ne = app->file.size;
    set_range(app, app->range_start, ne);
}

static long range_step(const binmap_app_t *app, bool coarse)
{
    if (app->file.size == 0) return 1;
    long step = coarse ? (long)(app->file.size / 100) : (long)(app->file.size / 1000);
    if (step < 1) step = 1;
    return step;
}

static void reset_view(binmap_app_t *app)
{
    app->yaw = 0.6f;
    app->pitch = 0.35f;
    app->zoom = 1.0f;
    app->auto_rotate = true;
    invalidate_3d_caches(app);
    app->needs_redraw = true;
}

static bool in_slider_strip(const binmap_app_t *app, int y)
{
    return y >= app->win_h - SLIDER_BAR_H && y < app->win_h;
}

static void handle_mouse_down(binmap_app_t *app, int mx, int my)
{
    if (!in_slider_strip(app, my)) return;
    int sx = byte_to_px(app, app->range_start);
    int ex = byte_to_px(app, app->range_end);
    size_t minw = range_min_width(app);

    if (abs(mx - sx) <= HANDLE_GRAB_PX) {
        app->drag_mode = DRAG_START;
    } else if (abs(mx - ex) <= HANDLE_GRAB_PX) {
        app->drag_mode = DRAG_END;
    } else if (mx > sx && mx < ex) {
        app->drag_mode = DRAG_MIDDLE;
    } else {
        size_t target = px_to_byte(app, mx);
        if (mx < sx) {
            size_t ns = target;
            if (ns + minw > app->range_end) {
                if (app->range_end >= minw) ns = app->range_end - minw; else ns = 0;
            }
            app->range_start = ns;
            app->drag_mode = DRAG_START;
        } else {
            size_t ne = target;
            if (ne > app->file.size) ne = app->file.size;
            if (app->range_start + minw > ne) ne = app->range_start + minw;
            if (ne > app->file.size) ne = app->file.size;
            app->range_end = ne;
            app->drag_mode = DRAG_END;
        }
    }
    app->drag_anchor_start = app->range_start;
    app->drag_anchor_end   = app->range_end;
    app->drag_anchor_px    = mx;
    app->needs_redraw = true;
}

static void handle_mouse_motion(binmap_app_t *app, int mx)
{
    if (app->drag_mode == DRAG_NONE) return;
    size_t minw = range_min_width(app);

    if (app->drag_mode == DRAG_START) {
        size_t ns = px_to_byte(app, mx);
        if (ns + minw > app->range_end) {
            if (app->range_end >= minw) ns = app->range_end - minw; else ns = 0;
        }
        if (ns != app->range_start) { app->range_start = ns; app->needs_redraw = true; }
    } else if (app->drag_mode == DRAG_END) {
        size_t ne = px_to_byte(app, mx);
        if (ne > app->file.size) ne = app->file.size;
        if (app->range_start + minw > ne) ne = app->range_start + minw;
        if (ne > app->file.size) ne = app->file.size;
        if (ne != app->range_end) { app->range_end = ne; app->needs_redraw = true; }
    } else if (app->drag_mode == DRAG_MIDDLE) {
        int tw = slider_track_w(app);
        if (tw < 1 || app->file.size == 0) return;
        double dpx = (double)(mx - app->drag_anchor_px);
        long   dbytes = (long)(dpx / (double)tw * (double)app->file.size);
        long   width = (long)(app->drag_anchor_end - app->drag_anchor_start);
        long   ns = (long)app->drag_anchor_start + dbytes;
        if (ns < 0) ns = 0;
        if (ns + width > (long)app->file.size) ns = (long)app->file.size - width;
        if (ns < 0) ns = 0;
        size_t nstart = (size_t)ns;
        size_t nend   = nstart + (size_t)width;
        if (nstart != app->range_start || nend != app->range_end) {
            app->range_start = nstart;
            app->range_end   = nend;
            app->needs_redraw = true;
        }
    }
}

static void handle_mouse_up(binmap_app_t *app)
{
    if (app->drag_mode == DRAG_NONE) return;
    bool changed = (app->range_start != app->drag_anchor_start)
                || (app->range_end   != app->drag_anchor_end);
    app->drag_mode = DRAG_NONE;
    if (changed) {
        invalidate_caches(app);
    }
    app->needs_redraw = true;
}

static void handle_key(binmap_app_t *app, SDL_Keycode k, SDL_Keymod mod, bool *running)
{
    switch (k) {
    case SDLK_ESCAPE:
    case SDLK_q: *running = false; break;
    case SDLK_TAB: {
        int n = VIEW_COUNT;
        int delta = (mod & KMOD_SHIFT) ? -1 : 1;
        int next = ((int)app->current_view + delta + n) % n;
        switch_view(app, (view_id_t)next);
        break;
    }
    case SDLK_l:
        app->show_legend = !app->show_legend;
        app->needs_redraw = true;
        break;
    case SDLK_a:
        if (view_is_3d(app->current_view)) {
            app->auto_rotate = !app->auto_rotate;
            app->needs_redraw = true;
        }
        break;
    case SDLK_r: reset_view(app); break;
    case SDLK_LEFT:  rotate_3d(app, -0.08f, 0); break;
    case SDLK_RIGHT: rotate_3d(app, +0.08f, 0); break;
    case SDLK_UP:    rotate_3d(app, 0, -0.08f); break;
    case SDLK_DOWN:  rotate_3d(app, 0, +0.08f); break;
    case SDLK_EQUALS:
    case SDLK_PLUS:
    case SDLK_KP_PLUS:  zoom_3d(app, ZOOM_STEP); break;
    case SDLK_MINUS:
    case SDLK_KP_MINUS: zoom_3d(app, 1.0f / ZOOM_STEP); break;
    case SDLK_LEFTBRACKET:  nudge_range_start(app, -range_step(app, mod & KMOD_SHIFT)); break;
    case SDLK_RIGHTBRACKET: nudge_range_start(app, +range_step(app, mod & KMOD_SHIFT)); break;
    case SDLK_COMMA:        nudge_range_end  (app, -range_step(app, mod & KMOD_SHIFT)); break;
    case SDLK_PERIOD:       nudge_range_end  (app, +range_step(app, mod & KMOD_SHIFT)); break;
    case SDLK_0:            set_range(app, 0, app->file.size); break;
    default: break;
    }
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr,
            "Usage: %s <file>\n"
            "\n"
            "binmap - binary visualization tool\n"
            "  TAB / SHIFT+TAB  next / previous view\n"
            "  L                toggle legend\n"
            "  A                toggle auto-rotate (3D views)\n"
            "  ARROWS           rotate (3D views)\n"
            "  + / - / WHEEL    zoom (3D views)\n"
            "  R                reset 3D view\n"
            "  [ / ]            nudge range start backward / forward\n"
            "  , / .            nudge range end backward / forward\n"
            "  SHIFT+key        coarse range step\n"
            "  0                reset range to full file\n"
            "  MOUSE DRAG       adjust range via slider at bottom\n"
            "  ESC / Q          quit\n",
            argv[0]);
        return 1;
    }

    binmap_app_t app = {0};
    if (!load_file(&app.file, argv[1])) return 1;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

    app.win_w = 1100;
    app.win_h = 800;
    app.window = SDL_CreateWindow("binmap",
                                  SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                  app.win_w, app.win_h,
                                  SDL_WINDOW_RESIZABLE);
    if (!app.window) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    app.renderer = SDL_CreateRenderer(app.window, -1,
                                      SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!app.renderer) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(app.window);
        SDL_Quit();
        return 1;
    }

    app.show_legend = true;
    app.current_view = VIEW_BYTE_CLASS;
    app.needs_redraw = true;
    app.yaw = 0.6f;
    app.pitch = 0.35f;
    app.zoom = 1.0f;
    app.auto_rotate = true;
    app.range_start = 0;
    app.range_end   = app.file.size;
    app.drag_mode   = DRAG_NONE;

    bool running = true;
    Uint32 last_tick = SDL_GetTicks();
    while (running) {
        bool animating = view_is_3d(app.current_view) && app.auto_rotate
                         && app.drag_mode == DRAG_NONE;

        if (app.needs_redraw) {
            redraw(&app);
            app.needs_redraw = false;
        }

        SDL_Event ev;
        bool got;
        if (animating) {
            got = SDL_WaitEventTimeout(&ev, 16);
        } else {
            got = SDL_WaitEvent(&ev);
            last_tick = SDL_GetTicks();
        }
        if (got) {
            do {
                switch (ev.type) {
                case SDL_QUIT:
                    running = false; break;
                case SDL_WINDOWEVENT:
                    switch (ev.window.event) {
                    case SDL_WINDOWEVENT_RESIZED:
                    case SDL_WINDOWEVENT_SIZE_CHANGED:
                        app.win_w = ev.window.data1;
                        app.win_h = ev.window.data2;
                        app.needs_redraw = true;
                        break;
                    case SDL_WINDOWEVENT_EXPOSED:
                        app.needs_redraw = true;
                        break;
                    default: break;
                    }
                    break;
                case SDL_KEYDOWN:
                    handle_key(&app, ev.key.keysym.sym, ev.key.keysym.mod, &running);
                    break;
                case SDL_MOUSEWHEEL:
                    if (view_is_3d(app.current_view)) {
                        if (ev.wheel.y > 0)      zoom_3d(&app, ZOOM_STEP);
                        else if (ev.wheel.y < 0) zoom_3d(&app, 1.0f / ZOOM_STEP);
                    }
                    break;
                case SDL_MOUSEBUTTONDOWN:
                    if (ev.button.button == SDL_BUTTON_LEFT)
                        handle_mouse_down(&app, ev.button.x, ev.button.y);
                    break;
                case SDL_MOUSEBUTTONUP:
                    if (ev.button.button == SDL_BUTTON_LEFT)
                        handle_mouse_up(&app);
                    break;
                case SDL_MOUSEMOTION:
                    if (app.drag_mode != DRAG_NONE)
                        handle_mouse_motion(&app, ev.motion.x);
                    break;
                default: break;
                }
            } while (running && SDL_PollEvent(&ev));
        }

        if (animating) {
            Uint32 now = SDL_GetTicks();
            float dt = (float)(now - last_tick) / 1000.0f;
            if (dt > 0.1f) dt = 0.1f;
            last_tick = now;
            app.yaw += 0.6f * dt;
            if (app.yaw > (float)(2 * M_PI)) app.yaw -= (float)(2 * M_PI);
            invalidate_3d_caches(&app);
            app.needs_redraw = true;
        }
    }

    invalidate_caches(&app);
    if (app.minimap_tex) { SDL_DestroyTexture(app.minimap_tex); app.minimap_tex = NULL; }
    if (app.renderer) SDL_DestroyRenderer(app.renderer);
    if (app.window)   SDL_DestroyWindow(app.window);
    SDL_Quit();
    if (app.file.data) munmap((void *)app.file.data, app.file.size);
    return 0;
}
