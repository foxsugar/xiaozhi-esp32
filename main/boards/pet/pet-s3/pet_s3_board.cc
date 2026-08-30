#include "wifi_board.h"
#include "pet_display.h"
#include "codecs/no_audio_codec.h"
#include "display/lcd_display.h"
#include "application.h"
#include "config.h"
#include "mcp_server.h"
#include "boards/common/button.h"
#include "ssid_manager.h"

#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <driver/spi_master.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include <cstring>
#include <string>
#include <vector>

namespace {
const char* TAG = "pet_s3";

// ST7789 2.4" 屏驱，GPIO 来自 config.h，走独立 HSPI(SPI3_HOST)
Display* InitializeSt7789Display() {
    ESP_LOGI(TAG, "init ST7789 2.4\" %dx%d on SPI%d", DISPLAY_WIDTH, DISPLAY_HEIGHT,
             DISPLAY_SPI_HOST + 2);  // SPI2->2, SPI3->3
    // 背光未接（常亮 3.3V），无需配置 BL GPIO

    esp_lcd_panel_io_handle_t panel_io = nullptr;
    esp_lcd_panel_handle_t panel = nullptr;
    spi_bus_config_t buscfg = {
        .mosi_io_num = DISPLAY_MOSI_PIN,
        .miso_io_num = GPIO_NUM_NC,
        .sclk_io_num = DISPLAY_SCLK_PIN,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .data4_io_num = GPIO_NUM_NC,
        .data5_io_num = GPIO_NUM_NC,
        .data6_io_num = GPIO_NUM_NC,
        .data7_io_num = GPIO_NUM_NC,
        // 单笔传输我们按行刷新（每行 320*2=640 字节），这里只给驱动留余量
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
        // 注：帧缓冲已在 PetDisplay 内部从 PSRAM 拷到内部 SRAM 行缓冲再发起 DMA，
        // 因此这里不需要 psram_dma_direct（v6.0.2 该标志也不会自动做 cache 写回）。
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)DISPLAY_SPI_HOST, &io_config, &panel_io));

    esp_lcd_panel_dev_config_t panel_config = {
        .rgb_ele_order = DISPLAY_RGB_ORDER,
        .bits_per_pixel = 16,
        .reset_gpio_num = DISPLAY_RESET_PIN,
        // 注意：esp_lcd_panel_dev_config_t.flags 在本 IDF(v6.0.2) 仅含
        // reset_active_high，无 psram_trans_buf 字段。整帧 esp_lcd_panel_draw_bitmap
        // 已可正常工作，花屏由逐行开窗改为整帧一次性刷新规避。
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    // 与已验证可用的测试项目保持一致：不调用 invert_color / set_gap / swap_xy / mirror，
    // 直接使用 ST7789 默认的 MADCTL 寻址，避免这些调用改动屏的映射导致显示错位/花屏。
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    if (DISPLAY_BACKLIGHT_PIN != GPIO_NUM_NC) {
        gpio_set_level(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT ? 0 : 1);
    }

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
    {"happy", "开心兴奋，配合呼吸起伏，适合高兴、夸奖、庆祝"},
    {"sad", "难过低落，配合呼吸起伏，适合伤心、委屈、安慰对方"},
    {"think", "思考，身体左右轻微晃动，适合犹豫、思考问题、不知道答案"},
    {"eat", "吃东西，配合呼吸起伏，适合吃饭、零食、馋了"},
    {"comfort", "安慰/困倦，缓慢深呼吸，适合安慰人、犯困、温柔回应"},
    {"talk", "说话/倾听，幅度很小的轻微呼吸，适合平淡聊天、陈述事实"},
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
    AudioCodec* audio_codec_ = nullptr;
    Button boot_button_;

    void InitializeDisplay() {
        display_ = InitializeSt7789Display();
        RegisterPetMcpTools();
    }

    void InitializeAudioCodec() {
        audio_codec_ = new NoAudioCodecSimplex(
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT,
            AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
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
        InitializeDisplay();
        InitializeAudioCodec();
        InitializeButtons();
    }

    Display* GetDisplay() override { return display_; }
    AudioCodec* GetAudioCodec() override { return audio_codec_; }
};

DECLARE_BOARD(PetS3Board);
