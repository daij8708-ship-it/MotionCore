#pragma once

#include <cstddef>
#include <cstdint>

#include "mc/transport.hpp"

#if defined(__unix__) || defined(__APPLE__)

namespace mc {

// ---------------------------------------------------------------------------
// 基于文件描述符的传输：把一条已经打开的 tty（真串口 / 伪终端）当成链路。
//
// 存在的理由：设备侧对端（mc_sim）和设备侧测试需要**真 tty**，而 Qt 的
// QSerialPort 会把 Qt 拖进核心库（ADR-001）。这里只用 POSIX 系统调用，
// 因此"设备端在一条真串口上能不能跑完整会话"可以在零 Qt 的前提下验证。
//
// 语义（在 ITransport 的约定之上，补几条 tty 特有的说明）：
//   send()     非阻塞 fd 上**可能只写出去一部分**。此时返回 false，
//              已写出的字节数记在 counters().bytesSent 里 —— 调用方不能把
//              false 读成"一个字节都没发"（这一条回环传输上没有，只有真 fd 有）。
//              EAGAIN 视为"这一拍发不完"，不是错误。
//   receive()  返回 0 只表示"这一拍没有数据"。**PTY 的 master 端在没有对端
//              打开从端时 read 会返回 EIO** —— 那是"对面还没接上"，不是链路坏掉，
//              因此同样按 0 处理，但计入 counters().readErrors 让这件事看得见。
//   close()    幂等；ownsFd 为 true 时由本对象关闭 fd。
// ---------------------------------------------------------------------------
class FdTransport final : public ITransport {
public:
    struct Counters {
        std::uint64_t bytesSent = 0;      // 真正写进 fd 的字节（含部分写出的）
        std::uint64_t bytesReceived = 0;
        std::uint64_t writeErrors = 0;
        std::uint64_t readErrors = 0;     // 含 PTY "从端尚未打开" 的 EIO
    };

    FdTransport(int fd, bool ownsFd = false);
    ~FdTransport() override;

    FdTransport(const FdTransport&) = delete;
    FdTransport& operator=(const FdTransport&) = delete;

    bool send(const std::uint8_t* data, std::size_t len) override;
    std::size_t receive(std::uint8_t* out, std::size_t maxLen) override;
    bool isOpen() const noexcept override;
    void close() override;

    const Counters& counters() const noexcept { return counters_; }
    int fd() const noexcept { return fd_; }

private:
    int fd_ = -1;
    bool ownsFd_ = false;
    Counters counters_{};
};

}  // namespace mc

#endif  // __unix__ || __APPLE__
