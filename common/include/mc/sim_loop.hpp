#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "mc/device_simulator.hpp"
#include "mc/transport.hpp"

namespace mc {

// ---------------------------------------------------------------------------
// 设备侧的"一拍"
//
// 与 HostSession::pump() 是同一件事的两端：读对端发来的字节 → 推进设备模型
// → 把设备要发的字节送回去。
//
// 抽出来的理由不是"少写几行"，而是让**进程内回环**与**一条真 tty** 跑同一段
// 设备侧代码：tty 上那些回环上不存在的现象（半包、EAGAIN、从端未打开的 EIO）
// 才会被走到，而不是只在 mc_sim 里各写一份。
//
// 时间由调用方以 nowMs 传入：真串口模式传单调时钟，测试传合成时钟。
//
// Config 刻意放在类外面（与 HandshakeConfig 同一个原因）：嵌套类型的默认成员
// 初始化器在**外层类还没定义完**时不可用，GCC 会直接拒绝 `Config config = Config{}`
// 这种默认实参。
// ---------------------------------------------------------------------------
struct SimLoopConfig {
    // 上行（设备→上位机）故障注入：每第 N 个字节取反，用来制造 CRC 错。
    // 0 = 不损坏。**默认值没有任何隐含有意义的取值**：要注入必须显式写 N。
    std::uint32_t corruptEveryNthByte = 0U;

    // 单拍最多读多少字节。与上位机 pump 的 maxBytesPerPump 同量级，理由相同：
    // 给"这一拍读不完"留下可观测的余地，而不是假设一次能读干净。
    std::size_t maxReadBytes = 512U;

    // 单拍最多取走多少待发字节。一帧遥测 30 B、单拍上限 kMaxTelemetryFramesPerTick=16
    // 帧（480 B），4096 B 给应答帧留足了余量。
    std::size_t maxWriteBytes = 4096U;
};

class SimLoop {
public:
    // 逐项累计设备侧发生的事。**设备侧有多少帧没能发出去**必须看得见：
    // 只说"设备发了 N 帧"会把"没取完"和"真的只产了这么多"混为一谈。
    struct Counters {
        std::uint64_t rounds = 0;
        std::uint64_t bytesReadFromPeer = 0;
        std::uint64_t bytesWrittenToPeer = 0;   // 仅**整帧全部写出**时才累加
        std::uint64_t bytesCorrupted = 0;
        std::uint64_t sendFailures = 0;         // 传输拒收 / 只写出去一部分
        std::size_t framesReceived = 0;
        std::size_t acksSent = 0;
        std::size_t nacksSent = 0;
        std::size_t telemetrySent = 0;
        std::size_t duplicatesSuppressed = 0;
        std::size_t malformedPayloads = 0;
    };

    SimLoop(DeviceSimulator& device, ITransport& link, SimLoopConfig config = SimLoopConfig{});

    void step(std::uint64_t nowMs);

    const Counters& counters() const noexcept { return counters_; }
    const SimLoopConfig& config() const noexcept { return config_; }

private:
    DeviceSimulator& device_;
    ITransport& link_;
    SimLoopConfig config_;
    std::vector<std::uint8_t> readBuffer_;
    std::vector<std::uint8_t> writeBuffer_;
    // 跨拍累计的字节序号：故障注入的"每第 N 个"是**整条上行流**上的序号，
    // 不是每拍重新数 —— 后者会让 N=2 变成"每帧前两个字节之一"，与配置说的不是一回事。
    std::uint32_t corruptOrdinal_ = 0U;
    Counters counters_{};
};

}  // namespace mc
