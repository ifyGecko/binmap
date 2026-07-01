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

/* Each entry is a NULL-terminated array of lines.  Line 0 is rendered as a
 * dim subtitle directly below the view title; the remaining lines are the
 * body.  Avoid characters not present in the bundled 5x7 font. */
static const char * const desc_byte_class[] = {
    "ROW-MAJOR. ONE PIXEL = ONE BYTE (OR CHUNK).",
    "COLOR ENCODES BYTE CATEGORY:",
    "  0X00 DARK, 0XFF WHITE, WHITESPACE GREEN,",
    "  CONTROL RED, ASCII BLUE, HIGH-BIT AMBER.",
    "USE: FAST ORIENTATION. SPOT HEADERS, TEXT",
    "REGIONS, PADDING, AND CODE AT A GLANCE.",
    NULL,
};
static const char * const desc_hilbert[] = {
    "BYTES LAID ALONG A 2D HILBERT CURVE.",
    "BYTES ADJACENT IN THE FILE STAY ADJACENT",
    "ON SCREEN (LOCALITY PRESERVING).",
    "USE: SECTION BOUNDARIES APPEAR AS COHERENT",
    "2D BLOCKS RATHER THAN SMEARED ROWS.",
    NULL,
};
static const char * const desc_digraph[] = {
    "256X256 MATRIX OF (DATA[I], DATA[I+1])",
    "TRANSITION COUNTS. LOG-SCALED HEAT COLOR.",
    "X = NEXT BYTE, Y = CURRENT BYTE.",
    "USE: FILE-TYPE FINGERPRINT. TEXT, X86, AES,",
    "JPEG EACH HAVE DISTINCT SIGNATURES.",
    NULL,
};
static const char * const desc_entropy[] = {
    "FILE SPLIT INTO CHUNKS. PLOT SHANNON",
    "ENTROPY PER CHUNK (0 TO 8 BITS PER BYTE).",
    "COLOR: DARK BLUE = LOW, WHITE = MAX.",
    "USE: LOCATE COMPRESSED, ENCRYPTED, OR",
    "RANDOM REGIONS VS STRUCTURED DATA.",
    NULL,
};
static const char * const desc_strings_density[] = {
    "PER-CHUNK FRACTION OF BYTES INSIDE A RUN",
    "OF >= 4 PRINTABLE ASCII (THE STRINGS RULE).",
    "COLOR: BLACK TO ORANGE TO WHITE AS DENSITY",
    "RISES.",
    "USE: FIND SYMBOL TABLES, MESSAGE BLOBS,",
    "EMBEDDED CONFIG, USER-FACING TEXT.",
    NULL,
};
static const char * const desc_self_similarity[] = {
    "SPLIT FILE INTO N CHUNKS. FOR EACH PAIR,",
    "COMPUTE L1 DISTANCE BETWEEN THEIR 256-BIN",
    "BYTE-FREQUENCY VECTORS. RENDER THE N X N",
    "MATRIX. HEAT COLOR: BRIGHT = SIMILAR.",
    "USE: OFF-DIAGONAL STRIPES REVEAL DUPLICATED",
    "REGIONS OR REPEATED PAYLOADS.",
    NULL,
};
static const char * const desc_bit_plane[] = {
    "4X2 GRID OF 8 SUB-IMAGES, ONE PER BIT 0..7.",
    "EACH PIXEL DRAWS THAT BIT OF THE BYTE",
    "(WHITE = 1, DARK = 0).",
    "USE: REVEAL BIT-ALIGNED STRUCTURE INVISIBLE",
    "TO BYTE-LEVEL VIEWS. LSB STEGANOGRAPHY.",
    NULL,
};
static const char * const desc_rgb_raw[] = {
    "EACH 3 CONSECUTIVE BYTES = ONE (R, G, B)",
    "PIXEL. LAID ROW-MAJOR ACROSS THE CANVAS.",
    "USE: LITERALLY REVEALS EMBEDDED BITMAPS,",
    "TEXTURES, OR FRAMEBUFFER DUMPS. FOR OTHER",
    "FILES, THE TEXTURE ITSELF IS A FINGERPRINT.",
    NULL,
};
static const char * const desc_trigraph[] = {
    "FOR EACH TRIPLE (A, B, C) PLOT A POINT AT",
    "(A, B, C) IN THE 256X256X256 CUBE.",
    "PER-PIXEL HIT COUNTS MAPPED TO LOG HEAT.",
    "USE: 3D VERSION OF THE DIGRAPH. AES FILLS",
    "THE CUBE UNIFORMLY, TEXT CLUSTERS IN ONE",
    "CORNER, CODE FORMS STRIATED PLANES.",
    NULL,
};
static const char * const desc_trigraph_spherical[] = {
    "SAME TRIPLES AS THE CUBE TRIGRAPH BUT READ",
    "AS SPHERICAL COORDS: THETA = A, PHI = B,",
    "R = C / 255. VELES-STYLE PROJECTION.",
    "USE: BIAS IN ANY BYTE POSITION FORMS",
    "VISIBLE SHELLS OR CLUSTERS. ALTERNATIVE",
    "GEOMETRIC READING OF 3-GRAM STRUCTURE.",
    NULL,
};

const char * const *view_descriptions[VIEW_COUNT] = {
    desc_byte_class,
    desc_hilbert,
    desc_digraph,
    desc_entropy,
    desc_strings_density,
    desc_self_similarity,
    desc_bit_plane,
    desc_rgb_raw,
    desc_trigraph,
    desc_trigraph_spherical,
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
    {"F",       "FOCUS/SPLIT"},
    {"H",       "HEAT CYCLE"},
    {"C-TAB",   "CYCLE PANEL"},
    {"1-9",     "PICK PANEL"},
    {"M",       "THUMB STRIP"},
    {"L",       "LEGEND"},
    {"D",       "DESCRIPTION"},
    {"A",       "AUTO-ROTATE"},
    {"ARROWS",  "ROTATE 3D"},
    {"+/-/WHL", "ZOOM 3D"},
    {"R",       "RESET 3D"},
    {"[ / ]",   "RANGE START"},
    {", / .",   "RANGE END"},
    {"SHF+KEY", "COARSE STEP"},
    {"0",       "RESET RANGE"},
    {"DRAG",    "RANGE SLIDER"},
    {"ESC",     "QUIT"},
};
#define CTRL_KEYS_COUNT (sizeof(ctrl_keys) / sizeof(ctrl_keys[0]))

#define STATUS_BAR_H     30
#define SLIDER_BAR_H     44
#define THUMB_STRIP_W    96
#define MIN_RANGE_BYTES  256
#define HANDLE_GRAB_PX   8
#define TRACK_MARGIN_X   12

/* ---------- small helpers ---------- */

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

static void format_offset(char *buf, size_t bufsz, size_t off)
{
    snprintf(buf, bufsz, "0x%zX", off);
}

static bool load_file(binmap_file_t *f, const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror(path); return false; }
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

/* ---------- per-panel cache ---------- */

static void free_panel_caches(binmap_app_t *app, int pi)
{
    binmap_panel_t *p = &app->panels[pi];
    for (int i = 0; i < VIEW_COUNT; i++) {
        if (p->cache[i]) { SDL_DestroyTexture(p->cache[i]); p->cache[i] = NULL; }
    }
    p->cache_w = p->cache_h = 0;
}

static void invalidate_caches(binmap_app_t *app)
{
    for (int i = 0; i < app->panel_count; i++) free_panel_caches(app, i);
}

static void invalidate_3d_caches(binmap_app_t *app)
{
    for (int pi = 0; pi < app->panel_count; pi++) {
        binmap_panel_t *p = &app->panels[pi];
        for (int i = 0; i < VIEW_COUNT; i++) {
            if (view_is_3d((view_id_t)i) && p->cache[i]) {
                SDL_DestroyTexture(p->cache[i]);
                p->cache[i] = NULL;
            }
        }
    }
}

/* Render one panel's current view into a caller-provided pixel buffer.
 * Shared by ensure_view_texture (single-panel path — RENDER_FULL) and
 * ensure_overlay_texture (RENDER_DATA_ONLY for the variance input,
 * RENDER_DECOR_ONLY for the legend layer that is composited on top). */
static void render_view_pixels(binmap_app_t *app, int pi, view_id_t view,
                               int cw, int ch, uint32_t *pixels,
                               render_mode_t mode)
{
    binmap_panel_t *p = &app->panels[pi];
    const uint8_t *d  = p->file.data + p->range_start;
    size_t         s  = p->range_end - p->range_start;
    size_t         bo = p->range_start;
    size_t         fs = p->file.size;

    switch (view) {
    case VIEW_BYTE_CLASS:      render_byte_class     (pixels, cw, ch, d, s, bo, fs, mode); break;
    case VIEW_HILBERT:         render_hilbert        (pixels, cw, ch, d, s, bo, fs, mode); break;
    case VIEW_DIGRAPH:         render_digraph        (pixels, cw, ch, d, s, bo, fs, mode); break;
    case VIEW_ENTROPY:         render_entropy        (pixels, cw, ch, d, s, bo, fs, mode); break;
    case VIEW_STRINGS_DENSITY: render_strings_density(pixels, cw, ch, d, s, bo, fs, mode); break;
    case VIEW_SELF_SIMILARITY: render_self_similarity(pixels, cw, ch, d, s, bo, fs, mode); break;
    case VIEW_BIT_PLANE:       render_bit_plane      (pixels, cw, ch, d, s, bo, fs, mode); break;
    case VIEW_RGB_RAW:         render_rgb_raw        (pixels, cw, ch, d, s, bo, fs, mode); break;
    case VIEW_TRIGRAPH:
        render_trigraph(pixels, cw, ch, d, s, bo, fs, app->yaw, app->pitch, app->zoom, mode);
        break;
    case VIEW_TRIGRAPH_SPHERICAL:
        render_trigraph_spherical(pixels, cw, ch, d, s, bo, fs,
                                  app->yaw, app->pitch, app->zoom, mode);
        break;
    default: break;
    }
}

static SDL_Texture *ensure_view_texture(binmap_app_t *app, int pi,
                                        view_id_t view, int cw, int ch)
{
    binmap_panel_t *p = &app->panels[pi];
    if (cw < 1 || ch < 1) return NULL;
    if (p->cache_w != cw || p->cache_h != ch) free_panel_caches(app, pi);

    bool is_3d = view_is_3d(view);
    if (!is_3d && p->cache[view]) return p->cache[view];

    uint32_t *pixels = (uint32_t *)calloc((size_t)cw * (size_t)ch, sizeof(uint32_t));
    if (!pixels) return NULL;
    render_view_pixels(app, pi, view, cw, ch, pixels, RENDER_FULL);

    if (!p->cache[view]) {
        p->cache[view] = SDL_CreateTexture(app->renderer,
                                            SDL_PIXELFORMAT_ARGB8888,
                                            SDL_TEXTUREACCESS_STATIC, cw, ch);
        if (!p->cache[view]) { free(pixels); return NULL; }
    }
    SDL_UpdateTexture(p->cache[view], NULL, pixels, cw * (int)sizeof(uint32_t));
    free(pixels);
    p->cache_w = cw;
    p->cache_h = ch;
    return p->cache[view];
}

/* ---------- overlay cache ---------- */

static void invalidate_overlay_caches(binmap_app_t *app)
{
    for (int i = 0; i < VIEW_COUNT; i++) {
        if (app->overlay_cache[i]) {
            SDL_DestroyTexture(app->overlay_cache[i]);
            app->overlay_cache[i] = NULL;
        }
    }
    app->overlay_cache_w = app->overlay_cache_h = 0;
}

static void invalidate_overlay_3d_caches(binmap_app_t *app)
{
    for (int i = 0; i < VIEW_COUNT; i++) {
        if (view_is_3d((view_id_t)i) && app->overlay_cache[i]) {
            SDL_DestroyTexture(app->overlay_cache[i]);
            app->overlay_cache[i] = NULL;
        }
    }
}

/* Overlay colour schemes.  H cycles 0 (off) -> 1..NUM -> 0. */
#define NUM_OVERLAY_SCHEMES 2

static const char *overlay_scheme_names[NUM_OVERLAY_SCHEMES + 1] = {
    "OFF",
    "SIMILARITY GLOW",   /* ice palette on agreement — white = files match */
    "DIFFERENCE GLOW",   /* fire palette on divergence — black = files match */
};

static inline uint32_t rgb_u32(int r, int g, int b)
{
    if (r < 0)   r = 0;
    if (r > 255) r = 255;
    if (g < 0)   g = 0;
    if (g > 255) g = 255;
    if (b < 0)   b = 0;
    if (b > 255) b = 255;
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* SIMILARITY GLOW — ice ramp: black -> ice blue -> sky -> white.
 * agreement = 0 maps to pure black; agreement = 1 maps to pure white so
 * identical files render as a uniformly white canvas. */
static uint32_t overlay_ice_glow(float t)
{
    if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
    t = powf(t, 0.55f);
    int r, g, b;
    if (t < 0.33f) {
        float k = t / 0.33f;
        r = (int)(30.0f  * k);
        g = (int)(60.0f  * k);
        b = (int)(120.0f * k);
    } else if (t < 0.75f) {
        float k = (t - 0.33f) / 0.42f;
        r = (int)(30.0f  + 70.0f  * k);
        g = (int)(60.0f  + 120.0f * k);
        b = (int)(120.0f + 135.0f * k);
    } else {
        float k = (t - 0.75f) / 0.25f;
        r = (int)(100.0f + 155.0f * k);
        g = (int)(180.0f + 75.0f  * k);
        b = 255;
    }
    return rgb_u32(r, g, b);
}

/* DIFFERENCE GLOW — fire ramp: black -> ember -> fire -> yellow -> white.
 * divergence = 0 maps to pure black so identical files render as a
 * uniformly black canvas. */
static uint32_t overlay_fire_glow(float t)
{
    if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
    t = powf(t, 0.55f);
    int r, g, b;
    if (t < 0.33f) {
        float k = t / 0.33f;
        r = (int)(120.0f * k);
        g = (int)(40.0f  * k);
        b = 0;
    } else if (t < 0.66f) {
        float k = (t - 0.33f) / 0.33f;
        r = (int)(120.0f + 135.0f * k);
        g = (int)(40.0f  + 80.0f  * k);
        b = 0;
    } else if (t < 0.9f) {
        float k = (t - 0.66f) / 0.24f;
        r = 255;
        g = (int)(120.0f + 120.0f * k);
        b = (int)(60.0f  * k);
    } else {
        float k = (t - 0.9f) / 0.1f;
        r = 255;
        g = (int)(240.0f + 15.0f  * k);
        b = (int)(60.0f  + 195.0f * k);
    }
    return rgb_u32(r, g, b);
}

/* Build (or return cached) overlay texture using app->overlay_scheme.
 * Agreement is derived from per-channel variance across all N panels'
 * rendered pixels; identical panels produce zero variance and therefore
 * maximum agreement (white for SIMILARITY GLOW, black for DIFFERENCE GLOW). */
static SDL_Texture *ensure_overlay_texture(binmap_app_t *app, view_id_t view,
                                           int cw, int ch)
{
    if (cw < 1 || ch < 1 || app->panel_count < 1) return NULL;
    if (app->overlay_cache_w != cw || app->overlay_cache_h != ch)
        invalidate_overlay_caches(app);

    bool is_3d = view_is_3d(view);
    if (!is_3d && app->overlay_cache[view]) return app->overlay_cache[view];

    int scheme = app->overlay_scheme;
    if (scheme <= 0 || scheme > NUM_OVERLAY_SCHEMES) scheme = 1;

    size_t px_count = (size_t)cw * (size_t)ch;
    int n = app->panel_count;

    /* Render every panel's DATA-ONLY image into its own scratch buffer.
     * Decorations (labels, wireframes, grids, ticks, titles) are excluded
     * here so they don't corrupt the similarity / difference signal — data
     * pixels have alpha=0xFF, non-data stays at calloc'd 0. */
    uint32_t *bufs[BINMAP_MAX_PANELS] = {0};
    for (int pi = 0; pi < n; pi++) {
        bufs[pi] = (uint32_t *)calloc(px_count, sizeof(uint32_t));
        if (!bufs[pi]) {
            for (int j = 0; j < pi; j++) free(bufs[j]);
            return NULL;
        }
        render_view_pixels(app, pi, view, cw, ch, bufs[pi], RENDER_DATA_ONLY);
    }

    uint32_t *out = (uint32_t *)calloc(px_count, sizeof(uint32_t));
    if (!out) { for (int pi = 0; pi < n; pi++) free(bufs[pi]); return NULL; }

    const float max_var = 3.0f * 128.0f * 128.0f;
    float inv_n = 1.0f / (float)n;
    /* Renderers write the alpha byte as 0 for anything that isn't file byte
     * data (bg fills, wireframes, grid lines, tick marks, axis / corner
     * labels, titles, borders) and 0xFF for pixels genuinely derived from
     * the file's bytes. If every panel marks a pixel as non-data, emit pure
     * black so decorative chrome and empty regions never confuse the
     * similarity / difference signal. */
    for (size_t i = 0; i < px_count; i++) {
        bool any_data = false;
        float sr = 0, sg = 0, sb = 0, sr2 = 0, sg2 = 0, sb2 = 0;
        for (int pi = 0; pi < n; pi++) {
            uint32_t c = bufs[pi][i];
            if (c & 0xFF000000u) any_data = true;
            float r = (float)((c >> 16) & 0xFF);
            float g = (float)((c >> 8)  & 0xFF);
            float b = (float)( c        & 0xFF);
            sr += r; sg += g; sb += b;
            sr2 += r*r; sg2 += g*g; sb2 += b*b;
        }
        if (!any_data) { out[i] = 0xFF000000u; continue; }

        float mr = sr * inv_n, mg = sg * inv_n, mb = sb * inv_n;
        float vr = sr2 * inv_n - mr * mr;
        float vg = sg2 * inv_n - mg * mg;
        float vb = sb2 * inv_n - mb * mb;
        float var = vr + vg + vb; if (var < 0) var = 0;
        float norm = var / max_var; if (norm > 1.0f) norm = 1.0f;
        float agreement = 1.0f - sqrtf(norm);
        if (agreement < 0) agreement = 0;

        out[i] = (scheme == 2)
            ? overlay_fire_glow(1.0f - agreement)
            : overlay_ice_glow(agreement);
    }

    for (int pi = 0; pi < n; pi++) free(bufs[pi]);

    /* Composite the focus panel's decoration layer (labels, wireframes,
     * grids, tick marks) on top of the glow so the user retains legend /
     * offset context in overlay mode. Titles are suppressed by the
     * renderer in RENDER_DECOR_ONLY — the status bar already names the
     * view. Focus-panel choice mirrors which file's ranges are shown in
     * the status bar. */
    uint32_t *deco = (uint32_t *)calloc(px_count, sizeof(uint32_t));
    if (deco) {
        int dpi = app->focus_panel;
        if (dpi < 0 || dpi >= n) dpi = 0;
        render_view_pixels(app, dpi, view, cw, ch, deco, RENDER_DECOR_ONLY);
        for (size_t i = 0; i < px_count; i++) {
            if (deco[i] & 0xFF000000u) out[i] = deco[i] | 0xFF000000u;
        }
        free(deco);
    }

    if (!app->overlay_cache[view]) {
        app->overlay_cache[view] = SDL_CreateTexture(app->renderer,
            SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, cw, ch);
        if (!app->overlay_cache[view]) { free(out); return NULL; }
    }
    SDL_UpdateTexture(app->overlay_cache[view], NULL, out, cw * (int)sizeof(uint32_t));
    free(out);
    app->overlay_cache_w = cw;
    app->overlay_cache_h = ch;
    return app->overlay_cache[view];
}

static SDL_Texture *ensure_minimap_texture(binmap_app_t *app, int pi, int w)
{
    binmap_panel_t *p = &app->panels[pi];
    if (w < 1) return NULL;
    if (p->minimap_tex && p->minimap_w == w) return p->minimap_tex;
    if (p->minimap_tex) { SDL_DestroyTexture(p->minimap_tex); p->minimap_tex = NULL; }
    uint32_t *row = (uint32_t *)calloc((size_t)w, sizeof(uint32_t));
    if (!row) return NULL;
    render_byte_class(row, w, 1, p->file.data, p->file.size, 0, p->file.size, RENDER_FULL);
    p->minimap_tex = SDL_CreateTexture(app->renderer,
                                        SDL_PIXELFORMAT_ARGB8888,
                                        SDL_TEXTUREACCESS_STATIC, w, 1);
    if (!p->minimap_tex) { free(row); return NULL; }
    SDL_UpdateTexture(p->minimap_tex, NULL, row, w * (int)sizeof(uint32_t));
    free(row);
    p->minimap_w = w;
    return p->minimap_tex;
}

/* ---------- layout ---------- */

static void compute_layout(binmap_app_t *app)
{
    for (int i = 0; i < app->panel_count; i++) {
        app->panels[i].canvas_rect = (SDL_Rect){0, 0, 0, 0};
        app->panels[i].slider_rect = (SDL_Rect){0, 0, 0, 0};
    }

    int canvas_top = STATUS_BAR_H;
    int total_h = app->win_h - canvas_top;
    if (total_h < 1) total_h = 1;

    /* Overlay mode: single full-canvas image, no per-panel rects, no sliders. */
    if (app->mode == MODE_OVERLAY) return;

    if (app->mode == MODE_FOCUS) {
        int pi = app->focus_panel;
        if (pi < 0 || pi >= app->panel_count) pi = 0;
        int strip_w = (app->panel_count > 1 && app->show_thumb_strip) ? THUMB_STRIP_W : 0;
        int cx = strip_w;
        int cw = app->win_w - strip_w;
        if (cw < 1) cw = 1;
        int ch = total_h - SLIDER_BAR_H;
        if (ch < 1) ch = 1;
        app->panels[pi].canvas_rect = (SDL_Rect){cx, canvas_top, cw, ch};
        app->panels[pi].slider_rect = (SDL_Rect){cx, canvas_top + ch, cw, SLIDER_BAR_H};
        return;
    }

    /* MODE_SPLIT — pick a grid based on panel count */
    int n = app->panel_count;
    int cols, rows;
    switch (n) {
    case 1: cols = 1; rows = 1; break;
    case 2:
        if (app->win_w >= app->win_h) { cols = 2; rows = 1; }
        else                          { cols = 1; rows = 2; }
        break;
    case 3: cols = 3; rows = 1; break;
    case 4: cols = 2; rows = 2; break;
    case 5:
    case 6: cols = 3; rows = 2; break;
    case 7:
    case 8: cols = 4; rows = 2; break;
    default: cols = n; rows = 1; break;
    }

    int cell_w = app->win_w / cols;
    int cell_h = total_h / rows;
    if (cell_w < 1) cell_w = 1;
    if (cell_h < 1) cell_h = 1;

    for (int i = 0; i < n; i++) {
        int gx = i % cols;
        int gy = i / cols;
        int x0 = gx * cell_w;
        int y0 = canvas_top + gy * cell_h;
        int w  = (gx == cols - 1) ? (app->win_w - x0) : cell_w;
        int h  = (gy == rows - 1) ? (canvas_top + total_h - y0) : cell_h;
        int slider_h = SLIDER_BAR_H;
        if (h < slider_h * 2) slider_h = h / 3;
        if (slider_h < 12)    slider_h = 12;
        int canvas_h = h - slider_h;
        if (canvas_h < 1) canvas_h = 1;
        app->panels[i].canvas_rect = (SDL_Rect){x0, y0, w, canvas_h};
        app->panels[i].slider_rect = (SDL_Rect){x0, y0 + canvas_h, w, slider_h};
    }
}

/* ---------- slider math (per-panel) ---------- */

static int panel_slider_track_x(const binmap_panel_t *p)
{
    return p->slider_rect.x + TRACK_MARGIN_X;
}
static int panel_slider_track_w(const binmap_panel_t *p)
{
    int w = p->slider_rect.w - 2 * TRACK_MARGIN_X;
    return w < 1 ? 1 : w;
}
static int panel_slider_track_y(const binmap_panel_t *p)
{
    int th = 14;
    int strip_h = p->slider_rect.h;
    if (strip_h >= SLIDER_BAR_H) return p->slider_rect.y + 20;
    int y = p->slider_rect.y + strip_h - th - 4;
    if (y < p->slider_rect.y + 2) y = p->slider_rect.y + 2;
    return y;
}

static int panel_byte_to_px(const binmap_panel_t *p, size_t b)
{
    if (p->file.size == 0) return panel_slider_track_x(p);
    double t = (double)b / (double)p->file.size;
    if (t < 0) t = 0; else if (t > 1) t = 1;
    return panel_slider_track_x(p) + (int)(t * panel_slider_track_w(p) + 0.5);
}

static size_t panel_px_to_byte(const binmap_panel_t *p, int x)
{
    int tx = panel_slider_track_x(p);
    int tw = panel_slider_track_w(p);
    if (x <= tx) return 0;
    if (x >= tx + tw) return p->file.size;
    double t = (double)(x - tx) / (double)tw;
    size_t b = (size_t)(t * (double)p->file.size + 0.5);
    if (b > p->file.size) b = p->file.size;
    return b;
}

static size_t range_min_width(const binmap_panel_t *p)
{
    return p->file.size < MIN_RANGE_BYTES ? p->file.size : MIN_RANGE_BYTES;
}

static void set_range_panel(binmap_app_t *app, int pi, size_t new_start, size_t new_end)
{
    binmap_panel_t *p = &app->panels[pi];
    size_t minw = range_min_width(p);
    if (new_end > p->file.size) new_end = p->file.size;
    if (new_start + minw > new_end) {
        if (new_end >= minw) new_start = new_end - minw;
        else                 new_start = 0;
    }
    if (new_start == p->range_start && new_end == p->range_end) return;
    p->range_start = new_start;
    p->range_end   = new_end;
    free_panel_caches(app, pi);
    invalidate_overlay_caches(app);
    app->needs_redraw = true;
}

static void nudge_range_start_panel(binmap_app_t *app, int pi, long delta)
{
    binmap_panel_t *p = &app->panels[pi];
    long s = (long)p->range_start + delta;
    if (s < 0) s = 0;
    size_t ns = (size_t)s;
    size_t minw = range_min_width(p);
    if (ns + minw > p->range_end) {
        if (p->range_end >= minw) ns = p->range_end - minw; else ns = 0;
    }
    set_range_panel(app, pi, ns, p->range_end);
}

static void nudge_range_end_panel(binmap_app_t *app, int pi, long delta)
{
    binmap_panel_t *p = &app->panels[pi];
    long e = (long)p->range_end + delta;
    if (e < 0) e = 0;
    size_t ne = (size_t)e;
    if (ne > p->file.size) ne = p->file.size;
    size_t minw = range_min_width(p);
    if (p->range_start + minw > ne) ne = p->range_start + minw;
    if (ne > p->file.size) ne = p->file.size;
    set_range_panel(app, pi, p->range_start, ne);
}

static long range_step_panel(const binmap_panel_t *p, bool coarse)
{
    if (p->file.size == 0) return 1;
    long step = coarse ? (long)(p->file.size / 100) : (long)(p->file.size / 1000);
    if (step < 1) step = 1;
    return step;
}

/* ---------- draw ---------- */

static void draw_panel_slider(binmap_app_t *app, int pi)
{
    binmap_panel_t *p = &app->panels[pi];
    if (p->slider_rect.w < 1 || p->slider_rect.h < 1) return;
    int strip_x = p->slider_rect.x;
    int strip_y = p->slider_rect.y;
    int strip_w = p->slider_rect.w;
    int strip_h = p->slider_rect.h;

    SDL_Rect strip = {strip_x, strip_y, strip_w, strip_h};
    SDL_SetRenderDrawColor(app->renderer, 22, 22, 28, 255);
    SDL_RenderFillRect(app->renderer, &strip);
    SDL_Rect sep = {strip_x, strip_y, strip_w, 1};
    SDL_SetRenderDrawColor(app->renderer, 70, 70, 90, 255);
    SDL_RenderFillRect(app->renderer, &sep);

    int tx = panel_slider_track_x(p);
    int tw = panel_slider_track_w(p);
    int ty = panel_slider_track_y(p);
    int th = 14;

    SDL_Texture *mm = ensure_minimap_texture(app, pi, tw);
    if (mm) {
        SDL_Rect dst = {tx, ty, tw, th};
        SDL_RenderCopy(app->renderer, mm, NULL, &dst);
    } else {
        SDL_Rect dst = {tx, ty, tw, th};
        SDL_SetRenderDrawColor(app->renderer, 40, 40, 50, 255);
        SDL_RenderFillRect(app->renderer, &dst);
    }

    int sx = panel_byte_to_px(p, p->range_start);
    int ex = panel_byte_to_px(p, p->range_end);
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
    SDL_Color start_col = (p->drag_mode == DRAG_START)
                            ? (SDL_Color){255, 240, 160, 255}
                            : (SDL_Color){255, 200, 100, 255};
    SDL_Color end_col   = (p->drag_mode == DRAG_END)
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

    if (strip_h >= 20) {
        int scale = (strip_w >= 500 && strip_h >= SLIDER_BAR_H) ? 2 : 1;
        int label_y = strip_y + 4;
        SDL_Color label_col = {200, 200, 215, 255};
        SDL_Color dim_col   = {150, 150, 165, 255};

        char lstart[64], lend[64], lmid[96];
        char sstart[32], send[32], slen[32];
        format_offset(sstart, sizeof(sstart), p->range_start);
        format_offset(send,   sizeof(send),   p->range_end);
        format_size (slen,   sizeof(slen),   p->range_end - p->range_start);
        snprintf(lstart, sizeof(lstart), "START %s", sstart);
        snprintf(lend,   sizeof(lend),   "END %s",   send);
        double pct = p->file.size
                     ? 100.0 * (double)(p->range_end - p->range_start) / (double)p->file.size
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
}

static void draw_status_bar(binmap_app_t *app)
{
    SDL_Rect bar = {0, 0, app->win_w, STATUS_BAR_H};
    SDL_SetRenderDrawColor(app->renderer, 22, 22, 28, 255);
    SDL_RenderFillRect(app->renderer, &bar);
    SDL_Rect sep = {0, STATUS_BAR_H - 1, app->win_w, 1};
    SDL_SetRenderDrawColor(app->renderer, 70, 70, 90, 255);
    SDL_RenderFillRect(app->renderer, &sep);

    char left[256];
    snprintf(left, sizeof(left), "[%d/%d] %s",
             app->current_view + 1, VIEW_COUNT,
             view_names[app->current_view]);
    SDL_Color title_color = {255, 200, 100, 255};
    draw_text(app->renderer, 10, 8, 2, title_color, left);

    int pi = (app->mode == MODE_FOCUS) ? app->focus_panel : app->active_panel;
    if (pi < 0 || pi >= app->panel_count) pi = 0;
    binmap_panel_t *p = &app->panels[pi];
    char sz[32];
    format_size(sz, sizeof(sz), p->file.size);

    char range_info[64] = "";
    if (app->mode != MODE_OVERLAY
        && (p->range_start != 0 || p->range_end != p->file.size)) {
        snprintf(range_info, sizeof(range_info),
                 "  [0x%zX..0x%zX]", p->range_start, p->range_end);
    }

    char pan_info[64] = "";
    if (app->panel_count > 1) {
        if (app->mode == MODE_OVERLAY) {
            snprintf(pan_info, sizeof(pan_info),
                     "  <%d FILES>", app->panel_count);
        } else {
            const char *mode_str = (app->mode == MODE_FOCUS) ? "F" : "S";
            snprintf(pan_info, sizeof(pan_info), "  <%d/%d %s>",
                     pi + 1, app->panel_count, mode_str);
        }
    }

    char right[512];
    if (app->mode == MODE_OVERLAY) {
        int scheme = app->overlay_scheme;
        if (scheme < 1 || scheme > NUM_OVERLAY_SCHEMES) scheme = 1;
        const char *scheme_name = overlay_scheme_names[scheme];
        if (view_is_3d(app->current_view)) {
            snprintf(right, sizeof(right),
                     "OVERLAY [%d/%d] %s%s  YAW %.0f  PITCH %.0f  ZOOM %.2fX%s",
                     scheme, NUM_OVERLAY_SCHEMES, scheme_name, pan_info,
                     app->yaw   * 180.0 / M_PI,
                     app->pitch * 180.0 / M_PI,
                     app->zoom,
                     app->auto_rotate ? "  AUTO" : "");
        } else {
            snprintf(right, sizeof(right), "OVERLAY [%d/%d] %s%s",
                     scheme, NUM_OVERLAY_SCHEMES, scheme_name, pan_info);
        }
    } else if (view_is_3d(app->current_view)) {
        snprintf(right, sizeof(right),
                 "%s (%s)%s%s  YAW %.0f  PITCH %.0f  ZOOM %.2fX%s",
                 basename_of(p->file.path), sz, range_info, pan_info,
                 app->yaw   * 180.0 / M_PI,
                 app->pitch * 180.0 / M_PI,
                 app->zoom,
                 app->auto_rotate ? "  AUTO" : "");
    } else {
        snprintf(right, sizeof(right), "%s (%s)%s%s",
                 basename_of(p->file.path), sz, range_info, pan_info);
    }
    SDL_Color text_color = {200, 200, 215, 255};
    int rw = text_width(2, right);
    int rx = app->win_w - rw - 10;
    int lw = text_width(2, left);
    if (rx < lw + 30) rx = lw + 30;
    draw_text(app->renderer, rx, 8, 2, text_color, right);

    char hover_buf[256];
    hover_buf[0] = '\0';
    bool have_hover = false;
    if (app->mode == MODE_OVERLAY) {
        bool all_have = app->panel_count > 0;
        bool all_same = true;
        size_t first_off = 0;
        for (int i = 0; i < app->panel_count; i++) {
            if (!app->panels[i].hover_has_offset) { all_have = false; break; }
            if (i == 0) first_off = app->panels[i].hover_offset;
            else if (app->panels[i].hover_offset != first_off) all_same = false;
        }
        if (all_have && all_same) {
            snprintf(hover_buf, sizeof(hover_buf), "@0x%zX", first_off);
            have_hover = true;
        } else {
            size_t len = 0;
            for (int i = 0; i < app->panel_count; i++) {
                if (!app->panels[i].hover_has_offset) continue;
                int n = snprintf(hover_buf + len, sizeof(hover_buf) - len,
                                 "%s%d@0x%zX", (len ? " " : ""),
                                 i + 1, app->panels[i].hover_offset);
                if (n < 0 || (size_t)n >= sizeof(hover_buf) - len) break;
                len += (size_t)n;
                have_hover = true;
            }
        }
    } else if (p->hover_has_offset) {
        snprintf(hover_buf, sizeof(hover_buf), "@0x%zX", p->hover_offset);
        have_hover = true;
    }

    if (have_hover) {
        int hw = text_width(2, hover_buf);
        int left_end  = 10 + lw;
        int right_beg = rx;
        int gap_w = right_beg - left_end;
        if (gap_w >= hw + 20) {
            int hx = left_end + (gap_w - hw) / 2;
            SDL_Color hover_col = {255, 240, 160, 255};
            draw_text(app->renderer, hx, 8, 2, hover_col, hover_buf);
        }
    }
}

static void draw_thumb_strip(binmap_app_t *app)
{
    if (app->mode != MODE_FOCUS || !app->show_thumb_strip || app->panel_count <= 1) return;
    int x0 = 0, y0 = STATUS_BAR_H;
    int w = THUMB_STRIP_W;
    int h = app->win_h - STATUS_BAR_H;
    SDL_Rect bg = {x0, y0, w, h};
    SDL_SetRenderDrawColor(app->renderer, 20, 20, 26, 255);
    SDL_RenderFillRect(app->renderer, &bg);
    SDL_Rect sep = {x0 + w - 1, y0, 1, h};
    SDL_SetRenderDrawColor(app->renderer, 70, 70, 90, 255);
    SDL_RenderFillRect(app->renderer, &sep);

    int pad = 6;
    int box_w = w - pad * 2;
    int box_h = 64;
    int step_y = box_h + 8;
    for (int i = 0; i < app->panel_count; i++) {
        int by = y0 + pad + i * step_y;
        if (by + box_h > y0 + h - pad) break;
        binmap_panel_t *p = &app->panels[i];
        SDL_Rect box = {x0 + pad, by, box_w, box_h};
        SDL_SetRenderDrawColor(app->renderer, 28, 28, 34, 255);
        SDL_RenderFillRect(app->renderer, &box);

        SDL_Texture *mm = ensure_minimap_texture(app, i, box_w);
        if (mm) {
            SDL_Rect fill = {x0 + pad, by, box_w, 14};
            SDL_RenderCopy(app->renderer, mm, NULL, &fill);
        }

        SDL_Color border = (i == app->focus_panel)
            ? (SDL_Color){255, 200, 100, 255}
            : (SDL_Color){80, 80, 100, 255};
        SDL_SetRenderDrawColor(app->renderer, border.r, border.g, border.b, 255);
        SDL_RenderDrawRect(app->renderer, &box);

        char lbl[8];
        snprintf(lbl, sizeof(lbl), "%d", i + 1);
        SDL_Color lc = (i == app->focus_panel)
            ? (SDL_Color){255, 240, 160, 255} : (SDL_Color){220, 220, 235, 255};
        draw_text(app->renderer, x0 + pad + 4, by + 20, 2, lc, lbl);

        const char *nm = basename_of(p->file.path);
        int max_chars = (box_w - 6) / (FONT_W + 1);
        if (max_chars < 1)  max_chars = 1;
        if (max_chars > 20) max_chars = 20;
        char short_nm[24];
        snprintf(short_nm, sizeof(short_nm), "%.*s", max_chars, nm);
        SDL_Color dim = {180, 180, 195, 255};
        draw_text(app->renderer, x0 + pad + 4, by + 40, 1, dim, short_nm);
        char sz[24];
        format_size(sz, sizeof(sz), p->file.size);
        draw_text(app->renderer, x0 + pad + 4, by + 50, 1, dim, sz);
    }
}

static void draw_panel_labels(binmap_app_t *app)
{
    if (app->mode == MODE_FOCUS || app->panel_count <= 1) return;
    SDL_SetRenderDrawBlendMode(app->renderer, SDL_BLENDMODE_BLEND);
    for (int pi = 0; pi < app->panel_count; pi++) {
        binmap_panel_t *p = &app->panels[pi];
        if (p->canvas_rect.w < 1) continue;

        SDL_Color border = (pi == app->active_panel)
            ? (SDL_Color){255, 200, 100, 255}
            : (SDL_Color){70, 70, 90, 180};
        SDL_SetRenderDrawColor(app->renderer, border.r, border.g, border.b, border.a);
        SDL_RenderDrawRect(app->renderer, &p->canvas_rect);

        char lbl[128];
        snprintf(lbl, sizeof(lbl), "%d.%s", pi + 1, basename_of(p->file.path));
        int scale = 1;
        int th = FONT_H * scale;
        int tw = text_width(scale, lbl);
        int tx = p->canvas_rect.x + 6;
        int ty = p->canvas_rect.y + 6;
        SDL_Rect shadow = {tx - 3, ty - 2, tw + 6, th + 4};
        SDL_SetRenderDrawColor(app->renderer, 0, 0, 0, 170);
        SDL_RenderFillRect(app->renderer, &shadow);
        SDL_Color lc = (pi == app->active_panel)
            ? (SDL_Color){255, 240, 160, 255} : (SDL_Color){220, 220, 235, 255};
        draw_text(app->renderer, tx, ty, scale, lc, lbl);
    }
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
    int y0 = app->win_h - legend_h - 12;
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

static void draw_description(binmap_app_t *app)
{
    if (!app->show_description) return;
    const char * const *lines = view_descriptions[app->current_view];
    if (!lines) return;

    int title_scale = 2;
    int body_scale  = 2;
    int title_h     = FONT_H * title_scale;
    int line_h      = (FONT_H + 3) * body_scale;
    int pad         = 14;
    int gap         = 8;

    int body_w = 0;
    int body_n = 0;
    for (const char * const *pp = lines; *pp; pp++) {
        int lw = text_width(body_scale, *pp);
        if (lw > body_w) body_w = lw;
        body_n++;
    }

    const char *title = view_names[app->current_view];
    int title_w = text_width(title_scale, title);
    int hint_scale = 1;
    const char *hint = "PRESS D TO DISMISS";
    int hint_w = text_width(hint_scale, hint);
    int hint_h = FONT_H * hint_scale;

    int content_w = body_w;
    if (title_w > content_w) content_w = title_w;
    if (hint_w  > content_w) content_w = hint_w;

    int content_h = title_h + gap + body_n * line_h + gap + hint_h;
    int box_w = content_w + pad * 2;
    int box_h = content_h + pad * 2;

    int canvas_top = STATUS_BAR_H;
    int canvas_bot = app->win_h;
    int canvas_h   = canvas_bot - canvas_top;
    if (box_h > canvas_h - 8) box_h = canvas_h - 8;
    if (box_w > app->win_w - 16) box_w = app->win_w - 16;
    int x0 = (app->win_w - box_w) / 2;
    int y0 = canvas_top + (canvas_h - box_h) / 2;

    SDL_SetRenderDrawBlendMode(app->renderer, SDL_BLENDMODE_BLEND);
    SDL_Rect bg = {x0, y0, box_w, box_h};
    SDL_SetRenderDrawColor(app->renderer, 18, 18, 24, 235);
    SDL_RenderFillRect(app->renderer, &bg);
    SDL_SetRenderDrawColor(app->renderer, 90, 90, 115, 255);
    SDL_RenderDrawRect(app->renderer, &bg);

    SDL_Color title_col = {255, 200, 100, 255};
    SDL_Color body_col  = {220, 220, 235, 255};
    SDL_Color hint_col  = {150, 150, 165, 255};

    int y = y0 + pad;
    int content_x = x0 + pad;
    draw_text(app->renderer, content_x, y, title_scale, title_col, title);
    y += title_h + gap;

    for (const char * const *pp = lines; *pp; pp++) {
        draw_text(app->renderer, content_x, y, body_scale, body_col, *pp);
        y += line_h;
    }

    int hint_x = x0 + box_w - pad - hint_w;
    int hint_y = y0 + box_h - pad - hint_h;
    draw_text(app->renderer, hint_x, hint_y, hint_scale, hint_col, hint);
}

static void redraw(binmap_app_t *app)
{
    SDL_SetRenderDrawColor(app->renderer, 10, 10, 12, 255);
    SDL_RenderClear(app->renderer);
    compute_layout(app);

    if (app->mode == MODE_OVERLAY) {
        int cw = app->win_w;
        int ch = app->win_h - STATUS_BAR_H;
        SDL_Texture *tex = ensure_overlay_texture(app, app->current_view, cw, ch);
        if (tex) {
            SDL_Rect dst = {0, STATUS_BAR_H, cw, ch};
            SDL_RenderCopy(app->renderer, tex, NULL, &dst);
        }
        draw_status_bar(app);
        draw_legend(app);
        draw_description(app);
        SDL_RenderPresent(app->renderer);
        return;
    }

    for (int pi = 0; pi < app->panel_count; pi++) {
        binmap_panel_t *p = &app->panels[pi];
        if (p->canvas_rect.w < 1 || p->canvas_rect.h < 1) continue;
        int cw = p->canvas_rect.w;
        int ch = p->canvas_rect.h;
        SDL_Texture *tex = ensure_view_texture(app, pi, app->current_view, cw, ch);
        if (tex) SDL_RenderCopy(app->renderer, tex, NULL, &p->canvas_rect);
    }

    for (int pi = 0; pi < app->panel_count; pi++) draw_panel_slider(app, pi);

    draw_panel_labels(app);
    draw_status_bar(app);
    draw_thumb_strip(app);
    draw_legend(app);
    draw_description(app);
    SDL_RenderPresent(app->renderer);
}

/* ---------- view / 3D ---------- */

static void switch_view(binmap_app_t *app, view_id_t v)
{
    app->current_view = v;
    app->show_description = false;
    for (int i = 0; i < app->panel_count; i++)
        app->panels[i].hover_has_offset = false;
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
    invalidate_overlay_3d_caches(app);
    app->needs_redraw = true;
}

#define ZOOM_MIN  0.25f
#define ZOOM_MAX  12.0f
#define ZOOM_STEP 1.15f

static void zoom_3d(binmap_app_t *app, float factor)
{
    if (!view_is_3d(app->current_view)) return;
    app->zoom *= factor;
    if (app->zoom < ZOOM_MIN) app->zoom = ZOOM_MIN;
    if (app->zoom > ZOOM_MAX) app->zoom = ZOOM_MAX;
    invalidate_3d_caches(app);
    invalidate_overlay_3d_caches(app);
    app->needs_redraw = true;
}

static void reset_view(binmap_app_t *app)
{
    app->yaw = 0.6f;
    app->pitch = 0.35f;
    app->zoom = 1.0f;
    app->auto_rotate = true;
    invalidate_3d_caches(app);
    invalidate_overlay_3d_caches(app);
    app->needs_redraw = true;
}

/* ---------- mouse routing ---------- */

static bool point_in_rect(int x, int y, const SDL_Rect *r)
{
    return r->w >= 1 && r->h >= 1 &&
           x >= r->x && x < r->x + r->w &&
           y >= r->y && y < r->y + r->h;
}

static int thumb_at(binmap_app_t *app, int mx, int my)
{
    if (app->mode != MODE_FOCUS || !app->show_thumb_strip || app->panel_count <= 1) return -1;
    if (mx < 0 || mx >= THUMB_STRIP_W || my < STATUS_BAR_H) return -1;
    int y0 = STATUS_BAR_H;
    int pad = 6;
    int box_h = 64;
    int step_y = box_h + 8;
    for (int i = 0; i < app->panel_count; i++) {
        int by = y0 + pad + i * step_y;
        if (my >= by && my < by + box_h) return i;
    }
    return -1;
}

static int panel_at(binmap_app_t *app, int mx, int my, bool *out_in_slider)
{
    if (out_in_slider) *out_in_slider = false;
    for (int i = 0; i < app->panel_count; i++) {
        if (point_in_rect(mx, my, &app->panels[i].canvas_rect)) return i;
        if (point_in_rect(mx, my, &app->panels[i].slider_rect)) {
            if (out_in_slider) *out_in_slider = true;
            return i;
        }
    }
    return -1;
}

static void handle_mouse_down(binmap_app_t *app, int mx, int my)
{
    int ti = thumb_at(app, mx, my);
    if (ti >= 0) {
        if (app->focus_panel != ti) {
            app->focus_panel = ti;
            app->active_panel = ti;
            app->needs_redraw = true;
        }
        return;
    }
    bool in_slider = false;
    int pi = panel_at(app, mx, my, &in_slider);
    if (pi < 0) return;
    if (pi != app->active_panel) {
        app->active_panel = pi;
        app->needs_redraw = true;
    }
    if (!in_slider) return;

    binmap_panel_t *p = &app->panels[pi];
    int sx = panel_byte_to_px(p, p->range_start);
    int ex = panel_byte_to_px(p, p->range_end);
    size_t minw = range_min_width(p);

    if (abs(mx - sx) <= HANDLE_GRAB_PX) {
        p->drag_mode = DRAG_START;
    } else if (abs(mx - ex) <= HANDLE_GRAB_PX) {
        p->drag_mode = DRAG_END;
    } else if (mx > sx && mx < ex) {
        p->drag_mode = DRAG_MIDDLE;
    } else {
        size_t target = panel_px_to_byte(p, mx);
        if (mx < sx) {
            size_t ns = target;
            if (ns + minw > p->range_end) {
                if (p->range_end >= minw) ns = p->range_end - minw; else ns = 0;
            }
            p->range_start = ns;
            p->drag_mode = DRAG_START;
        } else {
            size_t ne = target;
            if (ne > p->file.size) ne = p->file.size;
            if (p->range_start + minw > ne) ne = p->range_start + minw;
            if (ne > p->file.size) ne = p->file.size;
            p->range_end = ne;
            p->drag_mode = DRAG_END;
        }
    }
    p->drag_anchor_start = p->range_start;
    p->drag_anchor_end   = p->range_end;
    p->drag_anchor_px    = mx;
    app->needs_redraw = true;
}

static void update_hover_offset(binmap_app_t *app, int mx, int my)
{
    for (int i = 0; i < app->panel_count; i++) {
        if (app->panels[i].hover_has_offset) {
            app->panels[i].hover_has_offset = false;
            app->needs_redraw = true;
        }
    }
    if (app->current_view != VIEW_HILBERT) return;

    if (app->mode == MODE_OVERLAY) {
        int cw = app->win_w;
        int ch = app->win_h - STATUS_BAR_H;
        int cx = mx;
        int cy = my - STATUS_BAR_H;
        if (cw < 1 || ch < 1) return;
        if (cx < 0 || cx >= cw || cy < 0 || cy >= ch) return;
        for (int i = 0; i < app->panel_count; i++) {
            binmap_panel_t *p = &app->panels[i];
            size_t off;
            size_t range = p->range_end - p->range_start;
            if (render_hilbert_offset_at(cx, cy, cw, ch, range, p->range_start, &off)) {
                p->hover_has_offset = true;
                p->hover_offset = off;
                app->needs_redraw = true;
            }
        }
        return;
    }

    int pi = panel_at(app, mx, my, NULL);
    if (pi < 0) return;
    binmap_panel_t *p = &app->panels[pi];
    if (p->canvas_rect.w < 1) return;
    if (mx < p->canvas_rect.x || mx >= p->canvas_rect.x + p->canvas_rect.w) return;
    if (my < p->canvas_rect.y || my >= p->canvas_rect.y + p->canvas_rect.h) return;
    int cx = mx - p->canvas_rect.x;
    int cy = my - p->canvas_rect.y;
    int cw = p->canvas_rect.w;
    int ch = p->canvas_rect.h;
    size_t off;
    size_t range = p->range_end - p->range_start;
    if (render_hilbert_offset_at(cx, cy, cw, ch, range, p->range_start, &off)) {
        p->hover_has_offset = true;
        p->hover_offset = off;
        app->needs_redraw = true;
    }
}

static void handle_mouse_motion(binmap_app_t *app, int mx, int my)
{
    int pi_hover = panel_at(app, mx, my, NULL);
    if (pi_hover >= 0 && pi_hover != app->active_panel) {
        app->active_panel = pi_hover;
        app->needs_redraw = true;
    }
    update_hover_offset(app, mx, my);

    binmap_panel_t *p = NULL;
    for (int i = 0; i < app->panel_count; i++) {
        if (app->panels[i].drag_mode != DRAG_NONE) { p = &app->panels[i]; break; }
    }
    if (!p) return;
    size_t minw = range_min_width(p);

    if (p->drag_mode == DRAG_START) {
        size_t ns = panel_px_to_byte(p, mx);
        if (ns + minw > p->range_end) {
            if (p->range_end >= minw) ns = p->range_end - minw; else ns = 0;
        }
        if (ns != p->range_start) { p->range_start = ns; app->needs_redraw = true; }
    } else if (p->drag_mode == DRAG_END) {
        size_t ne = panel_px_to_byte(p, mx);
        if (ne > p->file.size) ne = p->file.size;
        if (p->range_start + minw > ne) ne = p->range_start + minw;
        if (ne > p->file.size) ne = p->file.size;
        if (ne != p->range_end) { p->range_end = ne; app->needs_redraw = true; }
    } else if (p->drag_mode == DRAG_MIDDLE) {
        int tw = panel_slider_track_w(p);
        if (tw < 1 || p->file.size == 0) return;
        double dpx = (double)(mx - p->drag_anchor_px);
        long   dbytes = (long)(dpx / (double)tw * (double)p->file.size);
        long   width = (long)(p->drag_anchor_end - p->drag_anchor_start);
        long   ns = (long)p->drag_anchor_start + dbytes;
        if (ns < 0) ns = 0;
        if (ns + width > (long)p->file.size) ns = (long)p->file.size - width;
        if (ns < 0) ns = 0;
        size_t nstart = (size_t)ns;
        size_t nend   = nstart + (size_t)width;
        if (nstart != p->range_start || nend != p->range_end) {
            p->range_start = nstart;
            p->range_end   = nend;
            app->needs_redraw = true;
        }
    }
}

static void handle_mouse_up(binmap_app_t *app)
{
    bool any_changed = false;
    for (int i = 0; i < app->panel_count; i++) {
        binmap_panel_t *p = &app->panels[i];
        if (p->drag_mode == DRAG_NONE) continue;
        bool changed = (p->range_start != p->drag_anchor_start)
                    || (p->range_end   != p->drag_anchor_end);
        p->drag_mode = DRAG_NONE;
        if (changed) {
            free_panel_caches(app, i);
            any_changed = true;
        }
        app->needs_redraw = true;
    }
    if (any_changed) invalidate_overlay_caches(app);
}

/* ---------- keys ---------- */

static void handle_key(binmap_app_t *app, SDL_Keycode k, SDL_Keymod mod, bool *running)
{
    int pi = (app->mode == MODE_FOCUS) ? app->focus_panel : app->active_panel;
    if (pi < 0 || pi >= app->panel_count) pi = 0;

    switch (k) {
    case SDLK_ESCAPE:
    case SDLK_q: *running = false; break;

    case SDLK_TAB: {
        if (mod & KMOD_CTRL) {
            if (app->panel_count > 1) {
                int delta = (mod & KMOD_SHIFT) ? -1 : 1;
                int next = (pi + delta + app->panel_count) % app->panel_count;
                app->active_panel = next;
                if (app->mode == MODE_FOCUS) app->focus_panel = next;
                app->needs_redraw = true;
            }
        } else {
            int n = VIEW_COUNT;
            int delta = (mod & KMOD_SHIFT) ? -1 : 1;
            int next = ((int)app->current_view + delta + n) % n;
            switch_view(app, (view_id_t)next);
        }
        break;
    }

    case SDLK_l:
        app->show_legend = !app->show_legend;
        app->needs_redraw = true;
        break;
    case SDLK_d:
        app->show_description = !app->show_description;
        app->needs_redraw = true;
        break;

    case SDLK_f:
        if (app->panel_count > 1) {
            if (app->mode == MODE_OVERLAY) {
                /* leave overlay first */
                app->mode = app->prev_mode;
                app->overlay_scheme = 0;
            }
            if (app->mode == MODE_SPLIT) {
                app->mode = MODE_FOCUS;
                app->focus_panel = pi;
            } else if (app->mode == MODE_FOCUS) {
                app->mode = MODE_SPLIT;
            }
            app->needs_redraw = true;
        }
        break;

    case SDLK_h:
        if (app->panel_count > 1) {
            if (app->mode != MODE_OVERLAY) {
                app->prev_mode = app->mode;
                app->mode = MODE_OVERLAY;
                app->overlay_scheme = 1;
            } else {
                app->overlay_scheme++;
                if (app->overlay_scheme > NUM_OVERLAY_SCHEMES) {
                    app->mode = app->prev_mode;
                    app->overlay_scheme = 0;
                }
            }
            invalidate_overlay_caches(app);
            app->needs_redraw = true;
        }
        break;

    case SDLK_m:
        if (app->panel_count > 1) {
            app->show_thumb_strip = !app->show_thumb_strip;
            app->needs_redraw = true;
        }
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

    case SDLK_LEFTBRACKET:
        nudge_range_start_panel(app, pi,
            -range_step_panel(&app->panels[pi], mod & KMOD_SHIFT));
        break;
    case SDLK_RIGHTBRACKET:
        nudge_range_start_panel(app, pi,
            +range_step_panel(&app->panels[pi], mod & KMOD_SHIFT));
        break;
    case SDLK_COMMA:
        nudge_range_end_panel(app, pi,
            -range_step_panel(&app->panels[pi], mod & KMOD_SHIFT));
        break;
    case SDLK_PERIOD:
        nudge_range_end_panel(app, pi,
            +range_step_panel(&app->panels[pi], mod & KMOD_SHIFT));
        break;

    case SDLK_0:
        set_range_panel(app, pi, 0, app->panels[pi].file.size);
        break;

    case SDLK_1: case SDLK_2: case SDLK_3: case SDLK_4:
    case SDLK_5: case SDLK_6: case SDLK_7: case SDLK_8:
    case SDLK_9: {
        int idx = (int)(k - SDLK_1);
        if (idx < app->panel_count) {
            app->active_panel = idx;
            if (app->mode == MODE_FOCUS) app->focus_panel = idx;
            app->needs_redraw = true;
        }
        break;
    }

    default: break;
    }
}

/* ---------- CLI ---------- */

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [OPTIONS] <file> [file2 ...]\n"
        "\n"
        "binmap - binary visualization tool (multi-file compare)\n"
        "\n"
        "Options:\n"
        "  -f, --focus       start in focus mode (single-file view)\n"
        "  -h, --help        show this help\n"
        "\n"
        "Files: up to %d may be given for side-by-side comparison.\n"
        "\n"
        "Keys:\n"
        "  TAB / SHIFT+TAB   next / previous view (linked across panels)\n"
        "  F                 toggle split / focus mode\n"
        "  H                 cycle overlay glow (off / similarity / difference)\n"
        "  1..9              select panel (focus mode: switch focused file)\n"
        "  CTRL+TAB          cycle to next panel\n"
        "  M                 toggle thumbnail strip (focus mode)\n"
        "  L                 toggle legend\n"
        "  D                 toggle view description\n"
        "  A                 toggle auto-rotate (3D views)\n"
        "  ARROWS            rotate (3D views)\n"
        "  + / - / WHEEL     zoom (3D views)\n"
        "  R                 reset 3D view\n"
        "  [ / ]             nudge active panel range start backward / forward\n"
        "  , / .             nudge active panel range end backward / forward\n"
        "  SHIFT+key         coarse range step\n"
        "  0                 reset active panel range to full file\n"
        "  MOUSE DRAG        adjust range via slider\n"
        "  ESC / Q           quit\n",
        prog, BINMAP_MAX_PANELS);
}

int main(int argc, char **argv)
{
    bool focus_start = false;
    int argi = 1;
    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        if (strcmp(argv[argi], "-f") == 0 || strcmp(argv[argi], "--focus") == 0) {
            focus_start = true; argi++;
        } else if (strcmp(argv[argi], "-h") == 0 || strcmp(argv[argi], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[argi], "--") == 0) {
            argi++; break;
        } else {
            fprintf(stderr, "binmap: unknown option '%s'\n", argv[argi]);
            print_usage(argv[0]);
            return 1;
        }
    }
    int file_count = argc - argi;
    if (file_count < 1) {
        print_usage(argv[0]);
        return 1;
    }
    if (file_count > BINMAP_MAX_PANELS) {
        fprintf(stderr, "binmap: too many files (%d given, max %d)\n",
                file_count, BINMAP_MAX_PANELS);
        return 1;
    }

    binmap_app_t app = {0};
    for (int i = 0; i < file_count; i++) {
        if (!load_file(&app.panels[i].file, argv[argi + i])) {
            for (int j = 0; j < i; j++) {
                if (app.panels[j].file.data)
                    munmap((void *)app.panels[j].file.data, app.panels[j].file.size);
            }
            return 1;
        }
        app.panels[i].range_start = 0;
        app.panels[i].range_end   = app.panels[i].file.size;
        app.panels[i].drag_mode   = DRAG_NONE;
    }
    app.panel_count      = file_count;
    app.active_panel     = 0;
    app.focus_panel      = 0;
    app.mode             = focus_start ? MODE_FOCUS : MODE_SPLIT;
    app.prev_mode        = app.mode;
    app.overlay_scheme   = 0;
    app.show_thumb_strip = false;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    /* Nearest-neighbour scaling for our pixel-perfect textures — must be
     * set before the textures are created so the hint is picked up. */
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

    app.show_legend      = true;
    app.show_description = false;
    app.current_view     = VIEW_BYTE_CLASS;
    app.needs_redraw     = true;
    app.yaw   = 0.6f;
    app.pitch = 0.35f;
    app.zoom  = 1.0f;
    app.auto_rotate = true;

    bool running = true;
    Uint32 last_tick = SDL_GetTicks();
    while (running) {
        bool animating = view_is_3d(app.current_view) && app.auto_rotate;
        for (int i = 0; i < app.panel_count && animating; i++)
            if (app.panels[i].drag_mode != DRAG_NONE) animating = false;

        if (app.needs_redraw) {
            redraw(&app);
            app.needs_redraw = false;
        }

        SDL_Event ev;
        bool got;
        if (animating) got = SDL_WaitEventTimeout(&ev, 16);
        else { got = SDL_WaitEvent(&ev); last_tick = SDL_GetTicks(); }

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
                    case SDL_WINDOWEVENT_LEAVE:
                        for (int i = 0; i < app.panel_count; i++)
                            app.panels[i].hover_has_offset = false;
                        app.needs_redraw = true;
                        break;
                    default: break;
                    }
                    break;
                case SDL_KEYDOWN:
                    handle_key(&app, ev.key.keysym.sym, ev.key.keysym.mod, &running); break;
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
                    handle_mouse_motion(&app, ev.motion.x, ev.motion.y);
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
            invalidate_overlay_3d_caches(&app);
            app.needs_redraw = true;
        }
    }

    invalidate_caches(&app);
    invalidate_overlay_caches(&app);
    for (int i = 0; i < app.panel_count; i++) {
        if (app.panels[i].minimap_tex) {
            SDL_DestroyTexture(app.panels[i].minimap_tex);
            app.panels[i].minimap_tex = NULL;
        }
    }
    if (app.renderer) SDL_DestroyRenderer(app.renderer);
    if (app.window)   SDL_DestroyWindow(app.window);
    SDL_Quit();
    for (int i = 0; i < app.panel_count; i++) {
        if (app.panels[i].file.data)
            munmap((void *)app.panels[i].file.data, app.panels[i].file.size);
    }
    return 0;
}
