#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <soc/gpio_num.h>

// ===== XiaoZhi Pet — LCDwiki 2.8" ESP32-S3 显示模块 (ES3C28P / ES3N28P) =====
//
// 硬件：ESP32-S3-N16R8（16MB Flash + 8MB OPI PSRAM）
//   · 2.8" IPS TFT，240x320，驱动 IC ILI9341V，4 线 SPI
//   · 音频编解码 ES8311（板载麦克风 + 喇叭），经 I2C 配置
//   · 电容触摸 FT6336G（I2C，与 ES8311 共用同一组总线）
//   · 单线 RGB 三色灯、microSD(SDIO)、电池电量 ADC(IO9)
//
// 引脚依据官方资料：https://www.lcdwiki.com/zh/2.8inch_ESP32-S3_Display
// 与仓库内 freenove-esp32s3-display-2.8-lcd 为同款引脚布局，可交叉参考。

// ===== 音频：ES8311（板载麦克风 + 喇叭）=====
#define AUDIO_INPUT_SAMPLE_RATE   24000
#define AUDIO_OUTPUT_SAMPLE_RATE  24000

#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_4   // I2S 主时钟
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_5   // I2S 位时钟
#define AUDIO_I2S_GPIO_WS   GPIO_NUM_7   // I2S 左右声道选择
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_8   // I2S 喇叭数据输出
#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_6   // I2S 麦克风数据输入
#define AUDIO_CODEC_PA_PIN  GPIO_NUM_1   // 功放使能（低电平使能）

// ES8311 通过 I2C 配置，与电容触摸屏共用一组 I2C（SDA=16 / SCL=15）
#define AUDIO_CODEC_I2C_NUM      I2C_NUM_0
#define AUDIO_CODEC_I2C_SCL_PIN  GPIO_NUM_15
#define AUDIO_CODEC_I2C_SDA_PIN  GPIO_NUM_16
#define AUDIO_CODEC_ES8311_ADDR  ES8311_CODEC_DEFAULT_ADDR
// 板子功放使能脚是"低电平使能"，故传给 Es8311AudioCodec 时 pa_inverted 需为 true
#define AUDIO_CODEC_PA_INVERTED  true

// ===== 按键 / 指示灯 =====
#define BOOT_BUTTON_GPIO  GPIO_NUM_0
#define BUILTIN_LED_GPIO  GPIO_NUM_42  // 单线 RGB 三色灯

// 可选：用于 MCP 测试的外设（灯/舵机），按需接线
#define LAMP_GPIO         GPIO_NUM_NC

// ===== LCD：ILI9341V 2.8" 240x320，4 线 SPI =====
#define DISPLAY_SPI_HOST      SPI3_HOST
#define DISPLAY_CS_PIN        GPIO_NUM_10  // 片选，低电平有效
#define DISPLAY_DC_PIN        GPIO_NUM_46  // 命令/数据选择（高=数据，低=命令）
#define DISPLAY_SCLK_PIN      GPIO_NUM_12  // SPI 时钟
#define DISPLAY_MOSI_PIN      GPIO_NUM_11  // SPI 写数据
#define DISPLAY_MISO_PIN      GPIO_NUM_13  // SPI 读数据
// 液晶屏复位与 ESP32-S3 的 EN 共用，无法用 GPIO 单独复位，故置 NC
#define DISPLAY_RESET_PIN     GPIO_NUM_NC
#define DISPLAY_BACKLIGHT_PIN GPIO_NUM_45  // 背光，高电平点亮
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false

// 40MHz：实测刷满一帧 240x320 约 52ms。
// 注意：ESP32 是小端，帧缓冲里的 uint16 像素必须在发送前交换高低字节
// （见 PetDisplay::FlushFrame），否则颜色会整体错乱、图片变成彩色噪声。
// 若出现杂点/花屏，可改回 (20 * 1000 * 1000)。
#define DISPLAY_SPI_SCLK_HZ   (40 * 1000 * 1000)
#define DISPLAY_SPI_MODE      0

// 竖屏 240x320。
// MADCTL(0x36) 目标值 0x48 = MX | BGR，取自厂商 Demo 的旋转表
// {0x48=竖屏, 0x88=竖屏倒转, 0x28=横屏, 0xE8=横屏倒转}（即常用的 ILI9341 旋转表）。
// 竖屏必须置 MX(MIRROR_X)，否则画面左右镜像；BGR 位由 DISPLAY_RGB_ORDER 控制。
#define DISPLAY_WIDTH         240
#define DISPLAY_HEIGHT        320
#define DISPLAY_MIRROR_X      true
#define DISPLAY_MIRROR_Y      false
#define DISPLAY_SWAP_XY       false
// 厂商 Demo 的 CONFIG_LV_INVERT_COLORS=y（发 0x21 Display Inversion ON），
// 同硬件的 freenove-esp32s3-display-2.8-lcd 也是 true。关掉会呈"底片"色。
#define DISPLAY_INVERT_COLOR  true
// 帧缓冲为标准 RGB565（R 在高位）。厂商 Demo 的 MADCTL 里 BGR 位为 1，
// 即要求屏按 BGR 解析，故这里必须是 LCD_RGB_ELEMENT_ORDER_BGR，
// 否则红色与蓝色对调。
#define DISPLAY_RGB_ORDER     LCD_RGB_ELEMENT_ORDER_BGR
#define DISPLAY_OFFSET_X      0
#define DISPLAY_OFFSET_Y      0

// ===== 触摸：FT6336G（暂未启用，与 ES8311 共用 AUDIO_CODEC_I2C_* 总线）=====
#define TOUCH_I2C_ADDR        0x38
#define TOUCH_RST_PIN         GPIO_NUM_18
#define TOUCH_INT_PIN         GPIO_NUM_17

// 宠物动画帧率（fps）
#define PET_ANIMATION_FPS     10

#endif // _BOARD_CONFIG_H_
