#include "mc_host/serial_transport.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

namespace mchost {
namespace {

QSerialPort::Parity toQtParity(SerialTransport::Config::Parity parity) noexcept {
    switch (parity) {
        case SerialTransport::Config::Parity::Even: return QSerialPort::EvenParity;
        case SerialTransport::Config::Parity::Odd:  return QSerialPort::OddParity;
        case SerialTransport::Config::Parity::None: break;
    }
    return QSerialPort::NoParity;
}

// Qt 的 SerialPortError 有十几个值，逐条翻译成使用者能据以行动的话。
// "Unknown error" 这种输出等于什么都没说 —— 排查的人需要知道**下一步做什么**。
QString describe(QSerialPort::SerialPortError error, const QString& portName,
                 const QString& qtText) {
    switch (error) {
        case QSerialPort::NoError:
            return QStringLiteral("无错误");
        case QSerialPort::DeviceNotFoundError:
            return QStringLiteral("找不到端口 %1（设备没插上，或名字写错了）").arg(portName);
        case QSerialPort::PermissionError:
            return QStringLiteral("没有权限打开 %1（Linux 上通常要把用户加进 dialout 组）")
                .arg(portName);
        case QSerialPort::OpenError:
        case QSerialPort::ResourceError:
            return QStringLiteral("%1 打不开或已被占用（另一个程序正开着它？）").arg(portName);
        case QSerialPort::NotOpenError:
            return QStringLiteral("端口尚未打开");
        case QSerialPort::WriteError:
        case QSerialPort::ReadError:
            return QStringLiteral("读写 %1 出错（线被拔了？）").arg(portName);
        case QSerialPort::UnsupportedOperationError:
            return QStringLiteral("本平台/驱动不支持该串口参数组合");
        case QSerialPort::TimeoutError:
            return QStringLiteral("等待 %1 超时").arg(portName);
        default:
            return QStringLiteral("串口错误（%1）：%2").arg(portName, qtText);
    }
}

}  // namespace

SerialTransport::SerialTransport(Config config) : config_(std::move(config)) {
    port_.setPortName(config_.portName);
}

SerialTransport::~SerialTransport() {
    close();
}

bool SerialTransport::open() {
    if (port_.isOpen()) {
        return true;
    }

    pending_.clear();
    systemError_ = QSerialPort::NoError;
    errorText_.clear();

    if (config_.portName.isEmpty()) {
        systemError_ = QSerialPort::DeviceNotFoundError;
        errorText_ = QStringLiteral("没有选择端口");
        return false;
    }

    if (!port_.open(QIODevice::ReadWrite)) {
        systemError_ = port_.error();
        errorText_ = describe(systemError_, config_.portName, port_.errorString());
        return false;
    }

    if (!port_.setBaudRate(config_.baudRate)) {
        systemError_ = port_.error();
        errorText_ = QStringLiteral("波特率 %1 不被支持").arg(config_.baudRate);
        close();
        return false;
    }

    // 逐项设置并在末尾统一查一次 error()：Qt 在 Linux 上对某些参数组合
    // 只报"不支持"，分不清是哪一项 —— 所以这里把整组参数一起报出来。
    port_.setDataBits(config_.dataBits == 7 ? QSerialPort::Data7 : QSerialPort::Data8);
    port_.setParity(toQtParity(config_.parity));
    port_.setStopBits(config_.stopBits == 2 ? QSerialPort::TwoStop : QSerialPort::OneStop);
    port_.setFlowControl(config_.hardwareFlowControl ? QSerialPort::HardwareControl
                                                    : QSerialPort::NoFlowControl);

    if (port_.error() != QSerialPort::NoError) {
        systemError_ = port_.error();
        errorText_ = describe(systemError_, config_.portName, port_.errorString());
        close();
        return false;
    }

    port_.clear();
    return true;
}

const char* SerialTransport::lastErrorText() const noexcept {
    // 用成员缓冲而不是返回 QString 内部指针：QString 的 UTF-8 缓冲是临时的，
    // 返回它的指针会在下一次转换时失效。接口要 const char*，就得有人持有它。
    errorUtf8_ = errorText_.toUtf8();
    return errorUtf8_.constData();
}

bool SerialTransport::send(const std::uint8_t* data, std::size_t len) {
    if (!port_.isOpen() || data == nullptr || len == 0U) {
        return false;
    }

    const qint64 written = port_.write(reinterpret_cast<const char*>(data),
                                       static_cast<qint64>(len));
    if (written != static_cast<qint64>(len)) {
        // 部分写入就是部分写入：报 false 让上层知道"这一帧没完整排进去"，
        // 而不是假装成功（上层会把发送失败计入内部不变式计数器）。
        return false;
    }

    // 立即把缓冲推出去。不做 waitForBytesWritten —— 那是**阻塞**等待，
    // 在一个由定时器驱动的 GUI 线程里阻塞就是界面卡住。
    (void)port_.flush();
    return true;
}

std::size_t SerialTransport::receive(std::uint8_t* out, std::size_t maxLen) {
    if (out == nullptr || maxLen == 0U) {
        return 0U;
    }

    if (pending_.isEmpty() && port_.isOpen()) {
        pending_ = port_.readAll();
    }

    const std::size_t take =
        std::min(maxLen, static_cast<std::size_t>(pending_.size()));
    if (take == 0U) {
        return 0U;
    }

    std::memcpy(out, pending_.constData(), take);
    pending_.remove(0, static_cast<int>(take));
    return take;
}

bool SerialTransport::isOpen() const noexcept {
    return port_.isOpen();
}

void SerialTransport::close() {
    pending_.clear();
    if (port_.isOpen()) {
        port_.close();
    }
}

}  // namespace mchost
