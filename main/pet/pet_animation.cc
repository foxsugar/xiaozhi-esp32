#include "pet_animation.h"

#include <esp_timer.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstring>
#include <cstdlib>

static const char* TAG = "pet_animation";

namespace {
// RAII 互斥锁包装：保证任何退出路径（含提前 return）都会释放锁。
class LockGuard {
public:
    explicit LockGuard(SemaphoreHandle_t m) : m_(m) {
        if (m_ != nullptr) xSemaphoreTake(m_, portMAX_DELAY);
    }
    ~LockGuard() {
        if (m_ != nullptr) xSemaphoreGive(m_);
    }
    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;

private:
    SemaphoreHandle_t m_;
};

// 动画渲染任务：在任务上下文（非 ISR）中逐帧绘制并刷屏。
// esp_lcd_panel_draw_bitmap 内部会走 SPI 传输，不能在 esp_timer 的 ISR 回调里
// 安全调用，否则会出现卡顿/ESP_ERR_NO_MEM/花屏。改用独立任务 + 延时。
//
// 注意：不能用 ulTaskNotifyTake(portMAX_DELAY) 把任务阻塞住再等 Play 唤醒，
// 因为 PetDisplay 构造函数里先 Play("idle") 时任务可能尚未运行到等待点，
// 导致唤醒通知被丢弃 → 任务永远卡住 → 屏幕停在首帧（花屏猫）不动。
// 因此任务改为常驻轮询 playing_，帧间隔用 vTaskDelay 让出 CPU，
// 仅在没有动作播放时短延时，避免空转。
void AnimationTask(void* arg) {
    auto* self = static_cast<PetAnimation*>(arg);
    while (true) {
        if (self->IsPlaying()) {
            // 按实测刷新耗时补偿延时，使实际帧周期等于 FrameMs()。
            // 若固定延时，实际周期 = 刷新耗时 + 延时，帧率会明显低于设定值
            // （例如 40MHz 下刷一帧约 52ms，叠加 100ms 延时会变成约 6.6fps），
            // 表现为动作一顿一顿不流畅。
            int64_t t0 = esp_timer_get_time();
            self->DrawCurrentFrame();
            int64_t elapsed_ms = (esp_timer_get_time() - t0) / 1000;
            int wait_ms = self->FrameMs() - static_cast<int>(elapsed_ms);
            if (wait_ms < 1) wait_ms = 1;  // 刷新已超预算时只让出极短时间
            vTaskDelay(pdMS_TO_TICKS(wait_ms));
        } else {
            // 无动作时短暂让出，等 Play() 把 playing_ 置 true
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}
}  // namespace

PetAnimation::PetAnimation(int width, int height)
    : width_(width), height_(height) {
    // 常驻帧缓冲：320x240x2 = 150KB，分配在 PSRAM（带 PSRAM 时）。
    // 避免定时器回调里每帧 malloc/free 造成内存碎片与失败。
    size_t buf_bytes = static_cast<size_t>(width_) * height_ * sizeof(uint16_t);
    frame_buf_ = static_cast<uint16_t*>(heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (frame_buf_ == nullptr) {
        // 退化到内部 SRAM
        frame_buf_ = static_cast<uint16_t*>(heap_caps_malloc(buf_bytes, MALLOC_CAP_8BIT));
    }
    if (frame_buf_ == nullptr) {
        ESP_LOGE(TAG, "frame buffer alloc failed (%u bytes)", (unsigned)buf_bytes);
    }

    mutex_ = xSemaphoreCreateMutex();

    // 创建动画任务（在任务上下文刷屏，避开 ISR 限制）
    xTaskCreate(AnimationTask, "pet_anim", 4096, this, 5, &task_handle_);
}

PetAnimation::~PetAnimation() {
    playing_ = false;
    if (task_handle_ != nullptr) {
        xTaskNotifyGive(task_handle_);  // 唤醒任务以便退出
        vTaskDelete(task_handle_);
        task_handle_ = nullptr;
    }
    if (frame_buf_) heap_caps_free(frame_buf_);
    if (mutex_ != nullptr) {
        vSemaphoreDelete(mutex_);
        mutex_ = nullptr;
    }
}

void PetAnimation::RegisterAction(const Action& action) {
    actions_.push_back(action);
}

bool PetAnimation::Play(const std::string& name) {
    const Action* act = nullptr;
    for (auto& a : actions_) {
        if (a.name == name) { act = &a; break; }
    }
    if (act == nullptr) {
        ESP_LOGW(TAG, "unknown action: %s", name.c_str());
        return false;
    }
    {
        // 直接改状态即可覆盖旧动作，不再调用 Stop()，避免非递归锁自锁。
        LockGuard lock(mutex_);
        current_action_ = name;
        current_frame_ = 0;
        frame_ms_ = act->fps > 0 ? 1000 / act->fps : 80;
        playing_ = true;
    }
    // 注意：这里不再直接 DrawCurrentFrame()。
    // Play() 可能从主任务（SetEmotion）、MCP 任务、开机延时任务等多处调用，
    // 若在此直接刷屏，会与动画任务并发调用 draw_bitmap，
    // 撞上 SPI 总线锁的 acquire/release 竞态导致 assert 崩溃。
    // 交给动画任务在 20ms 内画出第一帧，视觉上无差别。
    ESP_LOGI(TAG, "play action=%s frames=%d fps=%d", name.c_str(), act->frame_count, act->fps);
    return true;
}

void PetAnimation::Stop() {
    LockGuard lock(mutex_);
    if (playing_) {
        playing_ = false;
        current_action_.clear();
        current_frame_ = 0;
    }
}

void PetAnimation::DrawCurrentFrame() {
    LockGuard lock(mutex_);

    const Action* act = nullptr;
    for (auto& a : actions_) {
        if (a.name == current_action_) { act = &a; break; }
    }
    if (act == nullptr) return;
    if (frame_buf_ == nullptr) return;

    // 复用常驻 PSRAM 帧缓冲，避免每帧重新分配
    act->drawer(frame_buf_, width_, height_, current_frame_, act->frame_count);

    // 由 PetDisplay 通过 panel_ 刷屏：这里通过回调通知持有者
    if (on_frame_) {
        on_frame_(frame_buf_, width_, height_);
    }

    current_frame_ = (current_frame_ + 1) % act->frame_count;
}
