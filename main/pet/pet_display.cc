#include "pet_display.h"

#include "board.h"
#include "pet_gif_fs.h"
#include "pet_gif_player.h"

#include <esp_lcd_panel_ops.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstring>
#include <cmath>

// 宠物图片资源（由 scripts/convert_pet_image.py 生成，编入 main/pet/assets/*.c）
extern const uint16_t g_pet_idle[];
extern const uint16_t g_pet_blink[];
extern const uint16_t g_pet_happy[];
extern const uint16_t g_pet_sad[];
extern const uint16_t g_pet_eat[];
extern const uint16_t g_pet_think[];

#define TAG "PetDisplay"

static constexpr int kImgW = 240;
static constexpr int kImgH = 320;

// 将 240x320 源图完整复制到输出 buffer（w/h 应等于源图尺寸）
static void BlitImage(const uint16_t* src, uint16_t* buf, int w, int h) {
    if (w == kImgW && h == kImgH) {
        std::memcpy(buf, src, kImgW * kImgH * sizeof(uint16_t));
        return;
    }
    for (int y = 0; y < h; y++) {
        int sy = (y * kImgH) / h;
        for (int x = 0; x < w; x++) {
            int sx = (x * kImgW) / w;
            buf[y * w + x] = src[sy * kImgW + sx];
        }
    }
}

// 带呼吸亮度变化的源图复制
static void BlitImageBreathing(const uint16_t* src, uint16_t* buf, int w, int h, float k) {
    for (int y = 0; y < h; y++) {
        int sy = (y * kImgH) / h;
        for (int x = 0; x < w; x++) {
            int sx = (x * kImgW) / w;
            uint16_t p = src[sy * kImgW + sx];
            uint16_t r = (uint16_t)(((p >> 11) & 0x1F) * k);
            uint16_t g = (uint16_t)(((p >> 5) & 0x3F) * k);
            uint16_t b = (uint16_t)((p & 0x1F) * k);
            buf[y * w + x] = (r << 11) | (g << 5) | b;
        }
    }
}

PetDisplay::PetDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                       int width, int height, int offset_x, int offset_y, bool mirror_x,
                       bool mirror_y, bool swap_xy)
    : panel_io_(panel_io), panel_(panel) {
    width_ = width;
    height_ = height;

    // 单个内部 SRAM（DMA-capable）行缓冲，写法与已验证可用的测试项目完全一致。
    // esp_lcd_panel_draw_bitmap 在未设置 on_color_trans_done 时是同步的：
    // 内部 queue 后调用 get_trans_result 阻塞等待完成，因此不会与下一次 memcpy
    // 产生冲突，不需要 ping-pong，也不需要从 PSRAM 多拷一层 back_buffer。
    size_t row_bytes = static_cast<size_t>(width_) * sizeof(uint16_t);
    row_buffer_ = static_cast<uint16_t*>(
        heap_caps_malloc(row_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (row_buffer_ == nullptr) {
        ESP_LOGE(TAG, "row_buffer DMA alloc failed (%u bytes)", (unsigned)row_bytes);
        return;
    }
    ESP_LOGI(TAG, "row_buffer ok: %u bytes in internal SRAM", (unsigned)row_bytes);

    // offset / mirror / swap are already applied to the panel by the board
    // before constructing us; we only need to paint a black background once.
    InitPanel();

    // turn the panel on; otherwise the screen stays in display-off state.
    esp_err_t on_err = esp_lcd_panel_disp_on_off(panel_, true);
    if (on_err != ESP_OK && on_err != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGE(TAG, "disp_on_off failed: %s", esp_err_to_name(on_err));
    }

    anim_ = std::make_unique<PetAnimation>(width_, height_);
    anim_->SetFrameSink([this](const uint16_t* buf, int w, int h) { FlushFrame(buf, w, h); });

    // ---- 宠物 GIF 动作 ----
    // 20 个动作来自 /assets/gif/*.gif（SPIFFS），由 PetGifPlayer 读取并解码注册。
    // GIF 先注册，按名查找时优先命中 GIF 版。
    if (PetGifFs::Mount()) {
        PetGifPlayer::RegisterAll(anim_.get());
    } else {
        ESP_LOGE(TAG, "SPIFFS mount failed, no GIF actions available");
    }

    // ---- 真实猫图动作（仅保留无对应 GIF 的动作）----
    // idle：常态猫图直接贴图，不做呼吸（避免静止时整屏明暗闪烁）
    anim_->RegisterAction({"idle", 1, 1, [](uint16_t* buf, int w, int h, int frame, int total) {
        BlitImage(g_pet_idle, buf, w, h);
    }});
    // comfort：缓慢呼吸（常态图，暂无对应 GIF）
    anim_->RegisterAction({"comfort", 30, 12, [](uint16_t* buf, int w, int h, int frame, int total) {
        float k = 0.8f + 0.2f * sinf(frame * 0.4f);
        BlitImageBreathing(g_pet_idle, buf, w, h, k);
    }});

    // test：纯色全屏（用于校验 RGB 顺序）。分别提供 red/green/blue。
    anim_->RegisterAction({"test_red", 1, 1, [](uint16_t* buf, int w, int h, int frame, int total) {
        for (int i = 0; i < w * h; i++) buf[i] = 0xF800;  // R5=31
    }});
    anim_->RegisterAction({"test_green", 1, 1, [](uint16_t* buf, int w, int h, int frame, int total) {
        for (int i = 0; i < w * h; i++) buf[i] = 0x07E0;  // G6=63
    }});
    anim_->RegisterAction({"test_blue", 1, 1, [](uint16_t* buf, int w, int h, int frame, int total) {
        for (int i = 0; i < w * h; i++) buf[i] = 0x001F;  // B5=31
    }});

    // 开机先静态出一张猫图，5 秒后再启动动画。
    // 这样一次烧录就能分辨：静态显示是否正常 / 动画起来后才花屏。
    ESP_LOGI(TAG, "=== drawing STATIC idle frame (please look at the screen now) ===");
    int64_t t0 = esp_timer_get_time();
    FlushFrame(g_pet_idle, kImgW, kImgH);
    int64_t t1 = esp_timer_get_time();
    ESP_LOGI(TAG, "STATIC frame flushed, took %lld ms", (long long)((t1 - t0) / 1000));

    StartAnimationAfterBoot();
}

void PetDisplay::StartAnimationAfterBoot() {
    xTaskCreate(BootAnimationTask, "pet_boot", 3072, this, 5, nullptr);
}

void PetDisplay::BootAnimationTask(void* arg) {
    auto* self = static_cast<PetDisplay*>(arg);
    vTaskDelay(pdMS_TO_TICKS(5000));
    ESP_LOGI(TAG, "=== animation starts now (compare with the first 5s static picture) ===");
    if (self->anim_ != nullptr) {
        self->anim_->Play("idle");
    }
    vTaskDelete(nullptr);
}

PetDisplay::~PetDisplay() {
    if (emotion_timer_ != nullptr) {
        esp_timer_stop(emotion_timer_);
        esp_timer_delete(emotion_timer_);
        emotion_timer_ = nullptr;
    }
    if (row_buffer_ != nullptr) {
        heap_caps_free(row_buffer_);
        row_buffer_ = nullptr;
    }
}

void PetDisplay::InitPanel() {
    // 清屏用逐行方式，避免单次整帧 150KB 传输超出 SPI DMA 内部 SRAM 限制。
    if (row_buffer_ == nullptr) {
        ESP_LOGE(TAG, "InitPanel: row buffer not allocated");
        return;
    }
    for (size_t i = 0; i < static_cast<size_t>(width_); i++) row_buffer_[i] = 0x0000;
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, row_buffer_);
    }
    ESP_LOGI(TAG, "InitPanel: black background done");
}

void PetDisplay::FlushFrame(const uint16_t* buf, int w, int h) {
    if (buf == nullptr || w <= 0 || h <= 0) {
        return;
    }
    if (row_buffer_ == nullptr) {
        ESP_LOGE(TAG, "FlushFrame: row buffer not allocated");
        return;
    }

    // 逐行：把源帧的一行"字节交换后"拷进内部 SRAM 行缓冲，再 draw_bitmap 发出。
    //
    // 关键：ESP32 是小端，uint16 0xF800 在内存里是 [00][F8]，SPI 按内存顺序原样发出；
    // 而 ST7789 在 16bpp 下按"高字节在前"组装像素，直接 memcpy 会让每个像素高低字节
    // 颠倒 —— 实测写入纯红显示蓝色、纯绿显示红色、纯蓝显示绿色，真实图片则整幅变
    // 彩色噪声（即困扰已久的"花屏"）。因此这里必须逐像素交换高低字节。
    // 交换后：内存中为 [F8][00]，SPI 发出 F8,00，屏组装回 0xF800，颜色即正确。
    for (int y = 0; y < h; y++) {
        const uint16_t* src_row = buf + static_cast<size_t>(y) * w;
        for (int x = 0; x < w; x++) {
            uint16_t v = src_row[x];
            row_buffer_[x] = static_cast<uint16_t>((v << 8) | (v >> 8));
        }
        esp_err_t err = esp_lcd_panel_draw_bitmap(panel_, 0, y, w, y + 1, row_buffer_);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "FlushFrame row %d failed: %s", y, esp_err_to_name(err));
            return;
        }
    }
}

bool PetDisplay::PlayAction(const std::string& action) {
    if (anim_ == nullptr) {
        return false;
    }
    anim_->Play(action);
    return true;
}

void PetDisplay::SetEmotion(const char* emotion) {
    ESP_LOGI("PetDisplay", "SetEmotion called with: '%s'", emotion ? emotion : "(null)");
    if (anim_ == nullptr) {
        return;
    }
    std::string e(emotion ? emotion : "");
    std::string action = "talk";  // 默认为会动的兜底动作，保证每句话都有反应
    if (e == "happy" || e == "smile" || e == "love" || e == "laugh" || e == "laughing") {
        action = "happy";
    } else if (e == "sad" || e == "cry" || e == "crying" || e == "unhappy") {
        action = "sad";
    } else if (e == "angry" || e == "mad") {
        action = "angry";
    } else if (e == "surprise" || e == "surprised" || e == "shock") {
        action = "surprise";
    } else if (e == "think" || e == "thinking" || e == "confused") {
        action = "think";
    } else if (e == "blink" || e == "wink") {
        action = "blink";
    } else if (e == "eat" || e == "yum") {
        action = "eat";
    } else if (e == "drink" || e == "thirsty") {
        action = "drink";
    } else if (e == "sleep" || e == "sleepy") {
        action = "sleep";
    } else if (e == "cry" || e == "crying") {
        action = "cry";
    } else if (e == "laugh" || e == "laughing") {
        action = "laugh";
    } else if (e == "shy" || e == "embarrassed") {
        action = "shy";
    } else if (e == "fight" || e == "hit") {
        action = "fight";
    } else if (e == "headpat" || e == "pat") {
        action = "headpat";
    } else if (e == "bellyrub" || e == "tummy") {
        action = "bellyrub";
    } else if (e == "kiss" || e == "mua") {
        action = "kiss";
    } else if (e == "naughty" || e == "naughty") {
        action = "naughty";
    } else if (e == "walk" || e == "go") {
        action = "walk";
    } else if (e == "comfort" || e == "shy") {
        action = "comfort";
    } else if (e == "test_red") {
        action = "test_red";
    } else if (e == "test_green") {
        action = "test_green";
    } else if (e == "test_blue") {
        action = "test_blue";
    } else if (e == "idle" || e == "sleep" || e == "standby") {
        // 明确要静止时才用静态图
        action = "idle";
    }
    // 其余（neutral / 未知词 / 空）保持 "talk"，即轻微呼吸，画面不会死板

    if (action != "idle") {
        // 表情动作播约 5 秒后自动回到 idle 猫图
        anim_->Play(action);
        ScheduleEmotionReturn();
    } else {
        anim_->Play("idle");
    }
}

void PetDisplay::ScheduleEmotionReturn() {
    if (emotion_timer_ != nullptr) {
        esp_timer_stop(emotion_timer_);
    } else {
        esp_timer_create_args_t args = {};
        args.callback = &PetDisplay::EmotionReturnCallback;
        args.arg = this;
        args.name = "pet_emotion_return";
        esp_timer_create(&args, &emotion_timer_);
    }
    esp_timer_start_once(emotion_timer_, 5000 * 1000);  // 5s 后回到 idle

}

void PetDisplay::EmotionReturnCallback(void* arg) {
    auto* self = static_cast<PetDisplay*>(arg);
    if (self->anim_ != nullptr) {
        self->anim_->Play("idle");
    }
}
