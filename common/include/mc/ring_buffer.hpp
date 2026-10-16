#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <type_traits>

namespace mc {

// 固定容量环形缓冲。
//
// 设计取舍（与 StreamDecoder 的内部暂存区刻意不同）：
//   本缓冲**允许丢失**：写满后覆盖最旧的元素，并累加 discarded()。
//   它面向"实时遥测显示"这类场景 —— 显示跟不上时宁可丢旧数据，也不能无界增长。
//   StreamDecoder 的暂存区则**不允许丢失**（丢字节 = 协议解析错），
//   所以它用 std::vector 而不是本类型。两者不可互换。
//
// 内存占用恒定：容量由模板参数决定，底层是 std::array，运行期不分配堆内存。
template <typename T, std::size_t Capacity>
class RingBuffer {
    static_assert(Capacity > 0U, "RingBuffer 容量必须大于 0");

public:
    static constexpr std::size_t capacity() noexcept { return Capacity; }

    bool empty() const noexcept { return size_ == 0U; }
    bool full() const noexcept { return size_ == Capacity; }
    std::size_t size() const noexcept { return size_; }

    // 写满后覆盖最旧元素并累加 discarded()。
    void push(const T& value) noexcept(std::is_nothrow_copy_assignable<T>::value) {
        if (size_ == Capacity) {
            buf_[head_] = value;
            head_ = (head_ + 1U) % Capacity;
            ++discarded_;
            return;
        }
        buf_[(head_ + size_) % Capacity] = value;
        ++size_;
    }

    // 空则返回 false 且不修改 out。
    bool pop(T& out) noexcept(std::is_nothrow_copy_assignable<T>::value) {
        if (size_ == 0U) {
            return false;
        }
        out = buf_[head_];
        head_ = (head_ + 1U) % Capacity;
        --size_;
        return true;
    }

    // i == 0 取最旧元素。越界抛 std::out_of_range。
    const T& at(std::size_t i) const {
        if (i >= size_) {
            throw std::out_of_range("RingBuffer::at: index out of range");
        }
        return buf_[(head_ + i) % Capacity];
    }

    // 因覆盖而丢弃的元素累计数。
    std::uint64_t discarded() const noexcept { return discarded_; }

    // 只清空元素。discarded() 是累计统计量，不随清空归零 ——
    // 这样"重置显示缓冲"不会把丢包统计一起抹掉。
    void clear() noexcept {
        head_ = 0U;
        size_ = 0U;
    }

    // 需要把统计量也归零时，显式调用（与 clear() 分开，避免两种语义混淆）。
    void resetCounters() noexcept { discarded_ = 0U; }

private:
    std::array<T, Capacity> buf_{};
    std::size_t head_ = 0U;
    std::size_t size_ = 0U;
    std::uint64_t discarded_ = 0U;
};

}  // namespace mc
