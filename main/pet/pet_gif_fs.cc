#include "pet_gif_fs.h"

#include <cstdio>

#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_vfs_spiffs.h"

static const char* TAG = "PetGifFs";

bool PetGifFs::Mount() {
    esp_vfs_spiffs_conf_t conf = {
        .base_path = kMount,
        .partition_label = kPartition,
        .max_files = 32,
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGE(TAG, "SPIFFS partition '%s' not found (check partition table)", kPartition);
        return false;
    }
    if (err == ESP_FAIL) {
        ESP_LOGE(TAG, "Failed to mount or format SPIFFS partition '%s'", kPartition);
        return false;
    }
    size_t total = 0, used = 0;
    esp_spiffs_info(kPartition, &total, &used);
    ESP_LOGI(TAG, "SPIFFS '%s' mounted: total=%u KB used=%u KB", kPartition,
             (unsigned)(total / 1024), (unsigned)(used / 1024));
    return true;
}

std::vector<uint8_t> PetGifFs::LoadGif(const std::string& name) {
    std::string path = std::string(kDir) + "/" + name + ".gif";
    FILE* f = fopen(path.c_str(), "rb");
    if (f == nullptr) {
        ESP_LOGE(TAG, "open gif failed: %s", path.c_str());
        return {};
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(f);
        return {};
    }
    std::vector<uint8_t> buf((size_t)sz);
    size_t rd = fread(buf.data(), 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) {
        ESP_LOGE(TAG, "read gif truncated: %s (%u/%ld)", path.c_str(), (unsigned)rd, sz);
        return {};
    }
    return buf;
}
