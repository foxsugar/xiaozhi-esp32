// ============================================================================
// 宠物 GIF 动作说明（切换/重烧此板时务必注意）
// ----------------------------------------------------------------------------
// 本板的宠物动画使用 20 个端侧解码的 GIF，存放在 assets 分区（SPIFFS，
// 由 main/CMakeLists.txt 的 spiffs_create_partition_image(assets ...) 生成镜像）。
//
// 注意：GIF 数据【不】进 app 固件（4MB app 分区），而是烧录到独立的 assets
// 分区。因此：
//   1. 第一次烧录必须连带烧写 assets 分区镜像，否则宠物无动作（仅显示静态 idle）。
//   2. 用 idf.py flash / build.py 烧写时，assets 分区镜像会一并写入；
//      若只烧 app 或整片擦除后只烧 app，GIF 会丢失。
//   3. 切回本板固件时，记得重新烧 assets 分区（GIF 镜像），否则动作缺失。
//   4. 不要在此板启用 CONFIG_*_ASSETS（XiaoZhi 通用 Assets 系统），
//      assets 分区已由宠物 GIF 独占，二者会冲突。
//
// GIF 源文件：main/pet/assets/gif/（sad/eat/happy/... 共 20 个 .gif）
// 解码器：main/pet/gifdec/   播放注册：main/pet/pet_gif_player.cc
// ============================================================================

#include "wifi_board.h"
#include "pet_display.h"
#include "codecs/es8311_audio_codec.h"
#include "display/lcd_display.h"
#include "led/single_led.h"
#include "application.h"
#include "config.h"
#include "mcp_server.h"
#include "boards/common/button.h"
#include "ssid_manager.h"

#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_ili9341.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include <cstring>
#include <string>
#include <vector>

namespace {
const char* TAG = "pet_s3";

// ===== 厂商调校的初始化序列 =====
// 取自 LCDwiki 官方 Demo：
//   1-示例程序_Demo/ESP-IDF/2.8inch_ESP32-S3_LVGL/
//     components/lvgl_esp32_drivers/lvgl_tft/ili9341.c  -> ili9341_init()
// espressif 通用驱动自带的默认值（Gamma 0xE0/0xE1、VCOM 0xC5/0xC7、
// 电源 0xC0/0xC1、接口控制 0xF6）是按通用 ILI9341 给的，与这块屏不匹配，
// 会让画面整体发灰、偏色。这里逐条照抄 Demo 为本屏调校的取值。
//
// 以下 5 条驱动已经负责，这里不再重复，避免与后续的 esp_lcd_panel_* 调用打架：
//   0x11 退出睡眠（驱动 init 开头已发）
//   0x29 开显示（esp_lcd_panel_disp_on_off）
//   0x36 MADCTL（由 swap_xy / mirror 按 config.h 合成后下发）
//   0x3A COLMOD（驱动按 bits_per_pixel=16 发 0x55）
//   0x21 反色（由 esp_lcd_panel_invert_color(DISPLAY_INVERT_COLOR) 下发）
namespace {
const uint8_t kCfgCF[] = {0x00, 0xC1, 0x30};
const uint8_t kCfgED[] = {0x64, 0x03, 0x12, 0x81};
const uint8_t kCfgE8[] = {0x85, 0x00, 0x78};
const uint8_t kCfgCB[] = {0x39, 0x2C, 0x00, 0x34, 0x02};
const uint8_t kCfgF7[] = {0x20};
const uint8_t kCfgEA[] = {0x00, 0x00};
const uint8_t kCfgC0[] = {0x13};  // Power control 1, GVDD
const uint8_t kCfgC1[] = {0x13};  // Power control 2
const uint8_t kCfgC5[] = {0x22, 0x35};  // VCOM control 1
const uint8_t kCfgC7[] = {0xBD};  // VCOM control 2
const uint8_t kCfgB6[] = {0x0A, 0x82};  // Display function control
const uint8_t kCfgF6[] = {0x01, 0x30};  // Interface control
const uint8_t kCfgB1[] = {0x00, 0x1B};  // Frame rate control
const uint8_t kCfgF2[] = {0x00};  // Enable 3G, disabled
const uint8_t kCfg26[] = {0x01};  // Gamma set, curve 1
const uint8_t kCfgE0[] = {0x0F, 0x35, 0x31, 0x0B, 0x0F, 0x06, 0x49, 0xA7,
                          0x33, 0x07, 0x0F, 0x03, 0x0C, 0x0A, 0x00};  // 正极性 Gamma
const uint8_t kCfgE1[] = {0x00, 0x0A, 0x0F, 0x04, 0x11, 0x08, 0x36, 0x58,
                          0x4D, 0x07, 0x10, 0x0C, 0x32, 0x34, 0x0F};  // 负极性 Gamma

// 注意：必须是 static 存储期，驱动在 init 时只是保存指针，不会拷贝内容。
const ili9341_lcd_init_cmd_t kLcdInitCmds[] = {
    {0xCF, kCfgCF, sizeof(kCfgCF), 0},
    {0xED, kCfgED, sizeof(kCfgED), 0},
    {0xE8, kCfgE8, sizeof(kCfgE8), 0},
    {0xCB, kCfgCB, sizeof(kCfgCB), 0},
    {0xF7, kCfgF7, sizeof(kCfgF7), 0},
    {0xEA, kCfgEA, sizeof(kCfgEA), 0},
    {0xC0, kCfgC0, sizeof(kCfgC0), 0},
    {0xC1, kCfgC1, sizeof(kCfgC1), 0},
    {0xC5, kCfgC5, sizeof(kCfgC5), 0},
    {0xC7, kCfgC7, sizeof(kCfgC7), 0},
    {0xB6, kCfgB6, sizeof(kCfgB6), 0},
    {0xF6, kCfgF6, sizeof(kCfgF6), 0},
    {0xB1, kCfgB1, sizeof(kCfgB1), 0},
    {0xF2, kCfgF2, sizeof(kCfgF2), 0},
    {0x26, kCfg26, sizeof(kCfg26), 0},
    {0xE0, kCfgE0, sizeof(kCfgE0), 0},
    {0xE1, kCfgE1, sizeof(kCfgE1), 0},
};
constexpr uint16_t kLcdInitCmdsSize = sizeof(kLcdInitCmds) / sizeof(kLcdInitCmds[0]);
}  // namespace

// ILI9341V 2.8" 屏驱，GPIO 来自 config.h，走 HSPI(SPI3_HOST)
Display* InitializeIli9341Display() {
    ESP_LOGI(TAG, "init ILI9341V 2.8\" %dx%d on SPI%d", DISPLAY_WIDTH, DISPLAY_HEIGHT,
             DISPLAY_SPI_HOST + 2);  // SPI2->2, SPI3->3

    esp_lcd_panel_io_handle_t panel_io = nullptr;
    esp_lcd_panel_handle_t panel = nullptr;
    spi_bus_config_t buscfg = {
        .mosi_io_num = DISPLAY_MOSI_PIN,
        .miso_io_num = DISPLAY_MISO_PIN,
        .sclk_io_num = DISPLAY_SCLK_PIN,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .data4_io_num = GPIO_NUM_NC,
        .data5_io_num = GPIO_NUM_NC,
        .data6_io_num = GPIO_NUM_NC,
        .data7_io_num = GPIO_NUM_NC,
        // 单笔传输我们按行刷新（每行 240*2=480 字节），这里只给驱动留余量
        .max_transfer_sz = 16384,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(DISPLAY_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = DISPLAY_CS_PIN,
        .dc_gpio_num = DISPLAY_DC_PIN,
        .spi_mode = DISPLAY_SPI_MODE,
        .pclk_hz = DISPLAY_SPI_SCLK_HZ,
        .trans_queue_depth = 10,
        .on_color_trans_done = nullptr,
        .user_ctx = nullptr,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        // 注：帧缓冲已在 PetDisplay 内部拷到内部 SRAM 行缓冲再发起 DMA，
        // 因此这里不需要 psram_dma_direct（v6.0.2 该标志也不会自动做 cache 写回）。
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)DISPLAY_SPI_HOST, &io_config, &panel_io));

    // 用厂商 Demo 调校过的初始化序列替换驱动自带的通用默认值（详见 kLcdInitCmds 注释）
    // 注意：esp_lcd_panel_dev_config_t::vendor_config 是 void*，不能传 const 指针
    static ili9341_vendor_config_t vendor_config = {
        .init_cmds = kLcdInitCmds,
        .init_cmds_size = kLcdInitCmdsSize,
    };

    esp_lcd_panel_dev_config_t panel_config = {
        .rgb_ele_order = DISPLAY_RGB_ORDER,
        .bits_per_pixel = 16,
        // 屏复位与 ESP32-S3 的 EN 共用，无法用 GPIO 单独复位，故为 NC
        .reset_gpio_num = DISPLAY_RESET_PIN,
        .vendor_config = &vendor_config,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(panel_io, &panel_config, &panel));
    ESP_LOGI(TAG, "ILI9341 driver installed");
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    // 以下三项由 config.h 控制，便于只改配置即可调整方向与反色：
    //   · 画面像"底片"（颜色发白/发灰）→ DISPLAY_INVERT_COLOR 改 true
    //   · 横竖方向不对           → DISPLAY_SWAP_XY 改 true
    //   · 左右/上下镜像          → DISPLAY_MIRROR_X / Y 改 true
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    // 背光交给 PwmBacklight（见 PetS3Board::GetBacklight），此处不直接操作 GPIO，
    // 因为 ledc 未初始化时 gpio_set_level 无效，且会与背光控制冲突。

    return new PetDisplay(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                          DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
                          DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
}

// MCP 工具：pet_play_action —— 让 AI 在对话中主动选择宠物动作。
namespace {
// 可用动作及其含义。描述写清楚，AI 才能正确挑选；
// 同时用于入参校验，避免 AI 传错名字后静默失败。
struct ActionInfo {
    const char* name;
    const char* desc;
};

constexpr ActionInfo kActionTable[] = {
    {"idle", "静止站立的常态猫图（停止其他动作、安静待机）"},
    {"blink", "眨眼，适合惊讶、调皮或卖萌"},
    {"happy", "开心兴奋，适合高兴、夸奖、庆祝"},
    {"sad", "难过低落，适合伤心、委屈、安慰对方"},
    {"think", "思考，适合犹豫、思考问题、不知道答案"},
    {"eat", "吃东西，适合吃饭、零食、馋了"},
    {"comfort", "安慰/困倦，缓慢深呼吸，适合安慰人、犯困、温柔回应"},
    {"talk", "说话/倾听，幅度很小的轻微呼吸，适合平淡聊天、陈述事实"},
    {"drink", "喝水，适合口渴、喝水、喝饮料"},
    {"sleep", "困倦睡觉，适合犯困、想睡、晚安"},
    {"cry", "大哭，适合伤心哭泣、委屈大哭"},
    {"laugh", "大笑，适合欢乐、好笑、哈哈大笑"},
    {"shy", "尴尬害羞，适合不好意思、脸红、难为情"},
    {"surprise", "惊讶，适合吃惊、意外、震惊"},
    {"fight", "打架，适合玩耍打架、闹腾、不服"},
    {"headpat", "摸头，适合被夸、撒娇、求摸头"},
    {"bellyrub", "摸肚皮，适合撒娇、求抚摸肚皮"},
    {"angry", "生气，适合生气、发火、不满"},
    {"yum", "美味，适合吃到好吃的、回味、满足"},
    {"naughty", "调皮，适合淘气、捣蛋、恶作剧"},
    {"walk", "走路，适合走动、散步、跟他走"},
    {"kiss", "飞吻，适合喜欢、亲亲、送飞吻"},
};

const char* FindActionDesc(const std::string& name) {
    for (const auto& a : kActionTable) {
        if (name == a.name) return a.desc;
    }
    return nullptr;
}
}  // namespace

// 构造"动作名 -> 含义"的说明文本，拼进工具描述里，让 AI 知道能选什么。
static std::string BuildActionCatalog() {
    std::string s = "可选动作(action)：";
    for (size_t i = 0; i < sizeof(kActionTable) / sizeof(kActionTable[0]); i++) {
        if (i > 0) s += "；";
        s += kActionTable[i].name;
        s += "(";
        s += kActionTable[i].desc;
        s += ")";
    }
    return s;
}

void RegisterPetMcpTools() {
    std::string desc =
        "播放宠物表情动作，让屏幕上的宠物配合当前对话做出反应。"
        "每次回复都建议调用一次，让用户看到宠物的情绪变化；"
        "若本轮情绪平淡可选 talk，情绪明显则选对应动作。"
        "调用后宠物会自动持续播放该动作约 5 秒再回到 idle，无需频繁重复调用。"
        + BuildActionCatalog();

    McpServer::GetInstance().AddTool(
        "pet_play_action",
        desc,
        PropertyList({
            Property("action", kPropertyTypeString),
        }),
        [](const PropertyList& properties) -> ReturnValue {
            auto action = properties["action"].value<std::string>();
            auto* display = static_cast<PetDisplay*>(Board::GetInstance().GetDisplay());
            if (display == nullptr) {
                return std::string("{\"error\":\"no display\"}");
            }
            if (FindActionDesc(action) == nullptr) {
                // 明确告知可用动作，便于 AI 自我纠正后重试
                return std::string("{\"error\":\"unknown action\",\"hint\":\"") +
                       BuildActionCatalog() + "\"}";
            }
            bool ok = display->PlayAction(action);
            return ok ? std::string("{\"result\":\"ok\",\"action\":\"") + action + "\"}"
                      : std::string("{\"error\":\"play failed\"}");
        });
}

}  // namespace

class PetS3Board : public WifiBoard {
private:
    Display* display_ = nullptr;
    Button boot_button_;
    i2c_master_bus_handle_t i2c_bus_ = nullptr;

    // ES8311（音频）与 FT6336G（触摸）共用这组 I2C（SDA=16 / SCL=15）
    void InitializeI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = AUDIO_CODEC_I2C_NUM,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {.enable_internal_pullup = 1},
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
        ESP_LOGI(TAG, "I2C initialized: SDA=%d SCL=%d", AUDIO_CODEC_I2C_SDA_PIN,
                 AUDIO_CODEC_I2C_SCL_PIN);
    }

    void InitializeDisplay() {
        display_ = InitializeIli9341Display();
        RegisterPetMcpTools();
    }

    void InitializeButtons() {
        boot_button_.OnLongPress([this]() {
            ESP_LOGI(TAG, "BOOT long press: clear saved WiFi and enter config mode");
            SsidManager::GetInstance().Clear();
            EnterWifiConfigMode();
        });
    }

public:
    PetS3Board()
        : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeI2c();  // 最先初始化：ES8311 需要它
        InitializeDisplay();
        InitializeButtons();
        GetBacklight()->SetBrightness(100);
    }

    Display* GetDisplay() override { return display_; }

    AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(
            i2c_bus_, AUDIO_CODEC_I2C_NUM, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN, AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR,
            true,                      // use_mclk：板子接了 MCLK(IO4)
            AUDIO_CODEC_PA_INVERTED);  // 功放为低电平使能
        return &audio_codec;
    }

    Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }
};

DECLARE_BOARD(PetS3Board);
