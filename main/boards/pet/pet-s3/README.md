# 板子：pet-s3（ESP32-S3 宠物屏）

## 硬件

- 芯片：ESP32-S3-N16R8（16MB Flash + 8MB PSRAM，Octal SPI RAM）
- 屏幕：ILI9341V，240×320，40MHz SPI（理论上限约 19fps，SPI 刷屏为性能瓶颈）
- 音频：ES8311 编解码
- 输入：按键 + 触摸（见 `pet_s3_board.cc`）

## 宠物动画与 GIF

本板的宠物系统使用 **20 个本地 GIF 动作**，在端侧用 `gifdec` 解码播放，
**不依赖 XiaoZhi 通用表情/Assets 系统**，是一个独立的宠物引擎
（`PetDisplay` / `PetAnimation` / `PetGifPlayer`）。

### 重要：GIF 走独立 assets 分区，烧录需连带写入

GIF 数据 **不进 app 固件（4MB app 分区）**，而是打包成 SPIFFS 镜像烧录到
独立的 **assets 分区**（生成逻辑见 `main/CMakeLists.txt`）：

```cmake
if(BOARD_DIR STREQUAL "pet/pet-s3")
    spiffs_create_partition_image(assets "main/pet/assets/gif")
endif()
```

源文件：`main/pet/assets/gif/`（sad / eat / happy / drink / sleep / cry /
laugh / shy / think / surprise / fight / headpat / bellyrub / angry / blink /
yum / talk / naughty / walk / kiss，共 20 个 `.gif`）

切换板子 / 重烧时务必遵守：

1. **首次烧录必须连带烧写 assets 分区镜像**，否则宠物只有静态 idle 图，
   20 个 GIF 动作全部缺失。
2. 用 `build.py` / `idf.py flash` 烧写时，assets 镜像会一并写入；
   若只烧 app 分区、或整片擦除后只烧 app，GIF 会丢失。
3. **切回本板固件时，记得重新烧 assets 分区**，否则动作缺失。
4. **不要启用** `CONFIG_*_ASSETS`（XiaoZhi 通用 Assets 系统），
   assets 分区已由宠物 GIF 独占，二者会冲突。

### 代码位置

- 解码器：`main/pet/gifdec/`（纯 C，无 LVGL 依赖）
- SPIFFS 封装：`main/pet/pet_gif_fs.h` / `.cc`
- 动作注册与播放：`main/pet/pet_gif_player.h` / `.cc`
- 显示驱动：`main/pet/pet_display.cc`

## 构建

```sh
python scripts/build.py pet/pet-s3 --name pet-s3
```

构建脚本会自动打包 `main/pet/assets/gif/` 为 assets 分区镜像并加入烧录。
