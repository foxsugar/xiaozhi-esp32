#ifndef PET_GIF_FS_H
#define PET_GIF_FS_H

#include <cstdint>
#include <string>
#include <vector>

// 把宠物 GIF 放在 8MB "assets" SPIFFS 分区（pet-s3 默认未使用，本模块独占）。
// 设备侧路径：/assets/gif/<name>.gif
class PetGifFs {
public:
    // 挂载 assets 分区为 SPIFFS（挂载失败会格式化）。返回是否成功。
    static bool Mount();

    // 读取 /assets/gif/<name>.gif 到内存缓冲；失败返回空 vector。
    static std::vector<uint8_t> LoadGif(const std::string& name);

    static constexpr const char* kPartition = "assets";
    static constexpr const char* kMount = "/assets";
    static constexpr const char* kDir = "/assets";
};

#endif // PET_GIF_FS_H
