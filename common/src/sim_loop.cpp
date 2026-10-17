#include "mc/sim_loop.hpp"

namespace mc {

SimLoop::SimLoop(DeviceSimulator& device, ITransport& link, SimLoopConfig config)
    : device_(device), link_(link), config_(config) {
    readBuffer_.resize(config_.maxReadBytes);
    writeBuffer_.resize(config_.maxWriteBytes);
}

void SimLoop::step(std::uint64_t nowMs) {
    ++counters_.rounds;

    // ① 对端发来的字节 → 设备模型。
    const std::size_t got = link_.receive(readBuffer_.data(), readBuffer_.size());
    counters_.bytesReadFromPeer += got;

    const DeviceSimulator::TickResult tick =
        device_.tick(got > 0U ? readBuffer_.data() : nullptr, got, nowMs);
    counters_.framesReceived += tick.framesReceived;
    counters_.acksSent += tick.acksSent;
    counters_.nacksSent += tick.nacksSent;
    counters_.telemetrySent += tick.telemetrySent;
    counters_.duplicatesSuppressed += tick.duplicatesSuppressed;
    counters_.malformedPayloads += tick.malformedPayloads;

    // ② 设备待发的字节 → 对端。
    const std::size_t produced = device_.readOutgoing(writeBuffer_.data(), writeBuffer_.size());
    if (produced == 0U) {
        return;
    }

    if (config_.corruptEveryNthByte > 0U) {
        for (std::size_t i = 0U; i < produced; ++i) {
            ++corruptOrdinal_;
            if ((corruptOrdinal_ % config_.corruptEveryNthByte) == 0U) {
                writeBuffer_[i] = static_cast<std::uint8_t>(writeBuffer_[i] ^ 0xFFU);
                ++counters_.bytesCorrupted;
            }
        }
    }

    if (link_.send(writeBuffer_.data(), produced)) {
        counters_.bytesWrittenToPeer += produced;
    } else {
        // 非阻塞 fd 上"这一拍没全写出去"是常态，不是错误。
        // 到底写出去多少由**传输自己**记账（FdTransport::counters().bytesSent），
        // 本处不猜 —— 猜出来的数字会让两端对不上账。
        ++counters_.sendFailures;
    }
}

}  // namespace mc
