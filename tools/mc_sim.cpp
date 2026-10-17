// ---------------------------------------------------------------------------
// mc_sim —— 设备侧模拟器（可执行程序）
//
// 让**真上位机**（Qt 的 mc_host）在没有 STM32 的情况下，通过一条真串口/伪终端
// 与一个协议等价的设备对端通信：同一套帧格式、同一套 CRC、同一套拒绝规则、
// 同一个状态机、同一个运动模型 —— 用的就是 mc_core 里那个 DeviceSimulator。
// 因此"上位机在界面上看到的"与"协议测试里断言的"是同一件事。
//
// 用法：
//   mc_sim --pty                     自己开一对伪终端，打印从端路径，等上位机连上来
//   mc_sim --port /dev/ttyUSB0       在具名串口设备上扮演设备
//   mc_sim --self-test               自检：进程内跑完 上位机 ↔ 伪终端 ↔ 设备，退出码即结论
//
// ⚠ 本程序产出的一切都是**仿真**数据，不是任何实物的测量结果。
//   尤其：伪终端（PTY）不是串口的电气层，波特率/校验位在它上面不起作用，
//   所以"115200 能不能跑满 100 Hz 遥测"这类问题在这里**测不出来**。
//   --port 走真串口的那条路，在作者的环境里没有硬件可验，属于**未验证**代码。
// ---------------------------------------------------------------------------
#include <chrono>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "mc/device_simulator.hpp"
#include "mc/fd_transport.hpp"
#include "mc/frame_codec.hpp"
#include "mc/host_session.hpp"
#include "mc/pty.hpp"
#include "mc/sim_loop.hpp"
#include "mc/version.hpp"

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

namespace {

volatile std::sig_atomic_t gRunning = 1;

void onSignal(int) { gRunning = 0; }

void printUsage() {
    std::printf(
        "mc_sim %s —— 设备侧模拟器（仿真，非实物）\n"
        "\n"
        "  用法:\n"
        "    mc_sim --pty [选项]             开一对伪终端并打印从端路径\n"
        "    mc_sim --port <设备路径> [选项] 在具名串口上扮演设备\n"
        "    mc_sim --self-test              自检：进程内跑完整会话，退出码即结论\n"
        "\n"
        "  选项:\n"
        "    --rate <Hz>         设备 tick 频率（默认 100，即 10 ms 一拍）\n"
        "    --ticks <N>         跑满 N 拍就退出（默认 0 = 一直跑）\n"
        "    --baud <bps>        真串口波特率（默认 115200；PTY 上不起作用）\n"
        "    --kp/--ki/--kd <V>  设备侧整定初值（默认全 0：速度环纯前馈、位置环不动）\n"
        "    --corrupt-every <N> 上行每第 N 个字节取反（造 CRC 错；默认 0 = 不损坏）\n"
        "    --fault <名称|编号> 持续注入故障：none/commloss/overspeed/overlimit/crcburst/forced\n"
        "    --quiet             不打周期摘要\n"
        "    --help              打印本说明\n"
        "\n"
        "  说明: 默认 PID 全 0 是**有含义的**（不是「没配」）：速度环退化成纯前馈\n"
        "        （照常跟随目标），位置环因为没有反馈增益而不会动 —— 真设备上一台\n"
        "        没整定过的位置环也是这样。要跑位置环就显式给 --kp。\n",
        mc::kVersionString);
}

struct Options {
    bool pty = false;
    bool selfTest = false;
    bool help = false;
    bool quiet = false;
    std::string port;
    int rate = 100;
    std::uint64_t ticks = 0U;
    int baud = 115200;
    float kp = 0.0F;
    float ki = 0.0F;
    float kd = 0.0F;
    std::uint32_t corruptEvery = 0U;
    mc::FaultCode fault = mc::FaultCode::None;
};

bool parseFaultCode(const std::string& text, mc::FaultCode* out) {
    if (text == "none" || text == "0") { *out = mc::FaultCode::None; return true; }
    if (text == "commloss" || text == "1") { *out = mc::FaultCode::CommunicationLoss; return true; }
    if (text == "overspeed" || text == "2") { *out = mc::FaultCode::OverSpeed; return true; }
    if (text == "overlimit" || text == "3") { *out = mc::FaultCode::OverLimit; return true; }
    if (text == "crcburst" || text == "4") { *out = mc::FaultCode::CrcBurst; return true; }
    if (text == "forced" || text == "5") { *out = mc::FaultCode::Forced; return true; }
    return false;
}

// 支持 --key=value 与 --key value 两种写法。取到值返回 true。
bool takeValue(int argc, char** argv, int* index, const std::string& arg, const std::string& key,
               std::string* value, std::string* error) {
    const std::string prefix = key + "=";
    if (arg.rfind(prefix, 0) == 0) {
        *value = arg.substr(prefix.size());
        return true;
    }
    if (arg == key) {
        if (*index + 1 >= argc) {
            *error = "选项 " + key + " 后面缺少取值";
            return false;
        }
        *value = argv[++(*index)];
        return true;
    }
    return false;
}

bool parseOptions(int argc, char** argv, Options* opts, std::string* error) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        std::string value;

        if (arg == "--help" || arg == "-h") { opts->help = true; continue; }
        if (arg == "--pty") { opts->pty = true; continue; }
        if (arg == "--self-test") { opts->selfTest = true; continue; }
        if (arg == "--quiet") { opts->quiet = true; continue; }

        if (takeValue(argc, argv, &i, arg, "--port", &value, error)) {
            opts->port = value;
            continue;
        }
        if (takeValue(argc, argv, &i, arg, "--baud", &value, error)) {
            opts->baud = std::atoi(value.c_str());
            continue;
        }
        if (takeValue(argc, argv, &i, arg, "--rate", &value, error)) {
            opts->rate = std::atoi(value.c_str());
            continue;
        }
        if (takeValue(argc, argv, &i, arg, "--ticks", &value, error)) {
            opts->ticks = static_cast<std::uint64_t>(std::strtoull(value.c_str(), nullptr, 10));
            continue;
        }
        if (takeValue(argc, argv, &i, arg, "--kp", &value, error)) {
            opts->kp = std::strtof(value.c_str(), nullptr);
            continue;
        }
        if (takeValue(argc, argv, &i, arg, "--ki", &value, error)) {
            opts->ki = std::strtof(value.c_str(), nullptr);
            continue;
        }
        if (takeValue(argc, argv, &i, arg, "--kd", &value, error)) {
            opts->kd = std::strtof(value.c_str(), nullptr);
            continue;
        }
        if (takeValue(argc, argv, &i, arg, "--corrupt-every", &value, error)) {
            opts->corruptEvery = static_cast<std::uint32_t>(std::atoi(value.c_str()));
            continue;
        }
        if (takeValue(argc, argv, &i, arg, "--fault", &value, error)) {
            if (!parseFaultCode(value, &opts->fault)) {
                *error = "无法识别的故障码：" + value;
                return false;
            }
            continue;
        }

        *error = "无法识别的参数：" + arg;
        return false;
    }

    if (opts->rate <= 0 || opts->rate > 1000) {
        *error = "--rate 必须在 1..1000 之间";
        return false;
    }
    if (opts->pty && !opts->port.empty()) {
        *error = "--pty 与 --port 只能给一个";
        return false;
    }
    return true;
}

mc::SimulatorConfig simulatorConfig(const Options& opts) {
    mc::SimulatorConfig config;
    config.initialPid = mc::SetPid{opts.kp, opts.ki, opts.kd};
    config.forcedFaultCode = opts.fault;
    return config;
}

std::uint32_t periodMsOf(const Options& opts) {
    const std::uint32_t period = static_cast<std::uint32_t>(1000 / opts.rate);
    return (period == 0U) ? 1U : period;
}

// 把 fd 配置成 raw（关掉行规对二进制载荷的改写）+ 指定波特率。
// 真串口必须走这一步：默认行规的 ONLCR / ICRNL 会把 0x0A、0x0D 改掉，
// 表现成"随机的 CRC 错"，排查方向会被带偏。
bool configureRawTty(int fd, int baud, std::string* error) {
    termios tty{};
    if (::tcgetattr(fd, &tty) != 0) {
        *error = std::string("tcgetattr 失败：") + std::strerror(errno);
        return false;
    }
    ::cfmakeraw(&tty);

    struct Baud {
        int value;
        speed_t code;
    };
    static const Baud kBauds[] = {
        {9600, B9600},   {19200, B19200},   {38400, B38400},   {57600, B57600},
        {115200, B115200}, {230400, B230400},
#ifdef B460800
        {460800, B460800},
#endif
#ifdef B921600
        {921600, B921600},
#endif
    };
    speed_t code = 0U;
    for (const Baud& entry : kBauds) {
        if (entry.value == baud) {
            code = entry.code;
        }
    }
    if (code == 0U) {
        *error = "不支持的波特率：" + std::to_string(baud);
        return false;
    }
    if (::cfsetispeed(&tty, code) != 0 || ::cfsetospeed(&tty, code) != 0) {
        *error = std::string("设置波特率失败：") + std::strerror(errno);
        return false;
    }
    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;
    if (::tcsetattr(fd, TCSANOW, &tty) != 0) {
        *error = std::string("tcsetattr 失败：") + std::strerror(errno);
        return false;
    }
    return true;
}

void printModelSummary(const Options& opts, const mc::SimulatorConfig& config,
                       const std::string& where) {
    std::printf("=== mc_sim %s（设备侧模拟器 · 全部为仿真数据，非实物测量）===\n",
                mc::kVersionString);
    std::printf("  链路      : %s\n", where.c_str());
    std::printf("  拍频      : %d Hz（%u ms/拍）\n", opts.rate, periodMsOf(opts));
    std::printf("  整定初值  : kp=%.3f ki=%.3f kd=%.3f（全 0 = 速度环纯前馈 / 位置环不动）\n",
                static_cast<double>(opts.kp), static_cast<double>(opts.ki),
                static_cast<double>(opts.kd));
    std::printf("  上行注入  : %s\n",
                opts.corruptEvery == 0U
                    ? "无"
                    : ("每第 " + std::to_string(opts.corruptEvery) + " 个字节取反（造 CRC 错）")
                          .c_str());
    std::printf("  故障注入  : %s\n", mc::faultCodeName(opts.fault));
    std::printf("  通信丢失保护: %llu ms 无合法帧 → 进入 Fault（F7）\n",
                static_cast<unsigned long long>(config.commLossTimeoutMs));
    std::printf("\n");
}

int runDevice(const Options& opts) {
    mc::SimulatorConfig config = simulatorConfig(opts);

    int fd = -1;
    std::string where;
    if (opts.pty) {
        std::string slavePath;
        fd = mc::openPtyPair(&slavePath);
        if (fd < 0) {
            std::fprintf(stderr, "打不开伪终端：%s\n", std::strerror(errno));
            return 1;
        }
        where = "伪终端（进程 " + std::to_string(::getpid()) + " 的 master 端）";
        printModelSummary(opts, config, where);
        std::printf("把下面这一行填进上位机『系统设置』页的端口名，然后点『应用数据源』：\n");
        std::printf("    %s\n\n", slavePath.c_str());
        std::fflush(stdout);
    } else {
        fd = ::open(opts.port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd < 0) {
            std::fprintf(stderr, "打不开 %s：%s\n", opts.port.c_str(), std::strerror(errno));
            return 1;
        }
        std::string rawError;
        if (!configureRawTty(fd, opts.baud, &rawError)) {
            std::fprintf(stderr, "配置 %s 失败：%s\n", opts.port.c_str(), rawError.c_str());
            ::close(fd);
            return 1;
        }
        where = opts.port + "（" + std::to_string(opts.baud) + " bps，raw）";
        printModelSummary(opts, config, where);
        std::fflush(stdout);
    }

    mc::FdTransport link(fd, /*ownsFd=*/true);
    mc::DeviceSimulator device(config);

    mc::SimLoopConfig loopConfig;
    loopConfig.corruptEveryNthByte = opts.corruptEvery;
    mc::SimLoop loop(device, link, loopConfig);

    const std::uint32_t period = periodMsOf(opts);
    const auto start = std::chrono::steady_clock::now();
    auto next = start;
    std::uint64_t lastReportMs = 0U;

    while (gRunning != 0) {
        const std::uint64_t nowMs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start)
                .count());

        loop.step(nowMs);

        if (!opts.quiet && nowMs >= lastReportMs + 1000U) {
            lastReportMs = nowMs;
            const mc::Telemetry& telemetry = device.lastTelemetry();
            std::printf(
                "[%6llu ms] 状态 %-12s 故障 %-16s 速度 %8.2f deg/s 位置 %10.2f deg "
                "目标 %8.2f | 收 %llu B 发 %llu B(坏 %llu) 遥测 %zu\n",
                static_cast<unsigned long long>(nowMs), mc::deviceStateName(device.state()),
                mc::faultCodeName(device.faultCode()), static_cast<double>(telemetry.velocityDegPerSec),
                static_cast<double>(telemetry.positionDeg), static_cast<double>(telemetry.target),
                static_cast<unsigned long long>(loop.counters().bytesReadFromPeer),
                static_cast<unsigned long long>(loop.counters().bytesWrittenToPeer),
                static_cast<unsigned long long>(loop.counters().bytesCorrupted),
                loop.counters().telemetrySent);
            std::fflush(stdout);
        }

        if (opts.ticks != 0U && loop.counters().rounds >= opts.ticks) {
            break;
        }

        next += std::chrono::milliseconds(period);
        const auto now = std::chrono::steady_clock::now();
        if (next < now) {
            // 落后了就重新对齐，不做"补跑"：补跑会让虚拟时间与真实时间脱节，
            // 而设备侧的 F7 判据正是按真实时间算的。
            next = now + std::chrono::milliseconds(period);
        }
        std::this_thread::sleep_until(next);
    }

    const mc::SimLoop::Counters& counters = loop.counters();
    std::printf("\n=== 结束 ===\n");
    std::printf("  拍数        : %llu\n", static_cast<unsigned long long>(counters.rounds));
    std::printf("  收到字节    : %llu（解出 %zu 帧）\n",
                static_cast<unsigned long long>(counters.bytesReadFromPeer),
                counters.framesReceived);
    std::printf("  发出字节    : %llu（上行注入损坏 %llu 字节，传输拒收 %llu 次）\n",
                static_cast<unsigned long long>(counters.bytesWrittenToPeer),
                static_cast<unsigned long long>(counters.bytesCorrupted),
                static_cast<unsigned long long>(counters.sendFailures));
    std::printf("  应答        : ACK %zu / NAK %zu / 去重重放 %zu\n", counters.acksSent,
                counters.nacksSent, counters.duplicatesSuppressed);
    std::printf("  遥测        : 产出 %zu 帧\n", counters.telemetrySent);
    std::printf("  设备末态    : %s（故障 %s，锁存 %s）\n", mc::deviceStateName(device.state()),
                mc::faultCodeName(device.faultCode()), device.faultLatched() ? "是" : "否");
    std::printf("  传输账目    : 写错 %llu / 读错 %llu（PTY 上「从端未接」就是读错）\n",
                static_cast<unsigned long long>(link.counters().writeErrors),
                static_cast<unsigned long long>(link.counters().readErrors));
    return 0;
}

// ---------------------------------------------------------------------------
// 自检：进程内把 上位机 ↔ 伪终端 ↔ 设备 跑起来。
//
// 为什么值得单独做：mc_sim 的核心价值是"设备端能在一条**真 tty** 上跑完整会话"，
// 而这件事在进程内回环上测不到（那不是 tty），在真串口上又需要硬件。
// 自检把三段真实行为跑一遍，退出码即结论 —— 于是"它能不能用"不需要人工围观：
//   ① 干净链路：握手 → 选环 → 使能 → 给目标 → 停车
//   ② 上行损坏：CRC 错必须被上位机的解码器拒掉，而链路整体仍然可用
//   ③ 通信丢失（F7）：上位机不说话了 → 设备进 Fault 并锁存 → 确认后才回 Ready
// ---------------------------------------------------------------------------
class SelfTest {
public:
    void expect(bool ok, const std::string& what, const std::string& detail = std::string()) {
        ++checks_;
        if (ok) {
            std::printf("  [ 通过 ] %s\n", what.c_str());
            return;
        }
        ++failed_;
        std::printf("  [ 失败 ] %s", what.c_str());
        if (!detail.empty()) {
            std::printf("  —— %s", detail.c_str());
        }
        std::printf("\n");
    }

    int failed() const noexcept { return failed_; }
    int checks() const noexcept { return checks_; }

private:
    int checks_ = 0;
    int failed_ = 0;
};

// 一对伪终端的两端 + 两边的会话对象。
class TtySession {
public:
    TtySession(const mc::SimulatorConfig& simConfig, mc::SimLoopConfig loopConfig)
        : master_(mc::openPtyPair(&slavePath_)),
          deviceLink_(master_, /*ownsFd=*/false),
          hostFd_(openSlave()),
          hostLink_(hostFd_, /*ownsFd=*/false),
          sim_(simConfig),
          loop_(sim_, deviceLink_, loopConfig),
          host_(hostLink_) {
        host_.enableHandshake([this]() { return seq_++; });
    }

    ~TtySession() {
        if (hostFd_ >= 0) {
            ::close(hostFd_);
        }
        if (master_ >= 0) {
            ::close(master_);
        }
    }

    TtySession(const TtySession&) = delete;
    TtySession& operator=(const TtySession&) = delete;

    bool valid() const noexcept { return master_ >= 0 && hostFd_ >= 0; }

    // 让上位机按周期发心跳 —— 真上位机就是这么保活的。
    // 必须开：不开的话，命令发完之后链路就静默了，设备侧的 F7 会**顺手**触发，
    // 于是③里"制造一次通信丢失"就失去了因果（它早就因为静默发生了，
    // 断言会在错误的位置通过 —— 自检的第一版正是这么骗过自己的）。
    void enableHeartbeats(std::uint64_t periodMs, std::uint32_t missedBeats) {
        mc::LinkMonitor::Config config;
        config.heartbeatPeriodMs = periodMs;
        config.missedBeatsToDeclareLinkDown = missedBeats;
        config.initialAckTimeoutMs = 200U;
        config.timeoutMarginPercent = 300U;
        config.minAckTimeoutMs = 50U;
        config.maxAckTimeoutMs = 2000U;
        host_.enableLinkMonitor(config, [this]() { return seq_++; });
    }

    // 一拍：设备侧先跑（读→tick→写），上位机随后 pump（读到设备刚写的，并写回命令）。
    // 两端都是非阻塞 fd，字节在内核缓冲里立刻可见，因此不需要真的睡 10 ms。
    void step() {
        nowMs_ += kTickMs;
        loop_.step(nowMs_);
        accumulate(host_.pump(nowMs_));
    }

    // 只推进设备侧：用来制造"上位机不说话了"（F7 的触发条件）。
    void deviceOnlyRounds(std::size_t rounds) {
        for (std::size_t i = 0U; i < rounds; ++i) {
            nowMs_ += kTickMs;
            loop_.step(nowMs_);
        }
    }

    void rounds(std::size_t n) {
        for (std::size_t i = 0U; i < n; ++i) {
            step();
        }
    }

    void sendSetMode(std::uint8_t mode) { (void)host_.sendSetMode(mode, seq_++, nowMs_); }
    void sendStart() { (void)host_.sendStart(seq_++, nowMs_); }
    void sendSetTarget(float target) { (void)host_.sendSetTarget(target, seq_++, nowMs_); }
    void sendStop() { (void)host_.sendStop(seq_++); }
    void sendClearFault(std::uint8_t code) { (void)host_.sendClearFault(code, seq_++, nowMs_); }
    void sendHome(const mc::HomeParam& param) { (void)host_.sendHome(param, seq_++, nowMs_); }
    void sendSetLimits(const mc::SoftLimits& limits) {
        (void)host_.sendSetLimits(limits, seq_++, nowMs_);
    }
    void sendReadParams() { (void)host_.sendReadParams(seq_++, nowMs_); }

    bool waitForHandshake(std::size_t maxRounds) {
        for (std::size_t i = 0U; i < maxRounds; ++i) {
            if (host_.sessionReady()) {
                return true;
            }
            step();
        }
        return host_.sessionReady();
    }

    bool waitForDeviceState(mc::DeviceState wanted, std::size_t maxRounds) {
        for (std::size_t i = 0U; i < maxRounds; ++i) {
            if (sim_.state() == wanted) {
                return true;
            }
            step();
        }
        return sim_.state() == wanted;
    }

    mc::HostSession& host() noexcept { return host_; }
    mc::DeviceSimulator& device() noexcept { return sim_; }
    const mc::SimLoop::Counters& loopCounters() const noexcept { return loop_.counters(); }
    std::uint64_t nowMs() const noexcept { return nowMs_; }

    std::uint64_t telemetryAccepted() const noexcept { return telemetryAccepted_; }
    std::uint64_t telemetryRejected() const noexcept { return telemetryRejected_; }
    std::uint64_t acksMatched() const noexcept { return acksMatched_; }
    std::uint64_t naksReceived() const noexcept { return naksReceived_; }

    static constexpr std::uint32_t kTickMs = 10U;

private:
    int openSlave() const {
        return ::open(slavePath_.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    }

    void accumulate(const mc::HostSession::PumpResult& result) {
        telemetryAccepted_ += result.telemetryAccepted;
        telemetryRejected_ += result.telemetryRejected;
        acksMatched_ += result.acksMatched;
        naksReceived_ += result.naksReceived;
    }

    std::string slavePath_;
    int master_ = -1;
    mc::FdTransport deviceLink_;
    int hostFd_ = -1;
    mc::FdTransport hostLink_;
    mc::DeviceSimulator sim_;
    mc::SimLoop loop_;
    mc::HostSession host_;
    std::uint16_t seq_ = 0U;
    std::uint64_t nowMs_ = 0U;
    std::uint64_t telemetryAccepted_ = 0U;
    std::uint64_t telemetryRejected_ = 0U;
    std::uint64_t acksMatched_ = 0U;
    std::uint64_t naksReceived_ = 0U;
};

std::string fmt(float value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.3f", static_cast<double>(value));
    return buffer;
}

// ① 干净链路：握手 → 速度环 → 使能 → 给目标 → 停车。
void phaseCleanLink(SelfTest& test) {
    std::printf("\n① 干净链路（伪终端，无注入）\n");

    mc::SimulatorConfig config;
    config.commLossTimeoutMs = 0U;   // 本阶段不测 F7（那在③），别让静默顺手触发故障

    TtySession session(config, mc::SimLoopConfig{});
    if (!session.valid()) {
        test.expect(false, "建立伪终端", "posix_openpt / 打开从端失败");
        return;
    }

    test.expect(session.waitForHandshake(60U), "上位机在真 tty 上完成握手",
                "握手轮数没能收敛");
    test.expect(session.host().handshakeRounds() == 1ULL, "一次 HELLO 往返就握手成功",
                "轮数=" + std::to_string(session.host().handshakeRounds()));

    session.sendSetMode(1U);      // 速度环
    session.sendStart();
    session.sendSetTarget(200.0F);
    session.rounds(150U);         // 1.5 s 虚拟时间

    test.expect(session.device().state() == mc::DeviceState::Running,
                "设备进入 Running（命令真的走到了对端）",
                std::string("实际状态=") + mc::deviceStateName(session.device().state()));
    test.expect(session.acksMatched() >= 3U, "SET_MODE / START / SET_TARGET 的应答都回来了",
                "ACK 对上 " + std::to_string(session.acksMatched()) + " 条");
    test.expect(session.naksReceived() == 0U, "没有被拒的命令",
                "NAK " + std::to_string(session.naksReceived()) + " 条");
    test.expect(session.telemetryAccepted() > 20U, "遥测持续到达上位机",
                "收到 " + std::to_string(session.telemetryAccepted()) + " 条");
    test.expect(session.telemetryRejected() == 0U, "干净链路上没有一帧被拒",
                "被拒 " + std::to_string(session.telemetryRejected()) + " 条");

    // 上报速度是"量化位置在固定窗口上差分"出来的，因此只能落在量化噪声带里：
    // 一个计数 = 360/4096 度，窗口 50 ms → 噪声下限约 1.76 deg/s。
    // 这不是误差超标，而是模型里真实存在的分辨率极限。
    const float velocity = session.host().lastTelemetry().velocityDegPerSec;
    test.expect(std::fabs(velocity - 200.0F) <= 4.0F, "上位机读到的速度收敛到目标 200 deg/s",
                "实测 " + fmt(velocity));

    session.sendStop();
    test.expect(session.waitForDeviceState(mc::DeviceState::Ready, 200U),
                "STOP 之后设备自己收敛回 Ready（停机判稳）",
                std::string("实际状态=") + mc::deviceStateName(session.device().state()));
    // 停止是单向的，没有回执；上位机只能从遥测里看到 target 被撤销。
    test.expect(session.host().lastTelemetry().target == 0.0F,
                "STOP 之后遥测里的目标被撤销成 0",
                "目标=" + fmt(session.host().lastTelemetry().target));
    test.expect(session.host().lastTelemetry().state ==
                    static_cast<std::uint8_t>(mc::DeviceState::Ready),
                "上位机看到的状态与设备一致");
}

// ② 上行损坏：注入的 CRC 错必须被拒，链路整体仍可用。
void phaseCorruption(SelfTest& test) {
    std::printf("\n② 上行损坏（每第 128 个字节取反）\n");

    mc::SimulatorConfig config;
    config.commLossTimeoutMs = 0U;

    mc::SimLoopConfig loopConfig;
    // 每第 N 个字节取反：一帧遥测 30 B，N=128 时约 1/4 的帧被损坏。
    // N 取小会让**握手**也过不去（应答帧全被毁），那就变成"在测重传能不能扛住
    // 全损链路"，而不是"损坏的帧会不会被拒"。这里的 N 是有意选的。
    loopConfig.corruptEveryNthByte = 128U;

    TtySession session(config, loopConfig);
    if (!session.valid()) {
        test.expect(false, "建立伪终端");
        return;
    }

    test.expect(session.waitForHandshake(200U),
                "损坏的上行里握手仍然能完成（重传 + 设备侧重放应答）",
                "握手轮数=" + std::to_string(session.host().handshakeRounds()));

    session.sendSetMode(1U);
    session.sendStart();
    session.sendSetTarget(120.0F);
    session.rounds(150U);

    const mc::StreamDecoder::Stats decoder = session.host().decoder().stats();
    test.expect(session.loopCounters().bytesCorrupted > 0U, "上行确实被注入损坏",
                "损坏 " + std::to_string(session.loopCounters().bytesCorrupted) + " 字节");
    test.expect(decoder.framesCrcError > 0U, "上位机的解码器认出 CRC 错",
                "CRC 错 " + std::to_string(decoder.framesCrcError) + " 帧");
    // 坏帧死在**第一道闸**（CRC）。因此它既不会被接收入库，**也不会**出现在
    // "结构非法被拒"那个计数器里 —— 那是 CRC 之后才轮到的一道闸。
    // 两个数字都要说清：只看"被拒 0 条"会误读成"什么都没被拒"，
    // 而真相是"还没走到那一道就被拦下了"。
    test.expect(session.telemetryRejected() == 0U,
                "坏帧在 CRC 那一关就被拦下，没机会走到结构校验",
                "结构校验拒了 " + std::to_string(session.telemetryRejected()) + " 条");
    // 真正的证据是这两个数的差：设备产出多少帧、上位机入库多少帧。
    test.expect(session.telemetryAccepted() < session.loopCounters().telemetrySent,
                "被损坏的帧确实没进数据通路（入库数少于设备产出数）",
                "入库 " + std::to_string(session.telemetryAccepted()) + " / 产出 " +
                    std::to_string(session.loopCounters().telemetrySent));
    // 正对照：链路只是"变差"，不是"断掉" —— 好帧依然照常入库。
    test.expect(session.telemetryAccepted() > 20U, "没被损坏的遥测照常到达",
                "收到 " + std::to_string(session.telemetryAccepted()) + " 条");
    test.expect(session.device().state() == mc::DeviceState::Running,
                "设备侧不受上位机侧解码失败的影响（它是发送方）");
}

// ③ 通信丢失（F7）：上位机不说话 → 设备必须自己停下并锁存故障，
//    确认之后才回 Ready。
void phaseCommLoss(SelfTest& test) {
    std::printf("\n③ 通信丢失（F7）：上位机中途不说话了\n");

    mc::SimulatorConfig config;
    // 把窗口压到 300 ms：自检用的是合成时钟与心跳周期，跑 3 s 的真实窗口只是多花时间，
    // 而"超时→故障"这条逻辑与窗口长度无关（F7 在协议测试里已按默认值验证过）。
    config.commLossTimeoutMs = 300U;

    TtySession session(config, mc::SimLoopConfig{});
    if (!session.valid()) {
        test.expect(false, "建立伪终端");
        return;
    }
    // 上位机每 100 ms 发一次心跳（远小于 300 ms 的窗口），链路平时是"活的"；
    // 对"断链"的容忍设为 5 拍，这样短暂的静默不会让上位机自己也判链路失效 ——
    // 本阶段要单独观察的是**设备侧**的判断。
    session.enableHeartbeats(100U, 5U);

    test.expect(session.waitForHandshake(60U), "握手完成");
    session.sendSetMode(1U);
    session.sendStart();
    session.sendSetTarget(150.0F);
    session.rounds(100U);   // 先跑起来

    test.expect(session.device().state() == mc::DeviceState::Running,
                "平时有心跳保活时设备停在 Running",
                std::string("实际状态=") + mc::deviceStateName(session.device().state()));
    const float velocityBeforeOutage = session.device().trueVelocityDegPerSec();

    // 上位机"不说话了"：只推进设备侧 40 拍（400 ms > 300 ms 窗口）。
    session.deviceOnlyRounds(40U);

    test.expect(session.device().faultCode() == mc::FaultCode::CommunicationLoss,
                "设备自己判定通信丢失（F7 的判据是「超时没收到合法帧」）",
                std::string("故障码=") + mc::faultCodeName(session.device().faultCode()));
    test.expect(session.device().faultLatched(), "故障被锁存（不会自己好）");
    test.expect(std::fabs(session.device().trueVelocityDegPerSec()) <
                    std::fabs(velocityBeforeOutage),
                "失去上位机之后电机在减速，而不是继续追那个陈旧目标",
                "停话前 " + fmt(velocityBeforeOutage) + " → 现在 " +
                    fmt(session.device().trueVelocityDegPerSec()));

    // 上位机重新开始说话：它应当从遥测里看到 Fault，而不是毫不知情。
    session.rounds(20U);
    test.expect(session.host().lastTelemetry().state ==
                    static_cast<std::uint8_t>(mc::DeviceState::Fault),
                "上位机看到的状态是 Fault",
                "实际=" + std::to_string(session.host().lastTelemetry().state));
    test.expect(session.host().lastTelemetry().faultCode ==
                    static_cast<std::uint8_t>(mc::FaultCode::CommunicationLoss),
                "上位机看到的故障码是 CommunicationLoss");

    // 确认清除：只有显式确认才回 Ready。
    const std::uint64_t acksBefore = session.acksMatched();
    session.sendClearFault(static_cast<std::uint8_t>(mc::FaultCode::CommunicationLoss));
    session.rounds(20U);

    test.expect(session.acksMatched() > acksBefore, "CLEAR_FAULT 被应答");
    test.expect(session.device().state() == mc::DeviceState::Ready,
                "确认之后设备回到 Ready",
                std::string("实际状态=") + mc::deviceStateName(session.device().state()));
    test.expect(!session.device().faultLatched(), "故障锁存已被清除");
    test.expect(session.host().lastTelemetry().faultCode == 0U, "上位机侧看到的故障码归零");
}

// ④ 回零与软限位（P1-6）。
//
// 这一阶段要证的是**分层**：HOME 的应答只说"开始了"，"零点建立没有"只能读设备
// 持续报的那一位（遥测状态位组）。所以下面两条路线各走一遍：
//   · 走到底 → 位为 1、坐标被搬到 0；
//   · 半路 STOP → 位仍为 0（**这一格是这一位存在的全部理由**：状态在两拍之间
//     就从 Stopping 回到了 Ready，只看状态判不出"这一趟没走完"）。
void phaseHomingAndLimits(SelfTest& test) {
    std::printf("\n④ 回零与软限位\n");

    mc::SimulatorConfig config;
    config.commLossTimeoutMs = 0U;   // 本阶段不测 F7（那在③）

    TtySession session(config, mc::SimLoopConfig{});
    if (!session.valid()) {
        test.expect(false, "建立伪终端");
        return;
    }
    test.expect(session.waitForHandshake(60U), "握手完成");

    // --- 先让轴离开零点：原点就在脚下的回零什么都测不出来 -------------------
    session.sendSetMode(1U);
    session.sendStart();
    session.sendSetTarget(200.0F);
    session.rounds(60U);   // 0.6 s
    session.sendStop();
    test.expect(session.waitForDeviceState(mc::DeviceState::Ready, 200U),
                "把轴开走之后停稳（回零只在 Ready 放行）");
    const float awayFromZero = session.device().positionDeg();
    test.expect(std::fabs(awayFromZero) > 5.0F, "前提：轴确实离开了零点",
                "位置=" + fmt(awayFromZero) + " deg");
    test.expect(!session.device().homingAttained(),
                "出厂状态：本轮回零还没有建立零点这一位是 0");

    // --- 走到底的回零 ---------------------------------------------------------
    session.sendHome(mc::HomeParam{60.0F, 5000U});
    session.rounds(3U);
    test.expect(session.device().state() == mc::DeviceState::Home,
                "HOME 之后设备进入回零态（应答的语义是「开始了」）",
                std::string("实际状态=") + mc::deviceStateName(session.device().state()));
    test.expect(session.waitForDeviceState(mc::DeviceState::Ready, 600U),
                "回零在时限内走完并回到 Ready");
    test.expect(session.device().homingAttained(), "设备报「本轮回零已建立零点」");
    test.expect(std::fabs(session.device().positionDeg()) <= 1.0F,
                "零点建立在这里：坐标被搬到了 0",
                "位置=" + fmt(session.device().positionDeg()) + " deg");
    // 上位机拿到的只有帧：这一位必须真的从链路上过来。
    test.expect((session.host().lastTelemetry().statusFlags & mc::kTelemetryFlagHomingAttained) != 0U,
                "上位机从遥测里读到了这一位（不是自己猜的）");

    // --- 半路被 STOP 的回零：位必须还是 0 -------------------------------------
    session.sendSetMode(1U);
    session.sendStart();
    session.sendSetTarget(200.0F);
    session.rounds(60U);
    session.sendStop();
    test.expect(session.waitForDeviceState(mc::DeviceState::Ready, 200U), "再开出去一段并停稳");

    session.sendHome(mc::HomeParam{5.0F, 30000U});   // 慢速：留出打断的窗口
    test.expect(session.waitForDeviceState(mc::DeviceState::Home, 60U), "第二趟回零开始");
    test.expect(!session.device().homingAttained(), "新一轮开始就把这一位清掉了（它说的是「这一轮」）");

    session.sendStop();
    test.expect(session.waitForDeviceState(mc::DeviceState::Ready, 200U),
                "被打断的回零仍然会回到 Ready —— 所以**只看状态判不出**它没走完");
    test.expect(!session.device().homingAttained(),
                "被打断的这一趟没有被读成「零点已建立」");
    test.expect((session.host().lastTelemetry().statusFlags & mc::kTelemetryFlagHomingAttained) == 0U,
                "上位机这一侧读到的也是 0");

    // --- 软限位：越界就撤输出并进故障 ------------------------------------------
    //
    // 限位要**围绕现在的位置**给：设备会明确拒掉"把自己关在门外"的一组限位
    // （见 DeviceSimulator 的 SET_LIMITS 分支）。那条规则正是为了让"设完下一拍
    // 就报超限"这种像是设备自己出问题的现象根本不发生 —— 所以这里顺着它写。
    const float here = session.device().positionDeg();
    const mc::SoftLimits limits{true, here - 30.0F, here + 30.0F};
    session.sendSetLimits(limits);
    session.rounds(5U);
    test.expect(session.device().softLimits().enabled &&
                    session.device().softLimits().maxDeg == limits.maxDeg,
                "SET_LIMITS 在设备侧落地了（它才是执法的那一端）",
                "设备存的最大值=" + fmt(session.device().softLimits().maxDeg));
    test.expect(session.device().state() == mc::DeviceState::Ready,
                "设限位本身不该把设备打进故障",
                std::string("实际状态=") + mc::deviceStateName(session.device().state()));

    session.sendSetMode(1U);
    session.sendStart();
    session.sendSetTarget(200.0F);
    test.expect(session.waitForDeviceState(mc::DeviceState::Fault, 200U),
                "轴越过软限位之后进故障（撤输出，不是继续朝界外走）");
    test.expect(session.device().faultCode() == mc::FaultCode::OverLimit,
                "故障码是 OverLimit",
                std::string("实际=") + mc::faultCodeName(session.device().faultCode()));
    test.expect(session.host().lastTelemetry().faultCode ==
                    static_cast<std::uint8_t>(mc::FaultCode::OverLimit),
                "上位机从遥测里看到了这条故障码");

    // --- 回读设备实存参数（READ_PARAMS）----------------------------------------
    const std::uint64_t paramStatesBefore = session.host().paramStatesReceived();
    session.sendReadParams();
    session.rounds(5U);
    test.expect(session.host().paramStatesReceived() > paramStatesBefore,
                "参数回读拿到了设备回的一份（v2 起带软限位）");
    test.expect(session.host().lastParamState().limits.enabled &&
                    session.host().lastParamState().limits.minDeg == limits.minDeg,
                "回读回来的软限位与下发的一致（「我设了什么」与「它存了什么」对得上）",
                "回读最小值=" + fmt(session.host().lastParamState().limits.minDeg));
}

int runSelfTest() {
    std::printf("=== mc_sim %s 自检 ===\n", mc::kVersionString);
    std::printf("链路：一对伪终端（真 tty，非电气层）；时钟：合成 10 ms/拍，不真睡。\n");
    std::printf("⚠ 结论只覆盖软件行为，不覆盖任何硬件或波特率相关的性能。\n");

    SelfTest test;
    phaseCleanLink(test);
    phaseCorruption(test);
    phaseCommLoss(test);
    phaseHomingAndLimits(test);

    std::printf("\n=== 自检结果：%d 项检查，%d 项失败 ===\n", test.checks(), test.failed());
    if (test.failed() == 0) {
        std::printf("结论：设备侧模拟器在真实 tty 上的会话、注入、F7、回零与限位行为均符合设计。\n");
        return 0;
    }
    std::printf("结论：自检**未通过** —— 上面每一行 [ 失败 ] 都是一条可复现的证据。\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    Options opts;
    std::string error;
    if (!parseOptions(argc, argv, &opts, &error)) {
        std::fprintf(stderr, "%s\n\n", error.c_str());
        printUsage();
        return 1;
    }
    if (opts.help) {
        printUsage();
        return 0;
    }
    if (opts.selfTest) {
        return runSelfTest();
    }
    if (!opts.pty && opts.port.empty()) {
        std::fprintf(stderr, "必须给 --pty 或 --port（或 --self-test）\n\n");
        printUsage();
        return 1;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    return runDevice(opts);
}
