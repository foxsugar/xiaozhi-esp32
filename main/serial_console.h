#ifndef _SERIAL_CONSOLE_H_
#define _SERIAL_CONSOLE_H_

#include <string>

// 串口打字会话：监听 UART0（USB 串口），逐行读取用户输入作为对话文本，
// 转发给 Application::SendTextInput，替代麦克风输入。
class SerialConsole {
public:
    SerialConsole();
    ~SerialConsole();

    // 启动读任务（在 Application::Initialize 中调用）。
    void Start();

    // 向串口输出一行提示（供其他模块在协议未就绪等场景通知用户）。
    static void Print(const std::string& s);

private:
    static void ReaderTask(void* arg);
    void HandleWifiCommand(const std::string& input);
    void HandleTestCommand(const std::string& input);
    void HandlePlayCommand(const std::string& input);
    void HandleHelpCommand();
    void Run();

    void* task_handle_ = nullptr;  // FreeRTOS 任务句柄（TaskHandle_t 实际为指针）
};

#endif // _SERIAL_CONSOLE_H_
