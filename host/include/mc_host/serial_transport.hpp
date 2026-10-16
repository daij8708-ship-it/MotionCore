#pragma once

#include <cstddef>
#include <cstdint>

#include <QByteArray>
#include <QSerialPort>
#include <QString>

#include "mc/transport.hpp"

namespace mchost {

// 把 QSerialPort 包成 mc::ITransport。
//
// 它在 host 里、不在 libmc_core 里：核心库保持零 Qt（ADR-001），
// 换一个传输不该把 Qt 拖进协议层。
//
// ⚠️ 一个必须写下来的实现细节：**QSerialPort 不是"调 readAll 就去读 fd"**。
//    字节先由事件循环里的读通知器搬进 Qt 自己的缓冲，readAll 只是取走缓冲。
//    所以 receive() 必须在**事件循环里**被调用 —— 产品路径正是如此
//    （DeviceManager 的 QTimer 跑在事件循环里）。在测试里要自己转事件循环，
//    否则会看到"数据明明到了、receive() 却返回 0"。
//    绝不能在 receive() 里补一个 processEvents()：那是在 tick 回调里重入事件循环，
//    等于让 DeviceManager::onTick 在 onTick 内部被再跑一遍。
class SerialTransport final : public mc::ITransport {
public:
    struct Config {
        QString portName;          // 例如 "/dev/ttyUSB0"、"COM3"
        int baudRate = 115200;
        int dataBits = 8;          // 7 / 8
        enum class Parity : std::uint8_t { None = 0U, Even, Odd };
        Parity parity = Parity::None;
        int stopBits = 1;          // 1 / 2
        bool hardwareFlowControl = false;
    };

    explicit SerialTransport(Config config);
    ~SerialTransport() override;

    // 打开端口并施加串口参数。失败返回 false，原因见 lastErrorText() ——
    // 打开失败在真硬件上是**常态**（端口被别的程序占着、线没插、名字写错），
    // 因此不能只回一个 bool 了事。
    bool open();

    // 给界面直接用的中文说明（设备层再翻成 OpenError 令牌，见 device.hpp）。
    const char* lastErrorText() const noexcept;
    QSerialPort::SerialPortError lastSystemError() const noexcept { return systemError_; }

    bool send(const std::uint8_t* data, std::size_t len) override;
    std::size_t receive(std::uint8_t* out, std::size_t maxLen) override;
    bool isOpen() const noexcept override;
    void close() override;

private:
    Config config_;
    QSerialPort port_;
    // 上次读多了、还没交给调用方的余量。
    // 没有它就会**静默丢字节**：receive() 只接收 maxLen，多出来的部分
    // 若留在 Qt 缓冲里倒也无妨，但一旦我们自己 readAll 了就必须自己保管。
    QByteArray pending_;
    QString errorText_;
    mutable QByteArray errorUtf8_;   // lastErrorText() 的 stabilizer，见实现
    QSerialPort::SerialPortError systemError_ = QSerialPort::NoError;
};

}  // namespace mchost
