#ifndef BINMAP_FONT_H
#define BINMAP_FONT_H

#include <SDL3/SDL.h>
#include <stdint.h>

#define FONT_W 5
#define FONT_H 7

void draw_text(SDL_Renderer *r, int x, int y, int scale, SDL_Color color, const char *text);
int  text_width(int scale, const char *text);

/* Draw text directly into an ARGB8888 pixel buffer (used by render passes that
 * bake labels into the cached view texture). */
void blit_text(uint32_t *pixels, int pw, int ph,
               int x, int y, int scale, uint32_t color, const char *text);

#endif
