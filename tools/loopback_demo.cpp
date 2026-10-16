// ---------------------------------------------------------------------------
// 端到端回环演示
//
// 把「上位机会话」与「最小虚拟设备」用进程内回环链路连起来跑一段虚拟时间，
// 打印每种链路条件下的实测摘要。
//
// 全部数据都是**仿真**出来的：没有 STM32，没有电机，没有真实串口。
// 本程序证明的是"协议在链路上真的按设计工作"，不是任何硬件指标。
// ---------------------------------------------------------------------------
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "mc/device_simulator.hpp"
#include "mc/host_session.hpp"
#include "mc/loopback_transport.hpp"

namespace {

struct Scenario {
    std::string name;
    mc::LinkFaultConfig hostToDevice;
    mc::LinkFaultConfig deviceToHost;

    // 发命令**之前**先跑多少轮。给心跳留出把 RTT 量出来、把超时窗口撑开的时间 ——
    // 没有这段预热，监控器还没摸清链路，命令就会撞在初始窗口上。
    std::size_t warmupRounds = 0U;
    std::size_t rounds = 120U;

    bool enableLinkMonitor = false;
};

struct Summary {
    std::string name;
    std::size_t acksMatched = 0;
    std::size_t naksReceived = 0;
    std::size_t telemetryAccepted = 0;
    std::size_t telemetryRejected = 0;
    std::size_t telemetrySentByDevice = 0;
    mc::StreamDecoder::Stats decoder{};
    std::size_t timeouts = 0;
    std::size_t timeoutsFaulted = 0;
    std::size_t timeoutsRetried = 0;
    std::size_t timeoutsDropped = 0;   // 心跳漏拍：不是故障
    std::uint64_t framesSent = 0;
    std::uint64_t heartbeatsSent = 0;
    std::uint64_t bytesSentByDevice = 0;
    std::uint64_t bytesReadByHost = 0;
    std::uint64_t ackTimeoutMs = 0;
    bool monitorEnabled = false;
    bool linkDown = false;
    float finalVelocity = 0.0F;
    float finalPosition = 0.0F;
    bool enabled = false;
    mc::DeviceState finalState = mc::DeviceState::Disconnected;
    mc::FaultCode finalFault = mc::FaultCode::None;
};

Summary runScenario(const Scenario& scenario) {
    mc::LoopbackPair pair = mc::makeLoopbackPair();

    // 序列号计数器必须声明在 host **之前**：心跳的 seq 来源要引用它，
    // 而心跳与命令必须共用同一个计数器（AckTracker 只按 seq 判重、不看类型，
    // 两套计数器必然撞车，撞上之后命令会静默发不出去）。
    std::uint16_t seq = 0U;
    std::uint64_t now = 0U;

    mc::HostSession host(*pair.host);
    mc::DeviceSimulator sim;

    if (scenario.enableLinkMonitor) {
        mc::LinkMonitor::Config config;
        config.heartbeatPeriodMs = 500U;
        config.missedBeatsToDeclareLinkDown = 4U;
        config.initialAckTimeoutMs = 200U;
        config.timeoutMarginPercent = 300U;
        host.enableLinkMonitor(config, [&seq]() { return seq++; });
    }

    // 会话生命周期（握手与断线重连）交给 HostSession 自己驱动 ——
    // 演示程序不再手工发 HELLO，跑的就是界面将来会跑的那条路径。
    host.enableHandshake([&seq]() { return seq++; });

    Summary summary;
    summary.name = scenario.name;

    const auto stepOnce = [&]() {
        now += 10U;

        const mc::HostSession::PumpResult hostSide = host.pump(now);
        summary.acksMatched += hostSide.acksMatched;
        summary.naksReceived += hostSide.naksReceived;
        summary.telemetryAccepted += hostSide.telemetryAccepted;
        summary.telemetryRejected += hostSide.telemetryRejected;
        summary.ackTimeoutMs = hostSide.ackTimeoutMs;
        summary.linkDown = hostSide.linkDown;
        for (const mc::TimeoutEvent& event : hostSide.timeouts) {
            ++summary.timeouts;
            if (event.outcome == mc::TxOutcome::Fault) {
                ++summary.timeoutsFaulted;
            } else if (event.outcome == mc::TxOutcome::Retry) {
                ++summary.timeoutsRetried;
            } else {
                ++summary.timeoutsDropped;
            }
        }

        std::uint8_t toDevice[512];
        const std::size_t down = pair.device->receive(toDevice, sizeof(toDevice));
        const mc::DeviceSimulator::TickResult deviceSide = sim.tick(toDevice, down, now);
        summary.telemetrySentByDevice += deviceSide.telemetrySent;

        std::uint8_t fromDevice[512];
        const std::size_t up = sim.readOutgoing(fromDevice, sizeof(fromDevice));
        if (up > 0U) {
            pair.device->send(fromDevice, up);
        }
    };

    // 握手必须在**链路还干净**时完成。若故障注入从第一拍就生效，设备会停在
    // Initializing（HELLO 根本没到），后面所有控制命令都被状态门禁拒掉 ——
    // 那样这个场景就变成"在测状态门禁"，而不是"在测这种链路条件"。
    for (int i = 0; i < 30 && !host.sessionReady(); ++i) {
        stepOnce();
    }
    if (!host.sessionReady()) {
        std::printf("  %s\n    ⚠ 握手未能完成（%llu 轮）—— 该场景无意义\n",
                    scenario.name.c_str(),
                    static_cast<unsigned long long>(host.handshakeRounds()));
    }

    // 排空握手留下的在途遥测：不清掉的话，它会被算进第一个场景的"上位机接收"里，
    // 每个场景的接收数就都凭空多一条。
    for (int drain = 0; drain < 3; ++drain) {
        (void)host.pump(now);
    }

    // 握手自己产生的那 1 个 ACK 与那 1 帧 HELLO 都不属于任何链路条件，从统计里剔除；
    // 否则每个场景都凭空多一份，跟上一轮的数字没法对照。
    summary = Summary{};
    summary.name = scenario.name;
    const std::uint64_t framesSentAtScenarioStart = host.framesSent();

    pair.host->outgoingFaults() = scenario.hostToDevice;
    pair.device->outgoingFaults() = scenario.deviceToHost;

    for (std::size_t round = 0U; round < scenario.warmupRounds; ++round) {
        stepOnce();
    }

    // 上位机在这里刻意**不做本地参数校验** —— 参数合法性由设备裁决，
    // 这正是"不要相信上位机"的体现。
    host.sendSetMode(1U, seq++, now);
    host.sendStart(seq++, now);
    host.sendSetPid(mc::SetPid{1.6F, 0.4F, 0.02F}, seq++, now);
    host.sendSetTarget(300.0F, seq++, now);

    for (std::size_t round = 0U; round < scenario.rounds; ++round) {
        stepOnce();
    }

    summary.decoder = host.decoder().stats();
    summary.framesSent = host.framesSent() - framesSentAtScenarioStart;
    summary.bytesSentByDevice = pair.device->outgoingStats().bytesSent;
    summary.bytesReadByHost = pair.host->incomingStats().bytesReceived;
    summary.finalVelocity = sim.velocityDegPerSec();
    summary.finalPosition = sim.positionDeg();
    summary.enabled = sim.isEnabled();
    summary.finalState = sim.state();
    summary.finalFault = sim.faultCode();
    summary.monitorEnabled = host.linkMonitorEnabled();

    std::printf("  %s\n", scenario.name.c_str());
    std::printf("    上位机发送 %llu 帧 | ACK 对上 %zu | NAK 收到 %zu\n",
                static_cast<unsigned long long>(summary.framesSent),
                summary.acksMatched, summary.naksReceived);
    std::printf("    设备发出遥测 %zu 条 → 上位机接收 %zu 条 / 拒绝 %zu 条\n",
                summary.telemetrySentByDevice, summary.telemetryAccepted, summary.telemetryRejected);
    std::printf("    帧解码 %llu | 拒绝：CRC %llu / 长度 %llu / 版本 %llu / 重同步丢字节 %llu\n",
                static_cast<unsigned long long>(summary.decoder.framesDecoded),
                static_cast<unsigned long long>(summary.decoder.framesCrcError),
                static_cast<unsigned long long>(summary.decoder.framesBadLength),
                static_cast<unsigned long long>(summary.decoder.framesBadVersion),
                static_cast<unsigned long long>(summary.decoder.bytesDiscarded));
    // 循环结束时链路里天然还有在途数据（最后一帧还没被读走），因此这里报"在途"而不是判"不一致"。
    std::printf("    字节：设备发出 %llu / 上位机读走 %llu / 在途 %llu\n",
                static_cast<unsigned long long>(summary.bytesSentByDevice),
                static_cast<unsigned long long>(summary.bytesReadByHost),
                static_cast<unsigned long long>(summary.bytesSentByDevice - summary.bytesReadByHost));
    std::printf("    超时 %zu（判 Fault %zu / 可重传 %zu / 心跳漏拍 %zu）"
                "| 末端速度 %.1f deg/s | 末端位置 %.1f deg | 使能 %s | 末端状态 %s（故障码 %s）\n",
                summary.timeouts, summary.timeoutsFaulted, summary.timeoutsRetried,
                summary.timeoutsDropped,
                static_cast<double>(summary.finalVelocity),
                static_cast<double>(summary.finalPosition),
                summary.enabled ? "是" : "否",
                mc::deviceStateName(summary.finalState),
                mc::faultCodeName(summary.finalFault));

    if (scenario.enableLinkMonitor) {
        const mc::LinkMonitor* monitor = host.linkMonitor();
        const std::optional<std::uint64_t> rtt = monitor->rttP95Ms();
        char rttText[32];
        if (rtt.has_value()) {
            std::snprintf(rttText, sizeof(rttText), "%llu ms",
                          static_cast<unsigned long long>(*rtt));
        } else {
            std::snprintf(rttText, sizeof(rttText), "无样本");
        }
        summary.heartbeatsSent = monitor->stats().heartbeatsSent;
        std::printf("    链路监控：心跳 %llu 拍 / 已应答 %llu | 实测 RTT(P95) %s"
                    "| 生效超时窗口 %llu ms | 链路 %s\n",
                    static_cast<unsigned long long>(monitor->stats().heartbeatsSent),
                    static_cast<unsigned long long>(monitor->stats().heartbeatsAcked),
                    rttText,
                    static_cast<unsigned long long>(summary.ackTimeoutMs),
                    summary.linkDown ? "判失效" : "正常");
    }
    return summary;
}

}  // namespace

int main() {
    std::printf("=== 端到端回环演示 ===\n");
    std::printf("注意：以下全部为**仿真**数据（进程内回环，无 STM32、无电机、无真实串口）。\n\n");

    std::vector<Scenario> scenarios;

    scenarios.push_back(Scenario{"① 健康链路", {}, {}, 0U, 120U, false});

    mc::LinkFaultConfig corruptEveryOther;   // 设备→上位机 每第 2 字节取反
    corruptEveryOther.corruptEveryNthByte = 2U;
    scenarios.push_back(Scenario{"② 上报链路损坏", {}, corruptEveryOther, 0U, 120U, false});

    mc::LinkFaultConfig dropAll;             // 上位机→设备 全丢
    dropAll.dropEveryNthByte = 1U;
    scenarios.push_back(Scenario{"③ 下行链路全丢", dropAll, {}, 0U, 40U, false});

    // 延迟以"接收端 poll 次数"注入。**poll 数不等于毫秒数**：上位机一次 pump 会
    // poll 1–2 次（读一遍数据 + 再读一次确认空），所以 20 拍大约落在 100 ms 量级。
    // 提示词里的毫秒只是量级估计；⑥ 里的"实测 RTT"才是真实往返，不要拿拍数换算。
    mc::LinkFaultConfig latencySmall;        // 落在默认 200 ms 窗口之内
    latencySmall.latencyPolls = 10U;
    scenarios.push_back(Scenario{"④ 上行延迟注入 10 拍", {}, latencySmall, 0U, 120U, false});

    // ⑤ 与 ⑥ 是**同一组链路条件**，唯一差别是有没有开链路监控 —— 对比才成立。
    mc::LinkFaultConfig latencyBig;          // 明显超过默认 200 ms 窗口
    latencyBig.latencyPolls = 80U;
    scenarios.push_back(
        Scenario{"⑤ 上行延迟注入 80 拍 · 未开链路监控", {}, latencyBig, 120U, 80U, false});
    scenarios.push_back(
        Scenario{"⑥ 上行延迟注入 80 拍 · 已开链路监控", {}, latencyBig, 120U, 80U, true});

    for (const Scenario& scenario : scenarios) {
        runScenario(scenario);
        std::printf("\n");
    }

    std::printf("=== 观察到的行为 ===\n");
    std::printf("⓪ 每个场景开局都是 HostSession **自己**完成的握手（演示程序不再手工发 HELLO）：\n");
    std::printf("   会话生命周期与链路监控一样是**可选**能力，不启用时 pump() 行为与之前完全一致。\n");
    std::printf("① 健康链路：命令全部应答，遥测持续到达。\n");
    std::printf("② 上行损坏：帧在解码器就被拒（见上面分项统计落在哪一道关卡），\n");
    std::printf("   遥测『接收 0 条 / 拒绝 0 条』—— 坏数据从未进入数据通路，不是被应用层丢掉的；\n");
    std::printf("   命令的**应答**同样被损坏 → 幂等命令（SET_MODE / SET_PID）重发到用尽后判 Fault，\n");
    std::printf("   而这几次同 seq 的重传，被设备侧的去重窗口变成了『重放应答』——没有重复执行。\n");
    std::printf("③ 下行全丢：运动指令超时判 Fault 且**绝不重发**。\n");
    std::printf("   这里 8 帧而不是 4 帧，多出来的 4 帧**全是幂等命令的重发**（SET_MODE / SET_PID 各多 2 帧）；\n");
    std::printf("   START / SET_TARGET 各只发过 1 帧 —— 这就是 ADR-003 与 R5 在线上看得见的分界。\n");
    std::printf("④ 上行高延迟：延迟在超时窗口之内，链路仍然可用，只是数据来得晚。\n");
    std::printf("⑤ 延迟超过硬编码的 200 ms 窗口：运动指令被**误判**成超时 → Fault。\n");
    std::printf("   注意这是误判 —— 应答其实正在路上，只是比窗口慢。\n");
    std::printf("   （延迟以接收端 poll 次数注入，拍数不等于毫秒；真实往返看 ⑥ 的实测 RTT。）\n");
    std::printf("⑥ 同一组链路条件，开启链路监控之后：用心跳实测 RTT，把窗口按 3×RTT 撑开，\n");
    std::printf("   同样的延迟不再被判超时。⑤ 与 ⑥ 的『判 Fault』一栏就是这件事的证据。\n");
    std::printf("   ⚠ 仍然强调：这些是仿真数据。真实串口的 RTT 必须实测，不得照抄这里的数字。\n");
    return 0;
}
