#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "mc/device_simulator.hpp"
#include "mc/frame_codec.hpp"
#include "mc/loopback_transport.hpp"
#include "mc/sim_loop.hpp"

#if defined(__unix__) || defined(__APPLE__)
#include "mc/fd_transport.hpp"
#include "mc/pty.hpp"

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

// 设备侧"一拍"（SimLoop）与设备侧传输（FdTransport / 伪终端）的用例。
//
// 这一层单独测的理由：真链路上那些**回环传输上不存在**的现象（半包、EAGAIN、
// 还没有对端时的 EIO）只会在这里出现，而 mc_sim 与上位机打交道时天天在踩。
// 把它们关在单元用例里，比等"上位机连不上 mc_sim"再去查要省事得多。

namespace {

std::vector<std::uint8_t> encodeFrameBytes(mc::MessageType type, std::uint16_t seq,
                                          std::vector<std::uint8_t> payload = {}) {
    mc::Frame frame;
    frame.type = type;
    frame.seq = seq;
    frame.payload = std::move(payload);
    const mc::EncodeResult encoded = mc::encodeFrame(frame);
    EXPECT_TRUE(static_cast<bool>(encoded));
    return encoded ? encoded.bytes : std::vector<std::uint8_t>{};
}

// 把某个传输这一拍能读到的字节全部取走（读到 0 为止）。
std::vector<std::uint8_t> drain(mc::ITransport& transport) {
    std::vector<std::uint8_t> out;
    std::uint8_t buffer[512];
    for (;;) {
        const std::size_t n = transport.receive(buffer, sizeof(buffer));
        if (n == 0U) {
            return out;
        }
        out.insert(out.end(), buffer, buffer + n);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// SimLoop：读对端 → 推进设备 → 把设备要发的写回去
// ---------------------------------------------------------------------------

// 正对照：一整轮往返能在**任意**传输上跑通（这里用进程内回环）。
// SimLoop 不知道链路是什么，因此同一段代码既伺候回环也伺候真 tty。
TEST(SimLoop, CarriesARequestAndItsReplyOverAnyTransport) {
    mc::LoopbackPair pair = mc::makeLoopbackPair();
    mc::DeviceSimulator device;
    mc::SimLoop loop(device, *pair.device);

    const std::vector<std::uint8_t> hello = encodeFrameBytes(mc::MessageType::Hello, 7U);
    ASSERT_FALSE(hello.empty());
    ASSERT_TRUE(pair.host->send(hello.data(), hello.size()));

    loop.step(10U);

    EXPECT_EQ(device.state(), mc::DeviceState::Ready) << "握手这一轮走通了才可能到 Ready";
    EXPECT_EQ(loop.counters().framesReceived, std::size_t{1});
    EXPECT_EQ(loop.counters().acksSent, std::size_t{1});
    EXPECT_EQ(loop.counters().bytesReadFromPeer, static_cast<std::uint64_t>(hello.size()));

    const std::vector<std::uint8_t> reply = drain(*pair.host);
    mc::StreamDecoder decoder;
    const std::vector<mc::Frame> frames = decoder.feed(reply.data(), reply.size());
    ASSERT_EQ(frames.size(), std::size_t{1});
    EXPECT_EQ(frames[0].type, mc::MessageType::HelloAck);
    EXPECT_EQ(frames[0].seq, 7U);

    // 传输拒收与损坏计数在健康链路上必须是 0 —— 否则"设备发了多少"这个数字不可信。
    EXPECT_EQ(loop.counters().sendFailures, std::uint64_t{0});
    EXPECT_EQ(loop.counters().bytesCorrupted, std::uint64_t{0});
}

// 设备不依赖上位机说话也会上报：连续推进（对端一个字都不发）时遥测照常产出。
// 这条钉住的是"真设备是主动上报的"，不是"上位机问一句它答一句"。
TEST(SimLoop, TheDeviceKeepsReportingTelemetryWithoutBeingAsked) {
    mc::LoopbackPair pair = mc::makeLoopbackPair();
    mc::DeviceSimulator device;
    mc::SimLoop loop(device, *pair.device);

    for (std::uint64_t i = 1U; i <= 20U; ++i) {
        loop.step(i * 10U);
    }

    EXPECT_EQ(loop.counters().framesReceived, std::size_t{0});
    // 第一拍只用于对齐时间基准（此刻还不存在"过去一个周期"），所以 N 拍产出 N-1 帧。
    EXPECT_EQ(loop.counters().telemetrySent, std::size_t{19}) << "之后每拍一帧（10 ms 周期）";
    EXPECT_GT(loop.counters().bytesWrittenToPeer, std::uint64_t{0});
}

// 上行损坏的**语义**：序号是"整条上行流"上的，不是每拍重新数。
// 若每拍重数，N=5 会变成"每拍前 5 个字节里坏一个"，与配置说的完全不是一回事 ——
// 而计数器会把这件事暴露出来：损坏数必须等于 产出字节数 / N。
TEST(SimLoop, CorruptsEveryNthByteOfTheWholeUplinkStream) {
    constexpr std::uint32_t kEveryNth = 5U;

    mc::LoopbackPair pair = mc::makeLoopbackPair();
    mc::DeviceSimulator device;
    mc::SimLoopConfig config;
    config.corruptEveryNthByte = kEveryNth;
    mc::SimLoop loop(device, *pair.device, config);

    std::vector<std::uint8_t> received;
    for (std::uint64_t i = 1U; i <= 20U; ++i) {
        loop.step(i * 10U);
        const std::vector<std::uint8_t> chunk = drain(*pair.host);
        received.insert(received.end(), chunk.begin(), chunk.end());
    }

    ASSERT_GT(loop.counters().bytesWrittenToPeer, std::uint64_t{0});
    EXPECT_EQ(loop.counters().bytesCorrupted, loop.counters().bytesWrittenToPeer / kEveryNth)
        << "损坏计数必须与「整条流上每第 N 个」一致（回环不会拒收，所以写出==产出）";
    EXPECT_EQ(received.size(), static_cast<std::size_t>(loop.counters().bytesWrittenToPeer));

    mc::StreamDecoder decoder;
    const std::vector<mc::Frame> frames = decoder.feed(received.data(), received.size());
    EXPECT_GT(decoder.stats().framesCrcError, std::uint64_t{0})
        << "损坏必须能被解码器当成 CRC 错拒掉，而不是悄悄进数据通路";

    // N 比一帧还短的时候，**每一帧**都必然被打坏：一帧遥测 30 B，N=5 意味着
    // 每帧里恰好有 6 个字节被取反。所以这里一帧都解不出来 —— 这正是"注入生效"
    // 的直接证据。"只坏一部分、好帧照常解出来"是 N 大于帧长时的行为，
    // 由 mc_sim 自检的 ② 覆盖（那里 N=128）。
    EXPECT_EQ(frames.size(), std::size_t{0})
        << "N=5 < 帧长 30，不该还有帧能通过 CRC";
}

// 负对照：不配置注入就一个字节都不许改。**默认值没有隐含有意义的取值** ——
// 这条用例是它的门禁：哪天有人把默认值改成"每 2 个坏一个"，这里立刻红。
TEST(SimLoop, DoesNotCorruptAnythingWhenInjectionIsOff) {
    mc::LoopbackPair pair = mc::makeLoopbackPair();
    mc::DeviceSimulator device;
    mc::SimLoop loop(device, *pair.device);   // 默认配置

    std::vector<std::uint8_t> received;
    for (std::uint64_t i = 1U; i <= 10U; ++i) {
        loop.step(i * 10U);
        const std::vector<std::uint8_t> chunk = drain(*pair.host);
        received.insert(received.end(), chunk.begin(), chunk.end());
    }

    EXPECT_EQ(loop.counters().bytesCorrupted, std::uint64_t{0});
    mc::StreamDecoder decoder;
    const std::vector<mc::Frame> frames = decoder.feed(received.data(), received.size());
    EXPECT_EQ(decoder.stats().framesCrcError, std::uint64_t{0});
    EXPECT_EQ(frames.size(), std::size_t{9}) << "10 拍里第一拍只对齐基准，其余 9 拍各一帧";
}

#if defined(__unix__) || defined(__APPLE__)

namespace {

// 一对已连接的双向 fd（用 AF_UNIX 流套接字，等价于"一条已经打开的链路"）。
struct SocketPair {
    int fds[2] = {-1, -1};

    SocketPair() { (void)::socketpair(AF_UNIX, SOCK_STREAM, 0, fds); }

    ~SocketPair() {
        for (int& fd : fds) {
            if (fd >= 0) {
                ::close(fd);
            }
        }
    }

    SocketPair(const SocketPair&) = delete;
    SocketPair& operator=(const SocketPair&) = delete;

    bool valid() const noexcept { return fds[0] >= 0 && fds[1] >= 0; }
};

}  // namespace

// ---------------------------------------------------------------------------
// FdTransport：真 fd 上的传输语义
// ---------------------------------------------------------------------------

TEST(FdTransport, CarriesBytesBothWays) {
    SocketPair sockets;
    ASSERT_TRUE(sockets.valid());

    mc::FdTransport link(sockets.fds[0]);

    const std::vector<std::uint8_t> down = {0xAAU, 0x55U, 0x00U, 0xFFU};
    ASSERT_EQ(::write(sockets.fds[1], down.data(), down.size()),
              static_cast<ssize_t>(down.size()));

    std::uint8_t buffer[16] = {0};
    ASSERT_EQ(link.receive(buffer, sizeof(buffer)), down.size());
    EXPECT_EQ(std::memcmp(buffer, down.data(), down.size()), 0);

    const std::vector<std::uint8_t> up = {1U, 2U, 3U};
    EXPECT_TRUE(link.send(up.data(), up.size()));
    std::uint8_t echoed[16] = {0};
    ASSERT_EQ(::read(sockets.fds[1], echoed, sizeof(echoed)), static_cast<ssize_t>(up.size()));
    EXPECT_EQ(std::memcmp(echoed, up.data(), up.size()), 0);

    EXPECT_EQ(link.counters().bytesReceived, static_cast<std::uint64_t>(down.size()));
    EXPECT_EQ(link.counters().bytesSent, static_cast<std::uint64_t>(up.size()));
    EXPECT_TRUE(link.isOpen());
}

// 关掉之后不许再读写，且关两次不能出问题（析构还会再关一次）。
TEST(FdTransport, CloseIsIdempotentAndStopsIo) {
    SocketPair sockets;
    ASSERT_TRUE(sockets.valid());

    mc::FdTransport link(sockets.fds[0], /*ownsFd=*/true);
    link.close();
    link.close();

    EXPECT_FALSE(link.isOpen());
    std::uint8_t buffer[8] = {0};
    EXPECT_EQ(link.receive(buffer, sizeof(buffer)), std::size_t{0});
    const std::vector<std::uint8_t> bytes = {1U};
    EXPECT_FALSE(link.send(bytes.data(), bytes.size()));
}

// ---------------------------------------------------------------------------
// 伪终端：mc_sim 与设备侧测试用的"真 tty"
// ---------------------------------------------------------------------------

// 一帧从从端写进去，设备（挂在 master 端）必须用 HELLO_ACK 回过来。
// 这就是 mc_sim 里"上位机连上伪终端后能不能说话"的最小闭环。
TEST(FdTransportAndPty, ADeviceBehindTheMasterAnswersAFrameFromTheSlave) {
    std::string slavePath;
    const int master = mc::openPtyPair(&slavePath);
    ASSERT_GE(master, 0) << "内核没给出伪终端，本用例无法进行";
    ASSERT_FALSE(slavePath.empty());

    const int slave = ::open(slavePath.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    ASSERT_GE(slave, 0) << "打不开从端 " << slavePath;

    mc::FdTransport deviceLink(master, /*ownsFd=*/true);
    mc::DeviceSimulator device;
    mc::SimLoop loop(device, deviceLink);

    const std::vector<std::uint8_t> hello = encodeFrameBytes(mc::MessageType::Hello, 7U);
    ASSERT_FALSE(hello.empty());
    ASSERT_EQ(::write(slave, hello.data(), hello.size()), static_cast<ssize_t>(hello.size()));

    std::vector<std::uint8_t> collected;
    std::uint8_t buffer[512];
    for (std::uint64_t i = 1U; i <= 6U; ++i) {
        loop.step(i * 10U);
        for (;;) {
            const ssize_t n = ::read(slave, buffer, sizeof(buffer));
            if (n <= 0) {
                break;
            }
            collected.insert(collected.end(), buffer, buffer + n);
        }
    }
    ::close(slave);

    EXPECT_EQ(deviceLink.counters().bytesReceived, static_cast<std::uint64_t>(hello.size()));
    EXPECT_EQ(device.state(), mc::DeviceState::Ready) << "握手在真 tty 上走通了";

    // 设备还会持续上报遥测，所以这里要**从一堆帧里挑出** HELLO_ACK，而不是假设只有一条。
    mc::StreamDecoder decoder;
    const std::vector<mc::Frame> frames = decoder.feed(collected.data(), collected.size());
    bool sawHelloAck = false;
    for (const mc::Frame& frame : frames) {
        if (frame.type == mc::MessageType::HelloAck && frame.seq == 7U) {
            sawHelloAck = true;
        }
    }
    EXPECT_TRUE(sawHelloAck) << "解出 " << frames.size() << " 帧，其中没有 seq=7 的 HELLO_ACK";
}

// master 端在**还没有从端连上来**时读会拿到 EIO。那必须被当成"暂时没有数据"：
// 把它当成链路断掉，mc_sim 会在上位机连上来之前就退出 —— 表现成"模拟器一启动就没了"。
TEST(FdTransportAndPty, ReadingTheMasterBeforeTheSlaveIsOpenIsNotAnError) {
    std::string slavePath;
    const int master = mc::openPtyPair(&slavePath);
    ASSERT_GE(master, 0);

    mc::FdTransport link(master, /*ownsFd=*/true);

    std::uint8_t buffer[64] = {0};
    EXPECT_EQ(link.receive(buffer, sizeof(buffer)), std::size_t{0});
    EXPECT_TRUE(link.isOpen()) << "「对面还没接上」不能算链路断掉";
    EXPECT_GT(link.counters().readErrors, std::uint64_t{0})
        << "但这件事必须记在账上（否则「读不到」与「没有数据」就再也分不清）";
}

// 二进制载荷必须原样穿过 tty。
//
// 这条不是在测"字节搬运"（上面已经测过）—— 是在测**行规被关掉了**：
// 默认行规会把 0x0A 改写成 0x0D 0x0A（ONLCR）、把 0x0D 吞成 0x0A（ICRNL）。
// 协议帧是二进制的，CRC 里几乎必然出现这两个字节；一旦被改写，
// 现象就是"随机的 CRC 错"，而排查方向会被带偏到协议实现上去 ——
// 这正是 mc_sim 打开伪终端时必须 cfmakeraw 的理由，必须有用例钉住。
TEST(FdTransportAndPty, BinaryPayloadSurvivesTheLineDiscipline) {
    std::string slavePath;
    const int master = mc::openPtyPair(&slavePath);
    ASSERT_GE(master, 0);

    const int slave = ::open(slavePath.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    ASSERT_GE(slave, 0);

    mc::FdTransport deviceLink(master, /*ownsFd=*/true);
    mc::FdTransport peerLink(slave, /*ownsFd=*/true);

    // 最容易被打回的两个字节（CR / LF）各来两次，前后配上帧定界符。
    const std::vector<std::uint8_t> pattern = {0xAAU, 0x55U, 0x0AU, 0x0DU,
                                               0x00U, 0x0DU, 0x0AU, 0xFFU};

    ASSERT_TRUE(deviceLink.send(pattern.data(), pattern.size()));
    std::uint8_t fromDevice[64] = {0};
    const std::size_t gotFromDevice = peerLink.receive(fromDevice, sizeof(fromDevice));
    ASSERT_EQ(gotFromDevice, pattern.size()) << "字节数就变了，说明有人改写了流";
    EXPECT_EQ(std::memcmp(fromDevice, pattern.data(), pattern.size()), 0)
        << "0x0A/0x0D 被行规改写了 —— 二进制协议不能在没设 raw 的 tty 上跑";

    ASSERT_TRUE(peerLink.send(pattern.data(), pattern.size()));
    std::uint8_t fromPeer[64] = {0};
    const std::size_t gotFromPeer = deviceLink.receive(fromPeer, sizeof(fromPeer));
    ASSERT_EQ(gotFromPeer, pattern.size());
    EXPECT_EQ(std::memcmp(fromPeer, pattern.data(), pattern.size()), 0) << "反方向同样必须原样";
}

#endif  // __unix__ || __APPLE__
