// mc_fw_sim —— 用**固件核心**在宿主上扮演设备。
//
// 它不是又一份仿真器：收发、拆帧、状态机、运动模型全部走 firmware/ 里
// 与 STM32 完全相同的那份代码；只是把 UART 换成了 PTY/串口。
// 这样在板子到之前，真上位机就能先和"将来要烧进去的逻辑"跑一遍端到端。
//
//   mc_fw_sim --pty                 开一对伪终端，打印从端路径
//   mc_fw_sim --port /dev/ttyUSB0   在具名串口上扮演设备
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "mc/pty.hpp"
#include "mc_fw/device.hpp"

namespace {

volatile std::sig_atomic_t gRunning = 1;

void onSignal(int) { gRunning = 0; }

std::uint32_t monotonicMs() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(now.tv_sec) * 1000ULL +
        static_cast<std::uint64_t>(now.tv_nsec) / 1000000ULL);
}

int openSerial(const std::string& path) {
    const int fd = open(path.c_str(), O_RDWR | O_NOCTTY);
    if (fd < 0) {
        return -1;
    }
    termios tio{};
    if (tcgetattr(fd, &tio) != 0) {
        close(fd);
        return -1;
    }
    cfmakeraw(&tio);
    cfsetispeed(&tio, B115200);
    cfsetospeed(&tio, B115200);
    tio.c_cflag |= CLOCAL | CREAD;
    if (tcsetattr(fd, TCSANOW, &tio) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

bool writeAll(int fd, const std::uint8_t* data, std::size_t len) {
    std::size_t offset = 0U;
    while (offset < len) {
        const ssize_t written = write(fd, data + offset, len - offset);
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
        } else if (written < 0 && (errno == EINTR)) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

void printUsage() {
    std::printf(
        "mc_fw_sim —— 固件核心的宿主端设备\n"
        "\n"
        "  用法:\n"
        "    mc_fw_sim --pty                开一对伪终端并打印从端路径\n"
        "    mc_fw_sim --port <设备路径>    在具名串口上扮演设备\n"
        "\n"
        "  说明: 这里的固件逻辑与 firmware/port/stm32f103 用的是同一份核心；\n"
        "        区别只在于 UART 换成了 PTY/串口。执行器与编码器未接，\n"
        "        位置/速度来自固件内部的虚拟运动模型。\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::string port;
    bool usePty = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--pty") {
            usePty = true;
        } else if (arg == "--port" && i + 1 < argc) {
            port = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
        } else {
            std::fprintf(stderr, "未知参数: %s\n", arg.c_str());
            printUsage();
            return 2;
        }
    }

    int fd = -1;
    if (usePty) {
        std::string slavePath;
        fd = mc::openPtyPair(&slavePath);
        if (fd < 0) {
            std::fprintf(stderr, "开伪终端失败\n");
            return 1;
        }
        std::printf("从端: %s\n", slavePath.c_str());
        std::fflush(stdout);
    } else if (!port.empty()) {
        fd = openSerial(port);
        if (fd < 0) {
            std::fprintf(stderr, "打开串口失败: %s (%s)\n", port.c_str(), std::strerror(errno));
            return 1;
        }
    } else {
        printUsage();
        return 2;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    mcfw::Device device;
    mcfw::FrameParser parser;
    std::uint32_t lastTick = monotonicMs();

    while (gRunning != 0) {
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        const int ready = poll(&pfd, 1, 1);

        if (ready > 0 && (pfd.revents & POLLIN) != 0) {
            std::uint8_t incoming[64]{};
            const ssize_t got = read(fd, incoming, sizeof(incoming));
            if (got > 0) {
                const std::uint32_t now = monotonicMs();
                for (ssize_t i = 0; i < got; ++i) {
                    mcfw::ParsedFrame frame;
                    if (parser.push(incoming[i], frame)) {
                        device.handleFrame(frame, now);
                    }
                }
            } else if (got == 0) {
                // 对端还没打开从端：继续等，不要退出。
            } else if (errno != EINTR && errno != EAGAIN) {
                // PTY 从端未打开时可能出现 EIO；等对端连上来即可。
            }
        }

        const std::uint32_t now = monotonicMs();
        if (now != lastTick) {
            device.tick(now);
            lastTick = now;
        }

        std::uint8_t outgoing[mcfw::kMaxFrameBytes]{};
        std::size_t written = 0U;
        while ((written = device.popFrame(outgoing, sizeof(outgoing))) > 0U) {
            if (!writeAll(fd, outgoing, written)) {
                // 对端暂时不在线：丢掉这一帧，下一拍遥测还会再来。
                break;
            }
        }
    }

    close(fd);
    return 0;
}
