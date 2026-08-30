#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>
#include <soc/gpio_num.h>

// ===== XiaoZhi Pet — ST7789 2.4" 屏 (320x240 横屏)，无 mic / 无喇叭 =====
// 屏幕为 ST7789 2.4" 模块，控制器横屏输出 320x240。
// 走独立 HSPI(SPI3_HOST) 总线，避开与片内 Flash 冲突的 FSPI。
// 音频走 NoAudioCodec，交互依赖按键/触摸 + 服务端文本/LLM。

#define AUDIO_INPUT_SAMPLE_RATE   16000
#define AUDIO_OUTPUT_SAMPLE_RATE  24000

// 无音频：mic / 喇叭引脚全部置 NC（由 NoAudioCodec 处理）
#define AUDIO_I2S_MIC_GPIO_WS    GPIO_NUM_NC
#define AUDIO_I2S_MIC_GPIO_SCK   GPIO_NUM_NC
#define AUDIO_I2S_MIC_GPIO_DIN   GPIO_NUM_NC
#define AUDIO_I2S_SPK_GPIO_DOUT  GPIO_NUM_NC
#define AUDIO_I2S_SPK_GPIO_BCLK  GPIO_NUM_NC
#define AUDIO_I2S_SPK_GPIO_LRCK  GPIO_NUM_NC

// 交互按键（无 mic，靠按键触发对话/唤醒）
#define BOOT_BUTTON_GPIO          GPIO_NUM_0

// 可选：用于 MCP 测试的外设（灯/舵机），按需接线
#define LAMP_GPIO                 GPIO_NUM_NC

// ===== LCD: ST7789 2.4" (320x240 横屏) HSPI =====
// 按你板子的实际接线修改下面的 GPIO
#define DISPLAY_SPI_HOST          SPI3_HOST   // HSPI，独立于 Flash 的 FSPI
#define DISPLAY_BACKLIGHT_PIN     GPIO_NUM_NC // 背光未接，常亮 3.3V
#define DISPLAY_SCLK_PIN          GPIO_NUM_19 // SCL
#define DISPLAY_MOSI_PIN          GPIO_NUM_21 // SDA
#define DISPLAY_CS_PIN            GPIO_NUM_18
#define DISPLAY_DC_PIN            GPIO_NUM_20
#define DISPLAY_RESET_PIN         GPIO_NUM_17

// 40MHz：实测刷满一帧 240x320 约 52ms（20MHz 时为 104ms）。
// 提速可显著提高动作流畅度；当初 40MHz "花屏"实为像素字节序错误所致，
// 字节序修复后高速率可用。若出现杂点/花屏，改回 (20 * 1000 * 1000)。
#define DISPLAY_SPI_SCLK_HZ       (40 * 1000 * 1000)

// ST7789 2.4" 横屏配置（程序按 320x240 绘制，驱动 swap_xy 映射到物理 240x320 RAM）。
// 注意：ST7789 控制器内部 RAM 为 240x320 竖屏，需 SWAP_XY=true 才能横屏铺满。
// 若方向反了，调 MIRROR_X/Y；若左右/上下有偏移，调 OFFSET_X/Y。
// ST7789 2.4" 竖屏 (240x320)。INVERT=false 避免黑底/阴影过亮。
// 图片资源 scripts/convert_pet_image.py 生成标准 RGB565（R 在高字节，无字节交换）。
// 该 ST7789 屏的 MADCTL 实际为 BGR 顺序，所以驱动端声明为 BGR，由驱动把 R 调整到
// 屏期望的线上字节序。若实测红蓝对调（偏蓝），再改成 LCD_RGB_ELEMENT_ORDER_RGB。
#define DISPLAY_WIDTH             240
#define DISPLAY_HEIGHT            320
#define DISPLAY_MIRROR_X          false
#define DISPLAY_MIRROR_Y          false
#define DISPLAY_SWAP_XY           false
#define DISPLAY_INVERT_COLOR      false
#define DISPLAY_RGB_ORDER         LCD_RGB_ELEMENT_ORDER_RGB
#define DISPLAY_OFFSET_X          0
#define DISPLAY_OFFSET_Y          0
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false
#define DISPLAY_SPI_MODE          0

// 宠物动画帧率（fps）。逐行刷新 + DMA 异步，降一点更稳，避免花屏。
#define PET_ANIMATION_FPS         10

#endif // _BOARD_CONFIG_H_
