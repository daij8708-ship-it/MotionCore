#include "mc/loopback_transport.hpp"

#include <deque>

namespace mc {

namespace {

struct PendingByte {
    std::uint64_t releaseAt;   // 读取端的 poll 计数达到该值后才可见
    std::uint8_t byte;
};

}  // namespace

// 一条单向通道的共享状态。两个端点各持有一个 shared_ptr ——
// 通道本身不反向持有端点，因此不会形成 shared_ptr 环。
struct LinkChannel {
    std::deque<PendingByte> queue;
    std::uint64_t readerPolls = 0U;   // 读取端调用 receive() 的次数
    std::uint32_t sendCounter = 0U;   // 发送端累计字节数，用于"每第 N 个"故障注入
    bool open = true;
    LinkFaultConfig faults;
    LinkStats stats;
};

LoopbackTransport::LoopbackTransport(std::shared_ptr<LinkChannel> writeChannel,
                                     std::shared_ptr<LinkChannel> readChannel)
    : writeChannel_(std::move(writeChannel)), readChannel_(std::move(readChannel)) {}

LoopbackTransport::~LoopbackTransport() = default;

bool LoopbackTransport::send(const std::uint8_t* data, std::size_t len) {
    if (!isOpen() || (data == nullptr && len > 0U)) {
        return false;
    }

    LinkChannel& channel = *writeChannel_;
    bool complete = true;

    for (std::size_t i = 0U; i < len; ++i) {
        ++channel.stats.bytesSent;
        ++channel.sendCounter;

        if (channel.faults.dropEveryNthByte != 0U &&
            (channel.sendCounter % channel.faults.dropEveryNthByte) == 0U) {
            ++channel.stats.bytesDropped;
            continue;
        }

        std::uint8_t byte = data[i];
        if (channel.faults.corruptEveryNthByte != 0U &&
            (channel.sendCounter % channel.faults.corruptEveryNthByte) == 0U) {
            byte = static_cast<std::uint8_t>(byte ^ 0xFFU);
            ++channel.stats.bytesCorrupted;
        }

        if (channel.queue.size() >= channel.faults.maxQueueBytes) {
            // 有界队列：丢的是**新到**的字节（与 UART 接收 FIFO 溢出的行为一致），
            // 绝不静默 —— 计入统计并让 send 返回 false。
            ++channel.stats.bytesLostToOverflow;
            complete = false;
            continue;
        }

        channel.queue.push_back(PendingByte{channel.readerPolls + 1U + channel.faults.latencyPolls, byte});
    }

    return complete;
}

std::size_t LoopbackTransport::receive(std::uint8_t* out, std::size_t maxLen) {
    if (out == nullptr || maxLen == 0U) {
        return 0U;
    }

    LinkChannel& channel = *readChannel_;
    ++channel.readerPolls;

    std::size_t count = 0U;
    while (count < maxLen && !channel.queue.empty() &&
           channel.queue.front().releaseAt <= channel.readerPolls) {
        out[count] = channel.queue.front().byte;
        channel.queue.pop_front();
        ++count;
    }

    channel.stats.bytesReceived += count;
    return count;
}

bool LoopbackTransport::isOpen() const noexcept {
    return writeChannel_->open && readChannel_->open;
}

void LoopbackTransport::close() {
    // 关掉本端持有的两条通道 —— 对端共享同一批通道对象，因此也会立刻看到 !isOpen()。
    writeChannel_->open = false;
    readChannel_->open = false;
}

LinkFaultConfig& LoopbackTransport::outgoingFaults() noexcept {
    return writeChannel_->faults;
}

const LinkStats& LoopbackTransport::outgoingStats() const noexcept {
    return writeChannel_->stats;
}

const LinkStats& LoopbackTransport::incomingStats() const noexcept {
    return readChannel_->stats;
}

std::size_t LoopbackTransport::channelUseCount(const LoopbackTransport& endpoint) noexcept {
    std::size_t count = endpoint.writeChannel_.use_count();
    if (endpoint.readChannel_ != endpoint.writeChannel_) {
        count += endpoint.readChannel_.use_count();
    }
    return count;
}

LoopbackPair makeLoopbackPair() {
    auto hostToDevice = std::make_shared<LinkChannel>();
    auto deviceToHost = std::make_shared<LinkChannel>();

    auto host = std::shared_ptr<LoopbackTransport>(
        new LoopbackTransport(hostToDevice, deviceToHost));
    auto device = std::shared_ptr<LoopbackTransport>(
        new LoopbackTransport(deviceToHost, hostToDevice));

    return LoopbackPair{host, device};
}

}  // namespace mc
