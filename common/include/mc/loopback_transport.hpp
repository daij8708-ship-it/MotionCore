#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "mc/transport.hpp"

namespace mc {

struct LinkChannel;   // 内部共享状态，定义在 .cpp

struct LoopbackPair;  // 定义在本头文件末尾；此处前置声明供友元使用

// 单向链路上的故障注入配置。
// **默认全部为"无故障"**，且没有任何隐含有意义的取值 ——
// 想丢字节必须显式写 dropEveryNthByte = N。
struct LinkFaultConfig {
    std::uint32_t dropEveryNthByte = 0U;      // 0 = 不丢；N = 每第 N 个字节丢弃
    std::uint32_t corruptEveryNthByte = 0U;   // 0 = 不损坏；N = 每第 N 个字节取反
    std::uint32_t latencyPolls = 0U;          // 该字节要经过几次 receive 才可见
    std::size_t maxQueueBytes = 65536U;       // 有界队列：超出即丢，并计数
};

struct LinkStats {
    std::uint64_t bytesSent = 0;
    std::uint64_t bytesReceived = 0;
    std::uint64_t bytesDropped = 0;        // 故障注入丢弃
    std::uint64_t bytesCorrupted = 0;      // 故障注入损坏
    std::uint64_t bytesLostToOverflow = 0; // 队列上限导致丢失
};

// ---------------------------------------------------------------------------
// 进程内回环传输：一对端点，字节从一个端点流向另一个端点。
//
// 用途：在没有硬件、也不想拉起真实串口的情况下，把"上位机 ↔ 设备"整条链路跑起来。
// 两个方向**各自**有独立的故障配置与统计 —— 例如
//   pair.device->outgoingFaults().dropEveryNthByte = 3;
// 表示"设备发出的字节每 3 个丢 1 个"。
// ---------------------------------------------------------------------------
class LoopbackTransport final : public ITransport {
public:
    ~LoopbackTransport() override;

    bool send(const std::uint8_t* data, std::size_t len) override;
    std::size_t receive(std::uint8_t* out, std::size_t maxLen) override;
    bool isOpen() const noexcept override;
    void close() override;

    // 本端**发送方向**上的故障注入与统计。
    LinkFaultConfig& outgoingFaults() noexcept;
    const LinkStats& outgoingStats() const noexcept;

    // 本端**接收方向**上的统计。
    const LinkStats& incomingStats() const noexcept;

    // 共享一条通道的对象数（=2）。仅用于测试，确认没有额外的所有权。
    static std::size_t channelUseCount(const LoopbackTransport& endpoint) noexcept;

private:
    friend LoopbackPair makeLoopbackPair();
    LoopbackTransport(std::shared_ptr<LinkChannel> writeChannel,
                      std::shared_ptr<LinkChannel> readChannel);

    std::shared_ptr<LinkChannel> writeChannel_;
    std::shared_ptr<LinkChannel> readChannel_;
};

struct LoopbackPair {
    std::shared_ptr<LoopbackTransport> host;
    std::shared_ptr<LoopbackTransport> device;
};

// 建立一对已对接好的端点。
LoopbackPair makeLoopbackPair();

}  // namespace mc
