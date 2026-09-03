#include "pet_gif_player.h"

#include "pet_gif_fs.h"

#include <cstring>
#include <cmath>

extern "C" {
#include "gifdec.h"
}

#include "esp_log.h"

namespace {

const char* TAG = "PetGifPlayer";

// 把 gd_GIF 当前帧(canvas RGBA)转为 RGB565 写入 buf。
// 透明像素(alpha==0)保留 buf 原底色，实现透明叠加到背景。
void CanvasToRGB565(gd_GIF* gif, uint16_t* buf, int w, int h) {
    const uint8_t* px = gif->canvas;
    for (int i = 0; i < w * h; i++) {
        if (px[i * 4 + 3] == 0x00) {
            continue;  // 透明：保留底色
        }
        uint8_t r = px[i * 4 + 0];
        uint8_t g = px[i * 4 + 1];
        uint8_t b = px[i * 4 + 2];
        uint16_t rgb565 = ((uint16_t)(r >> 3) << 11) | ((uint16_t)(g >> 2) << 5) | (uint16_t)(b >> 3);
        buf[i] = rgb565;
    }
}

// 统计帧数并取 fps（基于首帧 delay，单位 1/100s）。
int ProbeGif(gd_GIF* gif, int* out_fps) {
    int frames = 0;
    int fps = 10;
    int first_delay = -1;
    for (;;) {
        int r = gd_get_frame(gif);
        if (r <= 0) break;
        if (first_delay < 0) first_delay = gif->gce.delay;
        frames++;
        if (frames > 4096) break;
    }
    if (first_delay > 0) {
        int f = 100 / first_delay;
        if (f < 6) f = 6;
        if (f > 20) f = 20;
        *out_fps = f;
    }
    gd_rewind(gif);
    return frames;
}

}  // namespace

void PetGifPlayer::RegisterAll(PetAnimation* anim) {
    if (anim == nullptr) return;

    static const char* kNames[] = {
        "sad", "eat", "happy", "drink", "sleep", "cry", "laugh", "shy",
        "think", "surprise", "fight", "headpat", "bellyrub", "angry",
        "blink", "yum", "talk", "naughty", "walk", "kiss",
    };

    for (const char* name : kNames) {
        std::vector<uint8_t> buf = PetGifFs::LoadGif(name);
        if (buf.empty()) {
            ESP_LOGW(TAG, "gif '%s' not found in SPIFFS, skip", name);
            continue;
        }
        gd_GIF* gif = gd_open_gif_data(buf.data(), buf.size());
        if (gif == nullptr) {
            ESP_LOGE(TAG, "gif '%s' open failed", name);
            continue;
        }
        int frames = 0, fps = 10;
        frames = ProbeGif(gif, &fps);
        if (frames <= 0) {
            ESP_LOGE(TAG, "gif '%s' has no frames", name);
            gd_close_gif(gif);
            continue;
        }

        // 将缓冲与解码句柄交给动作对象，drawer 通过捕获指针顺序解帧。
        auto* action = new GifAction();
        action->name = name;
        action->data = std::move(buf);
        action->gif = gif;
        action->decoded = 0;
        anim->RegisterAction({action->name, frames, fps,
            [action](uint16_t* buf, int w, int h, int frame, int total) {
                gd_GIF* g = static_cast<gd_GIF*>(action->gif);
                if (frame == 0) {
                    gd_rewind(g);
                    action->decoded = 0;
                    gd_get_frame(g);
                } else if (frame <= action->decoded) {
                    gd_rewind(g);
                    action->decoded = 0;
                    for (int i = 0; i <= frame; i++) gd_get_frame(g);
                    action->decoded = frame;
                } else {
                    while (action->decoded < frame) {
                        int r = gd_get_frame(g);
                        if (r <= 0) {
                            gd_rewind(g);
                            action->decoded = 0;
                            for (int i = 0; i <= frame; i++) gd_get_frame(g);
                            action->decoded = frame;
                            break;
                        }
                        action->decoded++;
                    }
                }
                CanvasToRGB565(g, buf, w, h);
            }});
        ESP_LOGI(TAG, "registered gif action '%s' frames=%d fps=%d", name, frames, fps);
    }
}
