#include "mc/fd_transport.hpp"

#if defined(__unix__) || defined(__APPLE__)

#include <cerrno>
#include <unistd.h>

namespace mc {

FdTransport::FdTransport(int fd, bool ownsFd) : fd_(fd), ownsFd_(ownsFd) {}

FdTransport::~FdTransport() { close(); }

bool FdTransport::send(const std::uint8_t* data, std::size_t len) {
    if (fd_ < 0) {
        return false;
    }
    std::size_t written = 0U;
    while (written < len) {
        const ssize_t n = ::write(fd_, data + written, len - written);
        if (n > 0) {
            written += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // 非阻塞 fd 上写入缓冲满了：这一拍发不完，剩下的留到下一拍。
            // 记在账上的是**确实写出去**的字节数，不是"想发的"字节数 ——
            // 后者会让"设备发了多少 / 上位机读了多少"永远对不上。
            counters_.bytesSent += written;
            return false;
        }
        ++counters_.writeErrors;
        counters_.bytesSent += written;
        return false;
    }
    counters_.bytesSent += written;
    return true;
}

std::size_t FdTransport::receive(std::uint8_t* out, std::size_t maxLen) {
    if (fd_ < 0 || out == nullptr || maxLen == 0U) {
        return 0U;
    }
    for (;;) {
        const ssize_t n = ::read(fd_, out, maxLen);
        if (n > 0) {
            counters_.bytesReceived += static_cast<std::size_t>(n);
            return static_cast<std::size_t>(n);
        }
        if (n == 0) {
            // 对端关闭（EOF）。返回 0，由调用方用 isOpen() 区分"没数据"与"断了"。
            return 0U;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0U;
        }
        // EIO 等：PTY 的 master 端在没有从端打开时就是 EIO，属常见情形，不是故障。
        // 计数但不返回错误：把"对面还没接上"当成链路坏掉，会让 mc_sim 在
        // 上位机连上来之前就退出。
        ++counters_.readErrors;
        return 0U;
    }
}

bool FdTransport::isOpen() const noexcept { return fd_ >= 0; }

void FdTransport::close() {
    if (fd_ >= 0 && ownsFd_) {
        ::close(fd_);
    }
    fd_ = -1;
    ownsFd_ = false;
}

}  // namespace mc

#endif  // __unix__ || __APPLE__
