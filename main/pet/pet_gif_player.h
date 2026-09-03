#ifndef PET_GIF_PLAYER_H
#define PET_GIF_PLAYER_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "pet_animation.h"

// 把 /assets/gif/*.gif 中可用的 GIF 动作注册进 PetAnimation 引擎。
// GIF 源为 240x320 RGB(A)，与屏同尺寸，直接逐像素转为 RGB565 写入帧缓冲；
// 透明像素保留目标缓冲底色，实现透明叠加。
class PetGifPlayer {
public:
    // 在 PetDisplay 构造后调用（需先 PetGifFs::Mount()），注册所有 GIF 动作。
    static void RegisterAll(PetAnimation* anim);

private:
    // 单个 GIF 动作资源：解码句柄 + 内存缓冲（生命周期跟随 anim）。
    struct GifAction {
        std::string name;
        std::vector<uint8_t> data;
        void* gif = nullptr;  // gd_GIF*
        int decoded = 0;      // 已顺序解到的帧索引
    };
};

#endif // PET_GIF_PLAYER_H
