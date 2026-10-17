#include <gtest/gtest.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "mc/device_state.hpp"
#include "mc/frame_codec.hpp"
#include "mc/payload.hpp"

namespace {

class SimProcess {
public:
    ~SimProcess() {
        if (pid_ > 0) {
            kill(pid_, SIGTERM);
            waitpid(pid_, nullptr, 0);
        }
        if (pipeRead_ >= 0) {
            close(pipeRead_);
        }
    }

    bool start(const char* executable) {
        int pipeFds[2] = {-1, -1};
        if (pipe(pipeFds) != 0) {
            return false;
        }

        pid_ = fork();
        if (pid_ < 0) {
            close(pipeFds[0]);
            close(pipeFds[1]);
            return false;
        }
        if (pid_ == 0) {
            dup2(pipeFds[1], STDOUT_FILENO);
            close(pipeFds[0]);
            close(pipeFds[1]);
            execl(executable, executable, "--pty", static_cast<char*>(nullptr));
            _exit(127);
        }

        close(pipeFds[1]);
        pipeRead_ = pipeFds[0];

        std::string line;
        char byte = 0;
        while (read(pipeRead_, &byte, 1) == 1) {
            if (byte == '\n') {
                break;
            }
            line.push_back(byte);
        }

        const std::string marker = "从端: ";
        const std::size_t at = line.find(marker);
        if (at == std::string::npos) {
            std::fprintf(stderr, "mc_fw_sim 输出: %s\n", line.c_str());
            return false;
        }
        slavePath_ = line.substr(at + marker.size());
        while (!slavePath_.empty() &&
               (slavePath_.back() == '\r' || slavePath_.back() == ' ')) {
            slavePath_.pop_back();
        }
        return !slavePath_.empty();
    }

    const std::string& slavePath() const { return slavePath_; }

private:
    pid_t pid_ = -1;
    int pipeRead_ = -1;
    std::string slavePath_;
};

int openSlave(const std::string& path) {
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

bool writeAll(int fd, const std::vector<std::uint8_t>& bytes) {
    std::size_t offset = 0U;
    while (offset < bytes.size()) {
        const ssize_t written = write(fd, bytes.data() + offset, bytes.size() - offset);
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
        } else if (written < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

class FrameReader {
public:
    explicit FrameReader(int fd) : fd_(fd) {}

    bool next(mc::Frame& out, int timeoutMs) {
        if (!queue_.empty()) {
            out = queue_.front();
            queue_.pop_front();
            return true;
        }

        pollfd pfd{};
        pfd.fd = fd_;
        pfd.events = POLLIN;
        const int ready = poll(&pfd, 1, timeoutMs);
        if (ready <= 0 || (pfd.revents & POLLIN) == 0) {
            return false;
        }

        std::uint8_t buffer[256]{};
        const ssize_t got = read(fd_, buffer, sizeof(buffer));
        if (got <= 0) {
            return false;
        }

        const std::vector<mc::Frame> frames = decoder_.feed(buffer, static_cast<std::size_t>(got));
        for (const mc::Frame& frame : frames) {
            queue_.push_back(frame);
        }
        if (queue_.empty()) {
            return false;
        }
        out = queue_.front();
        queue_.pop_front();
        return true;
    }

private:
    int fd_;
    mc::StreamDecoder decoder_;
    std::deque<mc::Frame> queue_;
};

bool sendFrame(int fd, mc::MessageType type, std::uint16_t seq,
               const std::vector<std::uint8_t>& payload) {
    mc::Frame frame;
    frame.version = mc::kProtocolVersion;
    frame.type = type;
    frame.seq = seq;
    frame.payload = payload;
    const mc::EncodeResult encoded = mc::encodeFrame(frame);
    return encoded && writeAll(fd, encoded.bytes);
}

bool waitForAck(FrameReader& reader, mc::MessageType type, std::uint16_t seq, int timeoutMs) {
    const auto start = std::chrono::steady_clock::now();
    while (true) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - start)
                                 .count();
        if (elapsed >= timeoutMs) {
            return false;
        }
        mc::Frame frame;
        if (!reader.next(frame, static_cast<int>(timeoutMs - elapsed))) {
            return false;
        }
        if (frame.type == type && frame.seq == seq) {
            return true;
        }
    }
}

}  // namespace

TEST(FirmwarePty, RealBytesReachTheFirmwareCoreAndMotionRuns) {
    SimProcess sim;
    ASSERT_TRUE(sim.start(MC_FW_SIM_PATH)) << "启动 mc_fw_sim 失败";

    const int fd = openSlave(sim.slavePath());
    ASSERT_GE(fd, 0) << sim.slavePath() << ": " << std::strerror(errno);
    FrameReader reader(fd);

    ASSERT_TRUE(sendFrame(fd, mc::MessageType::Hello, 1U, {}));
    ASSERT_TRUE(waitForAck(reader, mc::MessageType::HelloAck, 1U, 2000));

    ASSERT_TRUE(sendFrame(fd, mc::MessageType::SetMode, 2U, mc::encodeSetMode(mc::SetMode{1U})));
    ASSERT_TRUE(waitForAck(reader, mc::MessageType::SetModeAck, 2U, 2000));

    ASSERT_TRUE(sendFrame(fd, mc::MessageType::SetTarget, 3U,
                          mc::encodeSetTarget(mc::SetTarget{100.0F})));
    ASSERT_TRUE(waitForAck(reader, mc::MessageType::SetTargetAck, 3U, 2000));

    ASSERT_TRUE(sendFrame(fd, mc::MessageType::Start, 4U, {}));
    ASSERT_TRUE(waitForAck(reader, mc::MessageType::StartAck, 4U, 2000));

    bool sawRunning = false;
    float peakVelocity = 0.0F;
    for (int i = 0; i < 400 && !sawRunning; ++i) {
        mc::Frame frame;
        if (!reader.next(frame, 20)) {
            continue;
        }
        if (frame.type != mc::MessageType::Telemetry) {
            continue;
        }
        mc::Telemetry sample;
        ASSERT_EQ(mc::decodeTelemetry(frame.payload.data(), frame.payload.size(), sample),
                  mc::PayloadError::None);
        peakVelocity = sample.velocityDegPerSec > peakVelocity ? sample.velocityDegPerSec
                                                               : peakVelocity;
        sawRunning = sample.state == static_cast<std::uint8_t>(mc::DeviceState::Running) &&
                     sample.velocityDegPerSec > 10.0F;
    }
    EXPECT_TRUE(sawRunning) << "峰值速度 " << peakVelocity;

    ASSERT_TRUE(sendFrame(fd, mc::MessageType::Stop, 5U, {}));

    bool sawReady = false;
    for (int i = 0; i < 300 && !sawReady; ++i) {
        mc::Frame frame;
        if (!reader.next(frame, 20)) {
            continue;
        }
        if (frame.type != mc::MessageType::Telemetry) {
            continue;
        }
        mc::Telemetry sample;
        ASSERT_EQ(mc::decodeTelemetry(frame.payload.data(), frame.payload.size(), sample),
                  mc::PayloadError::None);
        sawReady = sample.state == static_cast<std::uint8_t>(mc::DeviceState::Ready) &&
                   sample.velocityDegPerSec < 5.0F;
    }
    EXPECT_TRUE(sawReady);

    close(fd);
}
