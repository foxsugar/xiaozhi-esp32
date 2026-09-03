#include "gifdec.h"

#include <stdlib.h>
#include <string.h>

#define MIN(A, B) ((A) < (B) ? (A) : (B))
#define MAX(A, B) ((A) > (B) ? (A) : (B))

typedef struct Entry {
    uint16_t length;
    uint16_t prefix;
    uint8_t  suffix;
} Entry;

typedef struct Table {
    int bulk;
    int nentries;
    Entry *entries;
} Table;

static uint16_t read_num(gd_GIF *gif) {
    uint8_t bytes[2];
    gif->data_pos += 2;
    memcpy(bytes, gif->data + gif->data_pos - 2, 2);
    return bytes[0] + (((uint16_t) bytes[1]) << 8);
}

static void mem_read(gd_GIF *gif, void *buf, size_t len) {
    memcpy(buf, gif->data + gif->data_pos, len);
    gif->data_pos += len;
}

static int mem_seek(gd_GIF *gif, size_t pos, int k) {
    if (k == SEEK_CUR) gif->data_pos += pos;
    else if (k == SEEK_SET) gif->data_pos = pos;
    return (int) gif->data_pos;
}

static gd_GIF *gif_open(gd_GIF *gif_base) {
    uint8_t sigver[3];
    uint16_t width, height, depth;
    uint8_t fdsz, bgidx, aspect;
    uint8_t *bgcolor;
    int gct_sz;
    gd_GIF *gif = NULL;

    mem_read(gif_base, sigver, 3);
    if (memcmp(sigver, "GIF", 3) != 0) return NULL;
    mem_read(gif_base, sigver, 3);
    if (memcmp(sigver, "89a", 3) != 0 && memcmp(sigver, "87a", 3) != 0) return NULL;
    width  = read_num(gif_base);
    height = read_num(gif_base);
    mem_read(gif_base, &fdsz, 1);
    if (!(fdsz & 0x80)) return NULL;
    depth = ((fdsz >> 4) & 7) + 1;
    gct_sz = 1 << ((fdsz & 0x07) + 1);
    mem_read(gif_base, &bgidx, 1);
    mem_read(gif_base, &aspect, 1);

    if (0 == width || 0 == height) return NULL;
    gif = malloc(sizeof(gd_GIF) + 5 * width * height);
    if (!gif) return NULL;
    memcpy(gif, gif_base, sizeof(gd_GIF));
    gif->width  = width;
    gif->height = height;
    gif->depth  = depth;
    gif->frame_data = malloc(width * height);
    if (!gif->frame_data) { free(gif); return NULL; }
    gif->canvas = (uint8_t *) &gif[1];
    gif->palette = &gif->gct;
    gif->bgindex = bgidx;
    mem_read(gif, gif->gct.colors, 3 * gif->gct.size);
    gif->gct.size = gct_sz;
    bgcolor = &gif->palette->colors[gif->bgindex * 3];
    for (int i = 0; i < width * height; i++) {
        gif->canvas[i * 4 + 0] = *(bgcolor + 2);
        gif->canvas[i * 4 + 1] = *(bgcolor + 1);
        gif->canvas[i * 4 + 2] = *(bgcolor + 0);
        gif->canvas[i * 4 + 3] = 0x00;
    }
    gif->anim_start = mem_seek(gif, 0, SEEK_CUR);
    gif->loop_count = -1;
    return gif;
}

static void discard_sub_blocks(gd_GIF *gif) {
    uint8_t size;
    do {
        mem_read(gif, &size, 1);
        mem_seek(gif, size, SEEK_CUR);
    } while (size);
}

gd_GIF *gd_open_gif_data(const void *data, size_t size) {
    gd_GIF gif_base;
    memset(&gif_base, 0, sizeof(gif_base));
    gif_base.data = (uint8_t *) data;
    gif_base.data_pos = 0;
    gif_base.data_size = size;
    return gif_open(&gif_base);
}

static void read_graphic_control_ext(gd_GIF *gif) {
    uint8_t rdit;
    mem_seek(gif, 1, SEEK_CUR);
    mem_read(gif, &rdit, 1);
    gif->gce.disposal = (rdit >> 2) & 3;
    gif->gce.input = rdit & 2;
    gif->gce.transparency = rdit & 1;
    gif->gce.delay = read_num(gif);
    mem_read(gif, &gif->gce.tindex, 1);
    mem_seek(gif, 1, SEEK_CUR);
}

static void read_ext(gd_GIF *gif) {
    uint8_t label;
    mem_read(gif, &label, 1);
    switch (label) {
        case 0x01: discard_sub_blocks(gif); break;
        case 0xF9: read_graphic_control_ext(gif); break;
        case 0xFE: discard_sub_blocks(gif); break;
        case 0xFF: discard_sub_blocks(gif); break;
        default: break;
    }
}

static uint16_t get_key(gd_GIF *gif, int key_size, uint8_t *sub_len, uint8_t *shift, uint8_t *byte) {
    int bits_read, rpad, frag_size;
    uint16_t key;
    key = 0;
    for (bits_read = 0; bits_read < key_size; bits_read += frag_size) {
        rpad = (*shift + bits_read) % 8;
        if (rpad == 0) {
            if (*sub_len == 0) {
                mem_read(gif, sub_len, 1);
                if (*sub_len == 0) return 0x1000;
            }
            mem_read(gif, byte, 1);
            (*sub_len)--;
        }
        frag_size = MIN(key_size - bits_read, 8 - rpad);
        key |= ((uint16_t) ((*byte) >> rpad)) << bits_read;
    }
    key &= (1 << key_size) - 1;
    *shift = (*shift + key_size) % 8;
    return key;
}

static int read_image_data(gd_GIF *gif, int interlace) {
    uint8_t sub_len, shift, byte;
    int init_key_size, key_size, table_is_full = 0;
    int frm_off, frm_size, str_len = 0, i, p, x, y;
    uint16_t key, clear, stop;
    Table *table;
    Entry entry = {0};
    size_t start, end;

    mem_read(gif, &byte, 1);
    key_size = (int) byte;
    start = mem_seek(gif, 0, SEEK_CUR);
    discard_sub_blocks(gif);
    end = mem_seek(gif, 0, SEEK_CUR);
    mem_seek(gif, start, SEEK_SET);
    clear = 1 << key_size;
    stop = clear + 1;
    table = malloc(sizeof(*table) + sizeof(Entry) * (MAX(1 << (key_size + 1), 0x100)));
    if (!table) return -1;
    table->bulk = MAX(1 << (key_size + 1), 0x100);
    table->nentries = (1 << key_size) + 2;
    table->entries = (Entry *) &table[1];
    for (int key = 0; key < (1 << key_size); key++)
        table->entries[key] = (Entry){1, 0xFFF, (uint8_t) key};
    key_size++;
    init_key_size = key_size;
    sub_len = shift = 0;
    key = get_key(gif, key_size, &sub_len, &shift, &byte);
    frm_off = 0;
    frm_size = gif->fw * gif->fh;
    while (frm_off < frm_size) {
        if (key == clear) {
            key_size = init_key_size;
            table->nentries = (1 << (key_size - 1)) + 2;
            table_is_full = 0;
        } else if (!table_is_full) {
            int ret = 0;
            if (table->nentries == table->bulk) {
                table->bulk *= 2;
                table = realloc(table, sizeof(*table) + sizeof(Entry) * table->bulk);
                if (!table) return -1;
                table->entries = (Entry *) &table[1];
            }
            table->entries[table->nentries] = (Entry){str_len + 1, key, entry.suffix};
            table->nentries++;
            if ((table->nentries & (table->nentries - 1)) == 0) key_size++;
        }
        key = get_key(gif, key_size, &sub_len, &shift, &byte);
        if (key == clear) continue;
        if (key == stop || key == 0x1000) break;
        entry = table->entries[key];
        str_len = entry.length;
        if (frm_off + str_len > frm_size) { free(table); return -1; }
        for (i = 0; i < str_len; i++) {
            p = frm_off + entry.length - 1;
            x = p % gif->fw;
            y = p / gif->fw;
            if (interlace) {
                int h = gif->fh;
                int passes[4] = {(h - 1) / 8 + 1, (h - 5) / 8 + 1, (h - 3) / 4 + 1, 0};
                int py = y;
                if (py < passes[0]) y = py * 8;
                else if ((py -= passes[0]) < passes[1]) y = py * 8 + 4;
                else if ((py -= passes[1]) < passes[2]) y = py * 4 + 2;
                else y = py * 2 + 1;
            }
            gif->frame_data[(gif->fy + y) * gif->width + gif->fx + x] = entry.suffix;
            if (entry.prefix == 0xFFF) break;
            else entry = table->entries[entry.prefix];
        }
        frm_off += str_len;
    }
    free(table);
    if (key == stop) mem_read(gif, &sub_len, 1);
    mem_seek(gif, end, SEEK_SET);
    return 0;
}

static int read_image(gd_GIF *gif) {
    uint8_t fisrz;
    int interlace;
    gif->fx = read_num(gif);
    gif->fy = read_num(gif);
    gif->fw = read_num(gif);
    gif->fh = read_num(gif);
    if (gif->fx + (uint32_t) gif->fw > gif->width || gif->fy + (uint32_t) gif->fh > gif->height) return -1;
    mem_read(gif, &fisrz, 1);
    interlace = fisrz & 0x40;
    if (fisrz & 0x80) {
        gif->lct.size = 1 << ((fisrz & 0x07) + 1);
        mem_read(gif, gif->lct.colors, 3 * gif->lct.size);
        gif->palette = &gif->lct;
    } else {
        gif->palette = &gif->gct;
    }
    return read_image_data(gif, interlace);
}

static void render_frame_rect(gd_GIF *gif) {
    int i = gif->fy * gif->width + gif->fx;
    int j, k;
    uint8_t index, *color;
    for (j = 0; j < gif->fh; j++) {
        for (k = 0; k < gif->fw; k++) {
            index = gif->frame_data[(gif->fy + j) * gif->width + gif->fx + k];
            color = &gif->palette->colors[index * 3];
            if (!gif->gce.transparency || index != gif->gce.tindex) {
                gif->canvas[(i + k) * 4 + 0] = *(color + 2);
                gif->canvas[(i + k) * 4 + 1] = *(color + 1);
                gif->canvas[(i + k) * 4 + 2] = *(color + 0);
                gif->canvas[(i + k) * 4 + 3] = 0xFF;
            }
        }
        i += gif->width;
    }
}

static void dispose(gd_GIF *gif) {
    int i;
    uint8_t *bgcolor;
    switch (gif->gce.disposal) {
        case 2:
            bgcolor = &gif->palette->colors[gif->bgindex * 3];
            i = gif->fy * gif->width + gif->fx;
            for (int j = 0; j < gif->fh; j++) {
                for (int k = 0; k < gif->fw; k++) {
                    gif->canvas[(i + k) * 4 + 0] = *(bgcolor + 2);
                    gif->canvas[(i + k) * 4 + 1] = *(bgcolor + 1);
                    gif->canvas[(i + k) * 4 + 2] = *(bgcolor + 0);
                    gif->canvas[(i + k) * 4 + 3] = gif->gce.transparency ? 0x00 : 0xFF;
                }
                i += gif->width;
            }
            break;
        case 3: break;
        default: render_frame_rect(gif); break;
    }
}

int gd_get_frame(gd_GIF *gif) {
    char sep;
    dispose(gif);
    mem_read(gif, &sep, 1);
    while (sep != ',') {
        if (sep == ';') {
            mem_seek(gif, gif->anim_start, SEEK_SET);
            if (gif->loop_count == 1 || gif->loop_count < 0) return 0;
            else if (gif->loop_count > 1) gif->loop_count--;
        } else if (sep == '!')
            read_ext(gif);
        else return -1;
        mem_read(gif, &sep, 1);
    }
    if (read_image(gif) == -1) return -1;
    return 1;
}

void gd_render_frame(gd_GIF *gif, uint8_t *buffer) {
    memcpy(buffer, gif->canvas, (size_t) gif->width * gif->height * 4);
}

void gd_rewind(gd_GIF *gif) {
    gif->loop_count = -1;
    mem_seek(gif, gif->anim_start, SEEK_SET);
}

void gd_close_gif(gd_GIF *gif) {
    if (!gif) return;
    if (gif->frame_data) free(gif->frame_data);
    free(gif);
}
