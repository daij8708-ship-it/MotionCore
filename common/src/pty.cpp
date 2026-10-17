#include "mc/pty.hpp"

#if defined(__unix__) || defined(__APPLE__)

#include <fcntl.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

namespace mc {

int openPtyPair(std::string* slavePath) {
    const int master = ::posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0) {
        return -1;
    }
    if (::grantpt(master) != 0 || ::unlockpt(master) != 0) {
        ::close(master);
        return -1;
    }
    const char* name = ::ptsname(master);
    if (name == nullptr) {
        ::close(master);
        return -1;
    }

    const std::string slave = name;

    // 打开从端只是为了把 termios 设成 raw，设完就关；pty 由 master 保活。
    // （关掉从端之后，master 的 read 会返回 EIO，直到对端把从端打开 ——
    //   FdTransport 把这种 EIO 按"还没有数据"处理。）
    const int slaveFd = ::open(slave.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (slaveFd >= 0) {
        termios tty{};
        if (::tcgetattr(slaveFd, &tty) == 0) {
            ::cfmakeraw(&tty);
            (void)::tcsetattr(slaveFd, TCSANOW, &tty);
        }
        ::close(slaveFd);
    }

    // master 非阻塞：设备侧的循环是"有没有数据立刻知道"，不能挂在 read 上。
    const int flags = ::fcntl(master, F_GETFL, 0);
    if (flags >= 0) {
        (void)::fcntl(master, F_SETFL, flags | O_NONBLOCK);
    }

    if (slavePath != nullptr) {
        *slavePath = slave;
    }
    return master;
}

}  // namespace mc

#endif  // __unix__ || __APPLE__
