#ifndef BINMAP_H
#define BINMAP_H

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    VIEW_BYTE_CLASS = 0,
    VIEW_HILBERT,
    VIEW_DIGRAPH,
    VIEW_MARKOV_CHORD,
    VIEW_ENTROPY,
    VIEW_AUTOCORRELATION,
    VIEW_STRINGS_DENSITY,
    VIEW_SELF_SIMILARITY,
    VIEW_BIT_PLANE,
    VIEW_HISTOGRAM,
    VIEW_MORTON,
    VIEW_RGB_RAW,
    VIEW_POLAR,
    VIEW_CONCENTRIC_RINGS,
    VIEW_CIRCULAR_HILBERT,
    VIEW_CYLINDRICAL,
    VIEW_HELICAL,
    VIEW_TORUS,
    VIEW_TRIGRAPH,
    VIEW_TRIGRAPH_SPHERICAL,
    VIEW_COUNT
} view_id_t;

typedef struct {
    const uint8_t *data;
    size_t size;
    const char *path;
} binmap_file_t;

typedef struct {
    SDL_Window  *window;
    SDL_Renderer *renderer;
    int win_w, win_h;
    view_id_t current_view;
    binmap_file_t file;
    SDL_Texture *cache[VIEW_COUNT];
    int cache_w, cache_h;
    bool show_legend;
    bool needs_redraw;
    /* 3D view state */
    float yaw;
    float pitch;
    float zoom;
    bool  auto_rotate;
} binmap_app_t;

extern const char *view_names[VIEW_COUNT];

bool view_is_3d(view_id_t v);

/* 2D renderers */
void render_byte_class(uint32_t *pixels, int w, int h, const uint8_t *data, size_t size);
void render_hilbert   (uint32_t *pixels, int w, int h, const uint8_t *data, size_t size);
void render_digraph   (uint32_t *pixels, int w, int h, const uint8_t *data, size_t size);
void render_entropy   (uint32_t *pixels, int w, int h, const uint8_t *data, size_t size);
void render_bit_plane (uint32_t *pixels, int w, int h, const uint8_t *data, size_t size);
void render_histogram (uint32_t *pixels, int w, int h, const uint8_t *data, size_t size);
void render_morton    (uint32_t *pixels, int w, int h, const uint8_t *data, size_t size);
void render_polar     (uint32_t *pixels, int w, int h, const uint8_t *data, size_t size);
void render_concentric_rings(uint32_t *pixels, int w, int h,
                             const uint8_t *data, size_t size);
void render_circular_hilbert(uint32_t *pixels, int w, int h,
                             const uint8_t *data, size_t size);
void render_autocorrelation(uint32_t *pixels, int w, int h,
                            const uint8_t *data, size_t size);
void render_strings_density(uint32_t *pixels, int w, int h,
                            const uint8_t *data, size_t size);
void render_self_similarity(uint32_t *pixels, int w, int h,
                            const uint8_t *data, size_t size);
void render_markov_chord   (uint32_t *pixels, int w, int h,
                            const uint8_t *data, size_t size);
void render_rgb_raw        (uint32_t *pixels, int w, int h,
                            const uint8_t *data, size_t size);

/* 3D renderers (yaw/pitch in radians) */
void render_cylindrical(uint32_t *pixels, int w, int h, const uint8_t *data, size_t size,
                        float yaw, float pitch, float zoom);
void render_helical    (uint32_t *pixels, int w, int h, const uint8_t *data, size_t size,
                        float yaw, float pitch, float zoom);
void render_torus      (uint32_t *pixels, int w, int h, const uint8_t *data, size_t size,
                        float yaw, float pitch, float zoom);
void render_trigraph   (uint32_t *pixels, int w, int h, const uint8_t *data, size_t size,
                        float yaw, float pitch, float zoom);
void render_trigraph_spherical(uint32_t *pixels, int w, int h,
                               const uint8_t *data, size_t size,
                               float yaw, float pitch, float zoom);

#endif
