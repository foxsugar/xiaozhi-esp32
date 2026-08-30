#include "serial_console.h"

#include "application.h"
#include "board.h"
#include "boards/common/wifi_board.h"
#include "ssid_manager.h"
#include "pet/pet_display.h"
#include "display/display.h"

#include <esp_log.h>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <fcntl.h>
#include <unistd.h>

#include <cstring>
#include <cctype>
#include <string>

static const char* TAG = "serial_console";

// 读取间隔（ms）。复用 IDF console 的 VFS stdin（fd 0），不再自行安装 UART 驱动。
static constexpr int kReadTimeoutMs = 50;

SerialConsole::SerialConsole() = default;

SerialConsole::~SerialConsole() {
    if (task_handle_ != nullptr) {
        vTaskDelete(static_cast<TaskHandle_t>(task_handle_));
        task_handle_ = nullptr;
    }
}

void SerialConsole::Start() {
    // 不要在 UART0 上调用 uart_driver_install：UART0 是 IDF 的 console 口
    // （GPIO43/44），装驱动会接管硬件并打断 esp_log 输出，导致之后所有日志消失
    // （表现为启动日志停在板子初始化那行，联网日志全没了）。
    // 这里直接复用 VFS 已经挂好的 stdin/stdout（fd 0/1），既不干扰日志也能读输入。
    fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);
    ESP_LOGI(TAG, "listening on VFS stdin, logs remain on UART0 console");

    // 欢迎提示
    const char* banner =
        "\r\n[串口会话] 直接输入文字后回车即可与 AI 对话（无需麦克风）。\r\n"
        "[串口会话] 输入 help 查看可用命令（wifi / play / test）。\r\n";
    write(STDOUT_FILENO, banner, strlen(banner));

    TaskHandle_t task = nullptr;
    xTaskCreate(ReaderTask, "serial_console", 4096, this, 5, &task);
    task_handle_ = task;
}

void SerialConsole::ReaderTask(void* arg) {
    static_cast<SerialConsole*>(arg)->Run();
}

void SerialConsole::Print(const std::string& s) {
    std::string line = "\r\n" + s + "\r\n";
    write(STDOUT_FILENO, line.c_str(), line.size());
}

// 解析 wifi 命令后面的 "ssid password" 部分，支持可选引号包裹（密码可含空格）。
// 返回 {ssid, password}，失败则 pair 的 first 为空。
static std::pair<std::string, std::string> ParseSsidPassword(const std::string& rest) {
    std::pair<std::string, std::string> result;
    size_t pos = 0;
    auto skip_ws = [&]() { while (pos < rest.size() && std::isspace((unsigned char)rest[pos])) pos++; };
    auto read_token = [&]() -> std::string {
        skip_ws();
        if (pos < rest.size() && rest[pos] == '"') {
            size_t start = ++pos;
            while (pos < rest.size() && rest[pos] != '"') pos++;
            std::string tok = rest.substr(start, pos - start);
            if (pos < rest.size()) pos++;  // 跳过闭合引号
            return tok;
        }
        size_t start = pos;
        while (pos < rest.size() && !std::isspace((unsigned char)rest[pos])) pos++;
        return rest.substr(start, pos - start);
    };
    result.first = read_token();
    result.second = read_token();
    return result;
}

void SerialConsole::HandleWifiCommand(const std::string& input) {
    // input 形如 "wifi <ssid> <password>"
    std::string rest = input.substr(strlen("wifi "));
    auto [ssid, password] = ParseSsidPassword(rest);
    if (ssid.empty()) {
        Print("[wifi] 用法: wifi <ssid> <password>  例如: wifi \"MyHome\" \"mypass\"");
        return;
    }
    Print("[wifi] 保存并尝试连接: " + ssid);
    auto* board = dynamic_cast<WifiBoard*>(&Board::GetInstance());
    if (board == nullptr) {
        Print("[wifi] 当前板不支持 WiFi");
        return;
    }
    board->ConnectToWifi(ssid, password);
    Print("[wifi] 已提交，请等待连接日志 (I WifiBoard Connected)");
}

void SerialConsole::HandleTestCommand(const std::string& input) {
    std::string arg = input.substr(strlen("test "));
    auto* disp = dynamic_cast<PetDisplay*>(Board::GetInstance().GetDisplay());
    if (disp == nullptr) {
        Print("[test] 当前显示非 PetDisplay");
        return;
    }
    std::string action = "idle";
    if (arg == "red") action = "test_red";
    else if (arg == "green") action = "test_green";
    else if (arg == "blue") action = "test_blue";
    else {
        Print("[test] 用法: test red | test green | test blue");
        return;
    }
    disp->PlayAction(action);
    Print("[test] 显示纯色: " + arg + " (若颜色不对请改 config.h 的 DISPLAY_RGB_ORDER)");
}

void SerialConsole::HandlePlayCommand(const std::string& input) {
    std::string arg = input.substr(strlen("play "));
    // 去掉首尾空白，方便输入 "play  happy" 这类带多余空格的写法
    size_t b = arg.find_first_not_of(" \t");
    size_t e = arg.find_last_not_of(" \t");
    if (b == std::string::npos) {
        Print("[play] 用法: play idle | blink | happy | sad | think | eat | comfort | talk");
        return;
    }
    arg = arg.substr(b, e - b + 1);

    auto* disp = dynamic_cast<PetDisplay*>(Board::GetInstance().GetDisplay());
    if (disp == nullptr) {
        Print("[play] 当前显示非 PetDisplay");
        return;
    }

    // 合法动作白名单：与 PetDisplay 构造函数中 RegisterAction 注册的名字一致。
    // 只接受白名单，避免把拼错的词或对话内容误当成动作。
    static const char* const kActions[] = {
        "idle", "blink", "happy", "sad", "think", "eat", "comfort", "talk",
        "test_red", "test_green", "test_blue",
    };
    bool valid = false;
    for (const char* a : kActions) {
        if (arg == a) { valid = true; break; }
    }
    if (!valid) {
        Print("[play] 未知动作: " + arg);
        Print("[play] 可用: idle | blink | happy | sad | think | eat | comfort | talk | test_red | test_green | test_blue");
        return;
    }

    bool ok = disp->PlayAction(arg);
    Print(ok ? ("[play] 播放动作: " + arg) : std::string("[play] 播放失败: " + arg));
}

void SerialConsole::HandleHelpCommand() {
    Print("[帮助] 可用命令:");
    Print("  wifi <ssid> <password>   连接 WiFi（密码含空格请用引号）");
    Print("  play <动作>              本地直接播放宠物动作");
    Print("      动作: idle blink happy sad think eat comfort talk test_red test_green test_blue");
    Print("  test red|green|blue      纯色测试（校验颜色顺序）");
    Print("  其他文字                 直接发给 AI 对话");
}

void SerialConsole::Run() {
    std::string line;
    uint8_t ch = 0;
    constexpr size_t kMaxLine = 256;

    while (true) {
        int len = read(STDIN_FILENO, &ch, 1);
        if (len <= 0) {
            vTaskDelay(pdMS_TO_TICKS(kReadTimeoutMs));
            continue;
        }
        // 回显，方便用户看到自己输入
        write(STDOUT_FILENO, &ch, 1);

        if (ch == '\r' || ch == '\n') {
            // 处理 Windows/Unix 换行：遇到 \r 后再吃一个 \n
            if (ch == '\r') {
                uint8_t nxt = 0;
                int n = read(STDIN_FILENO, &nxt, 1);
                if (n > 0 && nxt != '\n') {
                    // 不是换行，放回（简单丢弃即可，避免复杂）
                }
                write(STDOUT_FILENO, "\n", 1);
            }
            if (!line.empty()) {
                std::string input = line;
                line.clear();
                // 命令解析：以 "wifi " 开头的行作为配网命令，不进入对话
                if (input.rfind("wifi ", 0) == 0) {
                    HandleWifiCommand(input);
                    continue;
                }
                // 屏幕纯色测试：test red / test green / test blue（校验 RGB 顺序）
                if (input.rfind("test ", 0) == 0) {
                    HandleTestCommand(input);
                    continue;
                }
                // 本地播放宠物动作：play happy 等（不经过 AI，便于调试流畅度）
                if (input.rfind("play ", 0) == 0) {
                    HandlePlayCommand(input);
                    continue;
                }
                // 帮助
                if (input == "help" || input == "?") {
                    HandleHelpCommand();
                    continue;
                }
                std::string echo = "\r\n>>> 已发送: " + input + "\r\n";
                write(STDOUT_FILENO, echo.c_str(), echo.size());
                ESP_LOGI("serial_console", "input received: %s", input.c_str());
                Application::GetInstance().SendTextInput(input);
            }
            continue;
        }
        if (ch == 0x08 || ch == 0x7F) {  // 退格
            if (!line.empty()) {
                line.pop_back();
                write(STDOUT_FILENO, "\b \b", 3);
            }
            continue;
        }
        if (line.size() < kMaxLine) {
            line.push_back((char)ch);
        }
    }
}
