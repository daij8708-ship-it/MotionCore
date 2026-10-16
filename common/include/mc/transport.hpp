#pragma once

#include <cstddef>
#include <cstdint>

namespace mc {

// ---------------------------------------------------------------------------
// 传输抽象
//
// 刻意设计成**轮询**而不是回调，因为两个使用方天然都是轮询的：
//   - Qt 侧：QSerialPort 的 readyRead 信号到达后调 receive()
//   - 模拟器 / 命令行：主循环里每个 tick 调一次
// 这样 libmc_core 不必知道任何事件循环，也就不会把 Qt 拖进核心库（ADR-001）。
//
// 语义约定（含糊的返回值都必须能分辨）：
//   send()     全部字节成功排入发送队列返回 true；有任何字节因队列上限被丢返回 false。
//   receive()  返回本次实际读到的字节数。**0 有两种含义**：暂无数据，或传输已关闭 ——
//              调用方用 isOpen() 区分，不要让 0 既表示"没数据"又表示"断了"。
//   close()    幂等。
// ---------------------------------------------------------------------------
class ITransport {
public:
    virtual ~ITransport() = default;

    virtual bool send(const std::uint8_t* data, std::size_t len) = 0;
    virtual std::size_t receive(std::uint8_t* out, std::size_t maxLen) = 0;
    virtual bool isOpen() const noexcept = 0;
    virtual void close() = 0;
};

}  // namespace mc
