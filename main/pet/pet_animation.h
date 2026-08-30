#ifndef PET_ANIMATION_H
#define PET_ANIMATION_H

#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

// 宠物动作播放引擎（端侧）。
// 动画在独立 FreeRTOS 任务中逐帧绘制并刷屏（不在 esp_timer ISR 中），
// 因为 esp_lcd_panel_draw_bitmap 内部走 SPI 传输，不能在 ISR 安全调用。
class PetAnimation {
public:
    // 一个动作的帧绘制回调：给定帧索引 frame 与总帧数 total，
    // 在 (w,h) 的 RGB565 缓冲 buf 上绘制一帧。
    using FrameDrawer = std::function<void(uint16_t* buf, int w, int h, int frame, int total)>;

    struct Action {
        std::string name;       // 动作名，如 "idle" / "happy" / "comfort"
        int frame_count;        // 帧数
        int fps;                // 播放帧率
        FrameDrawer drawer;     // 帧绘制函数
    };

    PetAnimation(int width, int height);
    ~PetAnimation();

    // 每帧绘制完成后，由持有者（PetDisplay）负责刷屏到 panel
    using FrameSink = std::function<void(const uint16_t* buf, int w, int h)>;
    void SetFrameSink(FrameSink sink) { on_frame_ = std::move(sink); }

    // 注册一个动作
    void RegisterAction(const Action& action);
    // 播放指定动作（覆盖式，会停掉当前动作）
    bool Play(const std::string& name);
    // 当前是否正在播放
    bool IsPlaying() const { return playing_; }
    // 任务循环读取的帧间隔（ms）
    int FrameMs() const { return frame_ms_; }
    // 立即停止
    void Stop();
    // 绘制当前帧（由动画任务调用，需为公以供任务函数访问）
    void DrawCurrentFrame();

private:
    int width_;
    int height_;
    volatile bool playing_ = false;
    std::string current_action_;
    int current_frame_ = 0;
    int frame_ms_ = 80;
    TaskHandle_t task_handle_ = nullptr;
    std::vector<Action> actions_;
    FrameSink on_frame_;
    uint16_t* frame_buf_ = nullptr;  // 常驻帧缓冲（PSRAM），320x240x2
    // 保护所有刷屏与播放状态。
    // esp_lcd_panel_draw_bitmap 内部会 spi_device_acquire_bus/release_bus，
    // 该锁不可重入也不跨任务：若多个任务同时刷屏，锁状态会错乱并触发
    // "Cannot release a lock that hasn't been acquired" 断言崩溃。
    SemaphoreHandle_t mutex_ = nullptr;
};

#endif // PET_ANIMATION_H
