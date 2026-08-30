#ifndef PET_DISPLAY_H
#define PET_DISPLAY_H

#include "display.h"
#include "pet_animation.h"

#include <esp_heap_caps.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>

#include <cstring>
#include <mutex>
#include <string>

// PetDisplay owns the LCD panel exclusively for the pet animation.
// It intentionally does NOT inherit LcdDisplay / register an LVGL display
// device, so all LVGL-dependent paths (SetTheme/SetTextFont/etc) are skipped
// and cannot crash on uninitialized LVGL objects.
class PetDisplay : public Display {
public:
    PetDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
               int width, int height, int offset_x, int offset_y, bool mirror_x,
               bool mirror_y, bool swap_xy);
    ~PetDisplay() override;

    void Play(const std::string& name) { anim_->Play(name); }
    bool PlayAction(const std::string& action);
    void SetEmotion(const char* emotion) override;
    void SetChatMessage(const char* role, const char* content) override {}
    void ShowNotification(const char* notification, int duration_ms) override {}
    void SetTheme(Theme* theme) override {}
    void UpdateStatusBar(bool update_all = false) override {}
    void SetStatus(const char* status) override {}
    void ClearChatMessages() override {}

protected:
    bool Lock(int timeout_ms = 0) override { return true; }
    void Unlock() override {}
    void SetupUI() override {}

private:
    void InitPanel();
    void FlushFrame(const uint16_t* buf, int w, int h);
    void ScheduleEmotionReturn();
    static void EmotionReturnCallback(void* arg);
    // 开机先静态出图，延时后再启动动画：一次烧录即可分辨"静态"与"动画"两条路径
    void StartAnimationAfterBoot();
    static void BootAnimationTask(void* arg);

    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    std::unique_ptr<PetAnimation> anim_;
    esp_timer_handle_t emotion_timer_ = nullptr;
    // 单个内部 SRAM 行缓冲（与已验证可用的测试项目写法一致）。
    // esp_lcd_panel_draw_bitmap 未设置 on_color_trans_done 时是同步阻塞的，
    // 因此不需要 ping-pong 双缓冲，也不需要 PSRAM back_buffer 中间层。
    uint16_t* row_buffer_ = nullptr;
};

#endif  // PET_DISPLAY_H
