#ifndef BINMAP_H
#define BINMAP_H

#include <SDL2/SDL.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    VIEW_BYTE_CLASS = 0,
    VIEW_HILBERT,
    VIEW_DIGRAPH,
    VIEW_ENTROPY,
    VIEW_STRINGS_DENSITY,
    VIEW_SELF_SIMILARITY,
    VIEW_BIT_PLANE,
    VIEW_RGB_RAW,
    VIEW_TRIGRAPH,
    VIEW_TRIGRAPH_SPHERICAL,
    VIEW_COUNT
} view_id_t;

typedef struct {
    const uint8_t *data;
    size_t size;
    const char *path;
} binmap_file_t;

typedef enum {
    DRAG_NONE = 0,
    DRAG_START,
    DRAG_END,
    DRAG_MIDDLE
} drag_mode_t;

typedef struct {
    SDL_Window  *window;
    SDL_Renderer *renderer;
    int win_w, win_h;
    view_id_t current_view;
    binmap_file_t file;
    SDL_Texture *cache[VIEW_COUNT];
    int cache_w, cache_h;
    bool show_legend;
    bool show_description;
    bool needs_redraw;
    /* 3D view state */
    float yaw;
    float pitch;
    float zoom;
    bool  auto_rotate;
    /* range slider state */
    size_t range_start;   /* inclusive */
    size_t range_end;     /* exclusive */
    drag_mode_t drag_mode;
    size_t drag_anchor_start;
    size_t drag_anchor_end;
    int    drag_anchor_px;
    /* whole-file minimap texture (1px tall byte_class strip stretched to track width) */
    SDL_Texture *minimap_tex;
    int          minimap_w;
} binmap_app_t;

extern const char *view_names[VIEW_COUNT];
/* Multi-line per-view description. Each entry is a NULL-terminated array of
 * lines; the first line is treated as a subtitle for the popup. */
extern const char * const *view_descriptions[VIEW_COUNT];

bool view_is_3d(view_id_t v);

/* 2D renderers. base_offset is the absolute file offset of data[0]; used by
 * label overlays to render true file offsets even when a sub-range is shown. */
void render_byte_class(uint32_t *pixels, int w, int h, const uint8_t *data, size_t size,
                       size_t base_offset, size_t file_size);
void render_hilbert   (uint32_t *pixels, int w, int h, const uint8_t *data, size_t size,
                       size_t base_offset, size_t file_size);
void render_digraph   (uint32_t *pixels, int w, int h, const uint8_t *data, size_t size,
                       size_t base_offset, size_t file_size);
void render_entropy   (uint32_t *pixels, int w, int h, const uint8_t *data, size_t size,
                       size_t base_offset, size_t file_size);
void render_bit_plane (uint32_t *pixels, int w, int h, const uint8_t *data, size_t size,
                       size_t base_offset, size_t file_size);
void render_strings_density(uint32_t *pixels, int w, int h,
                            const uint8_t *data, size_t size,
                            size_t base_offset, size_t file_size);
void render_self_similarity(uint32_t *pixels, int w, int h,
                            const uint8_t *data, size_t size,
                            size_t base_offset, size_t file_size);
void render_rgb_raw        (uint32_t *pixels, int w, int h,
                            const uint8_t *data, size_t size,
                            size_t base_offset, size_t file_size);

/* 3D renderers (yaw/pitch in radians) */
void render_trigraph   (uint32_t *pixels, int w, int h, const uint8_t *data, size_t size,
                        size_t base_offset, size_t file_size,
                        float yaw, float pitch, float zoom);
void render_trigraph_spherical(uint32_t *pixels, int w, int h,
                               const uint8_t *data, size_t size,
                               size_t base_offset, size_t file_size,
                               float yaw, float pitch, float zoom);

#endif
