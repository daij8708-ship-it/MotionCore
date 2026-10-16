#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "mc/device_simulator.hpp"
#include "mc_host/device.hpp"

#include "host_test_util.hpp"

// 串口路径的真实验证 —— **用一对伪终端（PTY）代替真串口**，不用任何硬件。
//
// 为什么值得这么写：串口设备类的每一条观测值都只能从遥测里来，
// 而"遥测能不能真的从一条 tty 上跑通"这件事，回环传输测不出来（那不是 tty）。
// PTY 是内核提供的真 tty 设备（/dev/pts/N），QSerialPort 打开它、读写它，
// 走的路径与真串口完全一样，只是对面不是一块板子。
//
// 必须说清的边界：① 这是**伪终端**，不是真串口的电气层 ——
// 波特率/校验位在 PTY 上只是被记录、不起作用，所以"115200 能不能跑 100 Hz 遥测"
// 这类问题在这里**测不出来**；② 时钟由测试推进，不是真 wall-clock。
// 因此这些用例证明的是**协议与串口宿主代码的正确性**，不是硬件性能。
#if defined(__linux__)

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

namespace {

// 一对伪终端。master 端由测试自己当"设备"用，slave 端交给 QSerialPort。
class PtyPair {
public:
    PtyPair() {
        master_ = ::posix_openpt(O_RDWR | O_NOCTTY);
        if (master_ < 0) {
            return;
        }
        if (::grantpt(master_) != 0 || ::unlockpt(master_) != 0) {
            ::close(master_);
            master_ = -1;
            return;
        }
        const char* name = ::ptsname(master_);
        if (name == nullptr) {
            ::close(master_);
            master_ = -1;
            return;
        }
        slave_ = name;
        // 非阻塞：测试里"现在有没有数据"必须立刻知道，不能挂在 read 上。
        const int flags = ::fcntl(master_, F_GETFL, 0);
        ::fcntl(master_, F_SETFL, flags | O_NONBLOCK);
    }

    ~PtyPair() {
        if (master_ >= 0) {
            ::close(master_);
        }
    }

    PtyPair(const PtyPair&) = delete;
    PtyPair& operator=(const PtyPair&) = delete;

    bool valid() const noexcept { return master_ >= 0 && !slave_.empty(); }
    int master() const noexcept { return master_; }
    const std::string& slave() const noexcept { return slave_; }

    void writeToMaster(const std::uint8_t* data, std::size_t len) const {
        if (len > 0U) {
            (void)::write(master_, data, len);
        }
    }

    // 把 master 端能读的都读出来（非阻塞，读完即停）。
    std::vector<std::uint8_t> drainMaster() const {
        std::vector<std::uint8_t> out;
        std::uint8_t buffer[1024];
        for (;;) {
            const ssize_t n = ::read(master_, buffer, sizeof(buffer));
            if (n <= 0) {
                break;
            }
            out.insert(out.end(), buffer, buffer + n);
        }
        return out;
    }

private:
    int master_ = -1;
    std::string slave_;
};

mchost::SerialTransport::Config configFor(const PtyPair& pty) {
    mchost::SerialTransport::Config config;
    config.portName = QString::fromStdString(pty.slave());
    config.baudRate = 115200;
    return config;
}

}  // namespace

TEST(SerialTransport, MovesBytesBothWaysOverARealTty) {
    PtyPair pty;
    ASSERT_TRUE(pty.valid()) << "造不出伪终端，本用例无法进行";

    mchost::SerialTransport transport(configFor(pty));
    ASSERT_TRUE(transport.open()) << transport.lastErrorText();

    // master → 串口端
    const std::vector<std::uint8_t> down = {0xAAU, 0x55U, 0x01U, 0x02U, 0x03U};
    pty.writeToMaster(down.data(), down.size());
    // QSerialPort 的字节是**事件循环**搬进缓冲的（见 serial_transport.hpp 的说明），
    // 所以这里必须转事件循环 —— 直接调 receive() 会读到 0 字节。
    mchost_test::pumpFor(20);

    std::uint8_t buffer[64] = {0};
    const std::size_t got = transport.receive(buffer, sizeof(buffer));
    ASSERT_EQ(got, down.size());
    EXPECT_EQ(std::memcmp(buffer, down.data(), down.size()), 0);

    // 串口端 → master
    const std::vector<std::uint8_t> up = {0x10U, 0x20U, 0x30U};
    ASSERT_TRUE(transport.send(up.data(), up.size()));
    const std::vector<std::uint8_t> received = pty.drainMaster();
    ASSERT_EQ(received.size(), up.size());
    EXPECT_EQ(std::memcmp(received.data(), up.data(), up.size()), 0);

    transport.close();
    EXPECT_FALSE(transport.isOpen());
}

// 读多了的字节不能丢：receive() 只拿得走 maxLen，剩下的必须留在传输里等下一次。
// 真串口上一帧遥测 30 B，而上位机单拍只读 512 B —— 只要设备一密集,
// "读多了"就是常态，丢字节会被上层当成 CRC 错/重同步，排查方向立刻跑偏。
TEST(SerialTransport, KeepsTheTailItCouldNotHandOverYet) {
    PtyPair pty;
    ASSERT_TRUE(pty.valid());

    mchost::SerialTransport transport(configFor(pty));
    ASSERT_TRUE(transport.open()) << transport.lastErrorText();

    std::vector<std::uint8_t> down;
    for (std::uint8_t i = 0U; i < 20U; ++i) {
        down.push_back(i);
    }
    pty.writeToMaster(down.data(), down.size());
    mchost_test::pumpFor(20);

    std::uint8_t first[8] = {0};
    ASSERT_EQ(transport.receive(first, sizeof(first)), std::size_t{8});
    EXPECT_EQ(std::memcmp(first, down.data(), 8U), 0);

    std::uint8_t rest[64] = {0};
    ASSERT_EQ(transport.receive(rest, sizeof(rest)), std::size_t{12});
    EXPECT_EQ(std::memcmp(rest, down.data() + 8, 12U), 0);

    transport.close();
}

// 整条路：SerialDevice（QSerialPort）↔ 伪终端 ↔ 参考模拟器。
// 走的是与真设备**同一套**协议：握手、命令、应答、遥测，一个都不少。
TEST(SerialDevice, HandshakesCommandsAndReportsTelemetryOverAPty) {
    PtyPair pty;
    ASSERT_TRUE(pty.valid());

    mchost::SerialDevice device(configFor(pty), mc::LinkMonitor::Config{});
    ASSERT_TRUE(device.open()) << mchost::openErrorName(device.openError());
    EXPECT_EQ(device.openError(), mchost::OpenError::None);
    EXPECT_TRUE(device.isOpen());
    // 串口设备的能力位：不作故障注入、模式不可回读。
    EXPECT_FALSE(device.supportsFaultInjection());
    EXPECT_FALSE(device.runModeIsReadBack());

    mc::SimulatorConfig simConfig;
    simConfig.telemetryPeriodMs = 10U;
    mc::DeviceSimulator sim(simConfig);

    std::uint64_t now = 0U;
    const auto step = [&](std::uint32_t dtMs) {
        now += dtMs;
        // ① 设备侧待发字节 → master，并让 Qt 的事件循环把它们搬进缓冲。
        std::uint8_t fromSim[1024];
        const std::size_t up = sim.readOutgoing(fromSim, sizeof(fromSim));
        pty.writeToMaster(fromSim, up);
        mchost_test::pumpFor(2);

        // ② 上位机推进一拍（读缓冲 → 拆帧 → 分发；同时把命令写到端口上）。
        (void)device.tick(now);

        // ③ 上位机刚发出去的字节 → master → 喂给设备侧模拟器。
        //
        // ⚠️ 设备侧**每一拍都要推进**，哪怕这一拍没有收到任何字节：
        //    它自己的积分、F7 计时、以及"按自己的周期产遥测"都挂在 tick 上。
        //    第一版写成"有字节才 tick"，结果遥测一帧都发不出来（速度恒为 0）——
        //    那正是真设备**不依赖上位机说话**也能上报的证据。
        const std::vector<std::uint8_t> down = pty.drainMaster();
        static const std::uint8_t kNoBytes[1] = {0U};
        (void)sim.tick(down.empty() ? kNoBytes : down.data(), down.size(), now);
    };

    for (int i = 0; i < 40 && !device.sessionReady(); ++i) {
        step(10U);
    }
    ASSERT_TRUE(device.sessionReady()) << "在一条真实的 tty 上都没能握上手";
    EXPECT_EQ(device.handshakeRounds(), 1ULL);

    ASSERT_TRUE(device.setMode(1U));          // 速度环
    ASSERT_TRUE(device.start());
    ASSERT_TRUE(device.setTarget(200.0F));
    for (int i = 0; i < 60; ++i) {
        step(10U);
    }

    // 观测值全部来自遥测帧 —— 这就是串口与模拟器最大的区别，必须能真的读到值。
    EXPECT_GT(device.telemetryAcceptedTotal(), 20ULL);
    EXPECT_EQ(device.telemetryRejectedTotal(), 0ULL);
    EXPECT_EQ(device.deviceState(), mc::DeviceState::Running);
    EXPECT_TRUE(device.enabled());
    EXPECT_GT(device.velocityDegPerSec(), 50.0F);
    EXPECT_NEAR(device.deviceTarget(), 200.0F, 1.0F);
    EXPECT_FALSE(device.faultLatched());

    // 模式是"上位机最后一次被确认的设定值"，不是回读 —— 值对，但来源要说清。
    EXPECT_EQ(device.runMode(), mc::SimRunMode::Velocity);

    // 命令行里四条命令都走到了对端并在那之前被确认。
    std::size_t acked = 0U;
    for (const mchost::CommandRecord& record : device.commandLog()) {
        if (record.status == mchost::CommandStatus::Acked) {
            ++acked;
        }
    }
    EXPECT_EQ(acked, std::size_t{3}) << "SET_MODE / START / SET_TARGET 都该被确认";
    EXPECT_EQ(device.hostEncodeFailures(), 0ULL);
    EXPECT_EQ(device.hostSendFailures(), 0ULL);
    EXPECT_EQ(sim.encodeFailures(), 0ULL);

    device.close();
    EXPECT_FALSE(device.isOpen());
}

// 打不开的端口必须**明确失败**且说得出原因：真硬件上这是常态
// （端口被别的程序占着、线没插、权限不够），不是异常。
TEST(SerialDevice, AFailedOpenIsReportedWithAReason) {
    mchost::SerialTransport::Config config;
    config.portName = QStringLiteral("__no_such_port__");

    mchost::SerialDevice device(config, mc::LinkMonitor::Config{});
    EXPECT_FALSE(device.open());
    EXPECT_NE(device.openError(), mchost::OpenError::None);
    EXPECT_FALSE(device.isOpen());
    // 没打开也不能崩：所有观测访问器都要能安全地被调用（界面就是这么用的）。
    EXPECT_EQ(device.deviceState(), mc::DeviceState::Disconnected);
    EXPECT_FALSE(device.sessionReady());
    EXPECT_EQ(device.telemetryAcceptedTotal(), 0ULL);
}

#endif   // __linux__
