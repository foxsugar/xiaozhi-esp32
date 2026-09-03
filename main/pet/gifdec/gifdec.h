#ifndef PET_GIFDEC_H
#define PET_GIFDEC_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gd_Palette {
    int size;
    uint8_t colors[0x100 * 3];
} gd_Palette;

typedef struct gd_GCE {
    uint16_t delay;
    uint8_t tindex;
    uint8_t transparency;
    int disposal;
} gd_GCE;

typedef struct gd_GIF {
    int width, height, depth;
    uint8_t *frame_data;     // decoded palette-index frame (w*h)
    uint8_t *canvas;         // accumulated RGBA canvas (w*h*4)
    gd_Palette *palette;
    gd_Palette lct, gct;
    gd_GCE gce;
    uint8_t bgindex;
    int fx, fy, fw, fh;
    int anim_start;
    uint8_t *data;           // raw gif bytes (in-memory)
    size_t data_pos;
    size_t data_size;
    int loop_count;
    gd_Palette *(*plain_text)(struct gd_GIF *gif, int tx, int ty, int tw, int th, int cw, int ch, int fg, int bg);
    void (*comment)(struct gd_GIF *gif);
    void (*application)(struct gd_GIF *gif, char *id, char *auth);
} gd_GIF;

// Open a GIF from an in-memory buffer (no filesystem dependency).
gd_GIF *gd_open_gif_data(const void *data, size_t size);
// Advance to next frame. Returns 1 if a frame was read, 0 at loop end, -1 on error.
int gd_get_frame(gd_GIF *gif);
// Render current frame into an RGBA buffer (w*h*4). Use gd_GIF->canvas after get_frame.
void gd_render_frame(gd_GIF *gif, uint8_t *buffer);
void gd_rewind(gd_GIF *gif);
void gd_close_gif(gd_GIF *gif);

#ifdef __cplusplus
}
#endif

#endif // PET_GIFDEC_H
