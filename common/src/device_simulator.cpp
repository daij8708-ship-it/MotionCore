#include "mc/device_simulator.hpp"

#include <algorithm>
#include <cmath>

namespace mc {
namespace {

// 单次积分的最大步长。长时间没有 tick 之后（例如界面卡了一下），
// 不能让一次 dt 把速度一次性推到目标 —— 那会掩盖控制器的问题。
constexpr double kMaxIntegrationStepSeconds = 0.5;

}  // namespace

DeviceSimulator::DeviceSimulator(SimulatorConfig config) : config_(config) {
    // 设备侧的整定初值来自配置。默认全 0 是有含义的（见 SimulatorConfig）。
    pid_ = config_.initialPid;
    // 软限位同理：默认未启用，启用与否由配置显式决定（见 SimulatorConfig）。
    softLimits_ = config_.softLimits;
}

TransitionResult DeviceSimulator::applyEvent(DeviceEvent event) noexcept {
    const TransitionResult result = stateMachine_.apply(event);
    if (!result.accepted) {
        return result;
    }

    if (result.stateChanged && stateMachine_.is(DeviceState::Fault)) {
        // 进 Fault 就撤销目标：故障期间绝不能让电机继续追一个陈旧的目标值。
        // 速度仍按 accelDegPerSec2 限幅收敛到零 —— 那是"可控停车"，
        // 比把 PWM 一刀切断更安全（后者会让机械结构承受一次冲击）。
        target_ = 0.0F;
        if (faultCode_ == FaultCode::None) {
            faultCode_ = (config_.forcedFaultCode != FaultCode::None) ? config_.forcedFaultCode
                                                                     : FaultCode::Forced;
        }
    }

    if (result.stateChanged && stateMachine_.is(DeviceState::Ready)) {
        // 能回到 Ready 只有两条路：Initializing 握手成功、或走完 Recovery。
        // 两条路都不该再挂着故障码。
        faultCode_ = FaultCode::None;
    }

    return result;
}

DeviceSimulator::TickResult DeviceSimulator::tick(const std::uint8_t* incoming, std::size_t len,
                                                  std::uint64_t nowMs) {
    TickResult result;

    integrate(nowMs);

    if (incoming != nullptr && len > 0U) {
        const std::vector<Frame> frames = decoder_.feed(incoming, len);
        result.framesReceived = frames.size();
        if (!frames.empty()) {
            // 只要有一帧过了 CRC，就算"通信还活着" —— 这正是 F7 的判据。
            // 用"收到帧"而不是"收到字节"：线上一直有噪声但一帧都过不了 CRC，
            // 那不叫通信正常。
            hasReceivedFrame_ = true;
            lastValidFrameMs_ = nowMs;
        }
        for (const Frame& frame : frames) {
            handleFrame(frame, result);
        }
    }

    runSafetyChecks(nowMs, result);

    emitDueTelemetry(nowMs, result);

    return result;
}

void DeviceSimulator::emitDueTelemetry(std::uint64_t nowMs, TickResult& result) {
    const std::uint64_t period = config_.telemetryPeriodMs;
    if (period == 0U) {
        // 周期 0 必须显式分开写，不能让它落进下面的补发公式：那里的除法会
        // **整数除零**（变异 M33 实测：去掉这个分支，用例当场 SIGFPE 崩溃）。
        // 语义定为"来取就发一帧"。
        lastTelemetryMs_ = nowMs;
        emitTelemetry(result);
        return;
    }

    const std::uint64_t due = (nowMs - lastTelemetryMs_) / period;
    if (due == 0U) {
        return;
    }

    // 基准按**整数个周期**前进，而不是直接跳到 nowMs。
    // 跳到 nowMs 会把"不足一个周期的那点零头"吃掉，长期看就是帧数偏少；
    // 按周期前进则零头一直留着，累计误差恒小于一个周期。
    lastTelemetryMs_ += due * period;

    if (due > static_cast<std::uint64_t>(kMaxTelemetryFramesPerTick)) {
        const std::uint64_t dropped = due - static_cast<std::uint64_t>(kMaxTelemetryFramesPerTick);
        telemetryBacklogDropped_ += dropped;
        result.telemetryBacklogDropped = static_cast<std::size_t>(dropped);
    }

    const std::uint64_t toSend = std::min(due, static_cast<std::uint64_t>(kMaxTelemetryFramesPerTick));
    // 补发的这几帧内容相同：运动模型一拍只积分一次，帧之间没有新的采样点。
    // 它们的作用是**帧率压力**（解码、缓冲、绘制的负载），不代表真实设备那种
    // 每个采样点都不同的遥测密度 —— 引用这些数字时必须带上这句。
    for (std::uint64_t i = 0U; i < toSend; ++i) {
        emitTelemetry(result);
    }
}

void DeviceSimulator::integrate(std::uint64_t nowMs) {
    if (!hasTicked_) {
        hasTicked_ = true;
        lastTickMs_ = nowMs;
        lastTelemetryMs_ = nowMs;
        positionWindow_.push_back(PositionSample{nowMs, position_});
        return;
    }
    if (nowMs <= lastTickMs_) {
        // 时钟回退（换了时间源 / 测试直接给了更小的值）：不积分，只对齐基准。
        // 遥测基准也必须一起回退 —— 否则 nowMs - lastTelemetryMs_ 在无符号数上
        // 会变成一个巨大的值，接下来每一拍都发一帧遥测。
        lastTickMs_ = nowMs;
        if (lastTelemetryMs_ > nowMs) {
            lastTelemetryMs_ = nowMs;
        }
        return;
    }

    const double rawDt = static_cast<double>(nowMs - lastTickMs_) / 1000.0;
    lastTickMs_ = nowMs;
    const double dt = std::min(rawDt, kMaxIntegrationStepSeconds);

    // --- 控制律（阶段 8 起是真闭环，不再是"直接照目标给速度"）------------------
    //
    // 反馈量一律取自**量化后的位置/速度** —— 设备看不到未量化的真值，
    // 用真值做反馈等于让仿真里的控制器作弊，整定出来的现象没有参考价值。
    const bool homing = stateMachine_.is(DeviceState::Home);
    const bool driving =
        (stateMachine_.is(DeviceState::Running) && runMode_ != SimRunMode::Stopped) || homing;
    float command = 0.0F;
    if (homing) {
        // 回零**不走 PID**，也不吃操作员整定的那组增益：真设备的回零是
        // "以固定低速朝零点走"，快慢由回零参数定，与工艺环路的整定无关。
        // 拿 PID 去回零还会有一个恶果：整定得越猛，撞上零位越狠。
        //
        // 方向由**零点在当前位置的哪一侧**决定，判据是设备看得见的编码器读数
        // （真设备上是行程开关/Z 相，这里用零位代替）。
        const float error = 0.0F - measuredPosition_;
        if (std::fabs(error) <= config_.homeToleranceDeg) {
            command = 0.0F;
        } else {
            // 速度与误差成正比（时间常数 homeApproachSeconds），并受报文给的上限约束。
            const float proportional = error / std::max(1.0e-3F, config_.homeApproachSeconds);
            command = std::max(-homeSpeedLimit_, std::min(homeSpeedLimit_, proportional));
        }
        // 积分/微分状态在回零期间保持干净，出了回零不会带着一段无来源的记忆。
        resetPidState();
    } else if (driving) {
        if (runMode_ == SimRunMode::Velocity) {
            // 速度环：前馈 + 反馈修正。增益全 0 时退化成纯前馈（u = target），
            // 与"最小模型"的行为一致 —— 这是有意的可比性，不是巧合。
            const float error = target_ - measuredVelocity_;
            command = target_ + pidOutput(error, dt, config_.maxVelocityDegPerSec);
        } else {
            // 位置环：位置误差 → 速度指令（单环串级的第一级）。
            // 增益全 0 时输出恒为 0 —— 没整定的位置环不会动，真设备也是如此。
            // 反馈量是**编码器读数**：控制器看不到未量化的机械位置。
            // 后果是稳态会停在目标的一个计数之内（并可能有一个计数的极限环）——
            // 那是分辨率造成的，与整定无关，真设备同样如此。
            const float error = target_ - measuredPosition_;
            command = pidOutput(error, dt, config_.maxVelocityDegPerSec);
        }
    } else {
        // 不驱动时控制器**停止积累**：积分项在停机/故障期间继续涨，
        // 会让下一次启动的瞬间被一个陈旧的积分推一下。
        //
        // 这里是"控制器状态干净"的**唯一**兜底点。换环之所以不用单独清，
        // 是因为 SET_MODE 只在 Ready 下被放行，而 Ready 正在这条分支里 ——
        // 同一个不变式别在两处维护（删掉这处、靠换环去清，是清不掉的）。
        resetPidState();
    }
    lastCommand_ = command;

    // 执行器能力/安全保护：加减速限幅。
    const double maxDelta = static_cast<double>(config_.accelDegPerSec2) * dt;
    double delta = static_cast<double>(command) - static_cast<double>(velocity_);
    delta = std::max(-maxDelta, std::min(maxDelta, delta));

    // 一阶惯性：速度不会瞬间跟上指令，而是以时间常数 τ 指数逼近。
    // 这同时是"指令给了但机械跟不动"的物理来源。
    double commanded = static_cast<double>(velocity_) + delta;
    if (config_.inertiaTimeConstantMs > 0.0F) {
        const double tau = static_cast<double>(config_.inertiaTimeConstantMs) / 1000.0;
        const double alpha = 1.0 - std::exp(-dt / tau);
        commanded = static_cast<double>(velocity_) + (commanded - static_cast<double>(velocity_)) * alpha;
    }

    velocity_ = static_cast<float>(commanded);
    position_ += velocity_ * static_cast<float>(dt);
    updateMeasurement(nowMs);
}

void DeviceSimulator::updateMeasurement(std::uint64_t nowMs) {
    // ① 编码器量化：**只量化"看得见的那一份"，不动机械位置**。
    //
    // 第一版是就地量化（`position_ = round(position_*countsPerDeg)/countsPerDeg`），
    // 结果每拍丢掉不足一个计数的零头（1 度 = 11.3778 计数，每拍被抹掉 0.3778 个），
    // 于是位置**系统性少走 3.3%**：目标 100 deg/s 稳定后报出来恒是 96.68。
    // 真实编码器不会丢计数 —— 机械位置连续变化，编码器只是把它读数化。
    const float countsPerDeg =
        (config_.encoderCountsPerRev > 0.0F) ? (config_.encoderCountsPerRev / 360.0F) : 0.0F;
    measuredPosition_ = (countsPerDeg > 0.0F)
                           ? std::floor(position_ * countsPerDeg + 0.5F) / countsPerDeg
                           : position_;

    // ② 速度估计：在固定窗口上差分**量化后**的位置。
    //    真固件也不会用"单周期差分"——100 Hz 下单周期差分会被量化噪声淹没
    //    （4096 计数/转时，10 ms 里只走过 11 个计数，±1 个计数就是 ±9%）。
    positionWindow_.push_back(PositionSample{nowMs, measuredPosition_});
    const std::uint64_t windowMs =
        static_cast<std::uint64_t>(std::max(1.0F, config_.velocityWindowMs));
    while (positionWindow_.size() > 2U && (nowMs - positionWindow_.front().tMs) > windowMs) {
        positionWindow_.erase(positionWindow_.begin());
    }
    const PositionSample& oldest = positionWindow_.front();
    const PositionSample& newest = positionWindow_.back();
    const std::uint64_t spanMs = (newest.tMs > oldest.tMs) ? (newest.tMs - oldest.tMs) : 0U;
    measuredVelocity_ = (spanMs > 0U)
                            ? (newest.positionDeg - oldest.positionDeg) * 1000.0F /
                                  static_cast<float>(spanMs)
                            : 0.0F;
}

float DeviceSimulator::pidOutput(float error, double dt, float clampAbs) noexcept {
    const double kp = static_cast<double>(pid_.kp);
    const double ki = static_cast<double>(pid_.ki);
    const double kd = static_cast<double>(pid_.kd);

    integralError_ += static_cast<double>(error) * dt;

    // 第一拍不做微分：上一帧的误差还是 0，此刻的差分会是一个凭空的尖峰
    // （"微分冲击"）。真控制器上电时也要先把历史对齐再开微分。
    double derivative = 0.0;
    if (pidPrimed_ && dt > 0.0) {
        derivative = (static_cast<double>(error) - lastPidError_) / dt;
    }
    pidPrimed_ = true;
    lastPidError_ = error;

    double out = kp * error + ki * integralError_ + kd * derivative;

    // 抗积分饱和：输出一旦被限幅，就按"多出来的那部分"把积分退回去。
    // 不退的话，目标给大一截之后积分会涨到天上，反向时半天收不回来 ——
    // 这个坑在真设备上一样会踩，所以仿真里保留了它，并给出标准的处置。
    const double limit = static_cast<double>(clampAbs);
    if (out > limit || out < -limit) {
        const double excess = (out > limit) ? (out - limit) : (out + limit);
        if (ki != 0.0) {
            integralError_ -= excess / ki;
        }
        out = (out > limit) ? limit : -limit;
    }
    return static_cast<float>(out);
}

void DeviceSimulator::resetPidState() noexcept {
    integralError_ = 0.0;
    lastPidError_ = 0.0F;
    pidPrimed_ = false;
}

void DeviceSimulator::runSafetyChecks(std::uint64_t nowMs, TickResult& result) {
    // ① 设备侧通信丢失保护（F7）。
    //    只保护 Running：别的状态下没有输出，超时无需动作。
    if (config_.commLossTimeoutMs > 0U && hasReceivedFrame_ &&
        stateMachine_.is(DeviceState::Running) &&
        (nowMs - lastValidFrameMs_) > config_.commLossTimeoutMs) {
        faultCode_ = FaultCode::CommunicationLoss;
        result.commLossFaulted = applyEvent(DeviceEvent::FaultRaised).accepted;
    }

    // ② 停机判稳。STOP 是单向的（ADR-004），设备拿不到"已停"的回执，
    //    只能靠自己把"速度已经收敛"翻译成一次状态迁移，上位机才有办法
    //    把 Stopping 和 Ready 分开显示。
    //    判据用**测出来的**速度（设备能看见的那个），不是内部真值 ——
    //    真设备也没得选。
    if (stateMachine_.is(DeviceState::Stopping) &&
        std::fabs(measuredVelocity_) <= config_.stopVelocityThresholdDegPerSec) {
        applyEvent(DeviceEvent::StopCompleted);
    }

    // ③ 故障注入（测试用）。
    if (config_.forcedFaultCode != FaultCode::None && !stateMachine_.is(DeviceState::Fault)) {
        faultCode_ = config_.forcedFaultCode;
        applyEvent(DeviceEvent::FaultRaised);
    }

    // ④ 回零：到位或超时，二选一。
    //
    //    判据用的是**设备看得见的**位置（编码器读数），不是机械真值 ——
    //    真设备的"到位"判据也只能来自传感器。所以这里有一个真实存在的边界：
    //    容差小于一个编码器计数时永远等不到到位（见 SimulatorConfig 的说明）。
    if (stateMachine_.is(DeviceState::Home)) {
        if (std::fabs(measuredPosition_) <= config_.homeToleranceDeg) {
            defineOriginHere(nowMs);
            applyEvent(DeviceEvent::HomeCompleted);
        } else if (homeTimeoutMs_ > 0U && (nowMs - homeStartedMs_) > homeTimeoutMs_) {
            // 超时 = 走不到零点。原因可能是零点在行程之外、机械卡住、传感器坏了。
            // 上位机只看到"进故障了 + 故障码 6"，所以原因**必须**在代码表里说清楚
            // （见 alarm_text 的 kAlarmCodeHomeFailed）。
            faultCode_ = FaultCode::HomeFailed;
            applyEvent(DeviceEvent::FaultRaised);
        }
    }

    // ⑤ 软限位：**位置**越界就撤输出并进故障。
    //
    //    判据用编码器读数（设备看得见的那个）。回零期间不判：那时零点还没建立，
    //    限位是相对零点定义的，拿它去拦一段"去找零点"的运动是本末倒置。
    //    Fault 之后 isEnabled() 为假 → 下一拍 command=0 → 按加减速限幅收住。
    if (softLimits_.enabled && !stateMachine_.is(DeviceState::Home) &&
        stateMachine_.is(DeviceState::Running) &&
        !softLimitAllows(softLimits_, measuredPosition_)) {
        faultCode_ = FaultCode::OverLimit;
        applyEvent(DeviceEvent::FaultRaised);
    }
}

void DeviceSimulator::defineOriginHere(std::uint64_t nowMs) {
    // **把当前位置定义成零点**。真设备上这一步就是"记下开关的位置、把编码器
    // 计数清零"；这里同样是把坐标搬到零点，而不是把数值改小一点。
    position_ = 0.0F;
    measuredPosition_ = 0.0F;
    velocity_ = 0.0F;
    measuredVelocity_ = 0.0F;
    lastCommand_ = 0.0F;
    target_ = 0.0F;
    // 零点建立了 —— 这是遥测里那一位**唯一**被置位的时刻（见 payload.hpp）。
    // 放在这里而不是放在 HomeCompleted 的分支里：谁能置位这一位，应当与
    // "谁把坐标搬到了零点"是同一处，否则两处迟早会有一个先被改。
    homingAttained_ = true;
    // 测量窗口里存的还是"零点之前"的样本，留着会让下一拍的速度差分
    // 看到一个凭空跳变的位置 —— 那是伪速度，还会被当成超速报警。
    positionWindow_.clear();
    positionWindow_.push_back(PositionSample{nowMs, 0.0F});
}

std::uint64_t DeviceSimulator::homeElapsedMs(std::uint64_t nowMs) const noexcept {
    if (!stateMachine_.is(DeviceState::Home)) {
        return 0U;
    }
    return (nowMs > homeStartedMs_) ? (nowMs - homeStartedMs_) : 0U;
}

void DeviceSimulator::handleFrame(const Frame& frame, TickResult& result) {
    // HELLO 必须**绕过去重窗口**（而且不记入窗口）。
    //
    // 不走这条特例会出现一个自指的僵局：上位机重启后仍从 seq=0 发 HELLO，
    // 而窗口里恰好还留着上一会话的 (Hello, 0) —— 于是这条 HELLO 被当成重传
    // 直接回放掉，`recent_.clear()` 永远不会执行，新会话随后的所有命令
    // 都会与旧会话的 seq 撞车并被静默吞掉。现象正是"握手看起来成功了，
    // 但一条命令都不生效"。
    //
    // HELLO 不需要去重是合理的：它没有业务副作用，只重置会话状态，
    // 且本身幂等；应答丢了再来一次反而正是我们想要的。
    if (frame.type == MessageType::Hello) {
        (void)executeFrame(frame, result);
        return;
    }

    if (const DedupEntry* seen = findRecent(frame.type, frame.seq)) {
        // 重传：重放上一次的应答，但**不重复执行副作用**。
        ++result.duplicatesSuppressed;
        replayReply(*seen, result);
        return;
    }

    const Reply reply = executeFrame(frame, result);
    rememberReply(frame.type, frame.seq, reply);
}

DeviceSimulator::Reply DeviceSimulator::executeFrame(const Frame& frame, TickResult& result) {
    // 门禁先行：状态机说不行就立刻回 NAK，**不产生任何副作用**。
    // 放在 switch 之前而不是散在各 case 里，是为了保证"新增命令类型时
    // 不可能忘记过门禁"—— 这是唯一一个入口。
    if (stateMachine_.verdictFor(frame.type) == CommandVerdict::RejectIllegalState) {
        return replyWithNack(frame.seq, NackReason::IllegalState, result);
    }

    switch (frame.type) {
        case MessageType::Hello:
            // HELLO 是会话起点，因此清空去重窗口。
            // 不清的后果很隐蔽：上位机重启后会从 seq=0 重新编号，那些命令会被
            // 当成上一次会话的重传而全部静默吞掉 —— 表现为"握手成功但一条命令都不生效"。
            recent_.clear();
            // ⚠️ 若存在**未确认的故障**，状态机会把结果判成 Fault 而不是 Ready ——
            // 重连不能顺带把故障洗掉。此时仍然回 ACK：协议层面的握手确实成功了，
            // 设备的状态由随后的遥测如实上报。
            applyEvent(DeviceEvent::HandshakeOk);
            return replyWithAck(MessageType::HelloAck, frame.seq, result);

        case MessageType::Heartbeat:
            return replyWithAck(MessageType::HeartbeatAck, frame.seq, result);

        case MessageType::SetMode: {
            // 门禁已保证此刻处于 Ready —— 运动中途改模式会换掉 target 的单位语义。
            SetMode payload;
            if (decodeSetMode(frame.payload.data(), frame.payload.size(), payload) !=
                    PayloadError::None ||
                !isSetModeValid(payload)) {
                ++result.malformedPayloads;
                return replyWithNack(frame.seq, NackReason::BadParameter, result);
            }
            runMode_ = static_cast<SimRunMode>(payload.mode);
            if (runMode_ == SimRunMode::Stopped) {
                target_ = 0.0F;
            }
            // 这里刻意**不**再清一次控制器状态：SET_MODE 只在 Ready 下被放行，
            // 而 Ready 每拍都走 integrate() 的不驱动分支，PID 状态早就是干净的。
            // （位置环的误差是度、速度环的是度/秒，换环确实不能带旧积分 ——
            //   那条不变式由不驱动分支统一兜底，见那里的注释。）
            return replyWithAck(MessageType::SetModeAck, frame.seq, result);
        }

        case MessageType::Start:
            if (runMode_ == SimRunMode::Stopped) {
                // 门禁放行了（确实是 Ready），但"没选控制模式就使能"在语义上不成立。
                // 这不是状态机该管的事：状态没问题，缺的是配置。
                return replyWithNack(frame.seq, NackReason::IllegalState, result);
            }
            applyEvent(DeviceEvent::StartAccepted);
            return replyWithAck(MessageType::StartAck, frame.seq, result);

        case MessageType::Stop:
            applyEvent(DeviceEvent::StopReceived);
            target_ = 0.0F;
            // 单向：不回 ACK（ADR-004）。停止是幂等的安全动作，重复无副作用。
            return Reply{};

        case MessageType::SetTarget: {
            // 门禁已保证是 Ready 或 Running。Ready 也放行的理由见 device_state.cpp。
            SetTarget payload;
            if (decodeSetTarget(frame.payload.data(), frame.payload.size(), payload) !=
                    PayloadError::None ||
                !isSetTargetValid(payload)) {
                ++result.malformedPayloads;
                return replyWithNack(frame.seq, NackReason::BadParameter, result);
            }
            // 位置环的目标**站在限位之外**就直接拒：等它跑过去再报超限故障，
            // 等于让设备明知故犯地撞一次墙。速度环的目标是 deg/s，
            // 与位置限位不是一回事，所以只在位置环判。
            if (runMode_ == SimRunMode::Position &&
                !softLimitAllows(softLimits_, payload.target)) {
                ++result.malformedPayloads;
                return replyWithNack(frame.seq, NackReason::BadParameter, result);
            }
            target_ = payload.target;
            return replyWithAck(MessageType::SetTargetAck, frame.seq, result);
        }

        case MessageType::ReadParams:
            // 这条命令**不带参数**。非空载荷说明对端与我们对格式的理解不一致 ——
            // 与其猜它想读什么，不如明确拒掉：一个猜错的"读回结果"比一次拒绝危险得多。
            if (!frame.payload.empty()) {
                ++result.malformedPayloads;
                return replyWithNack(frame.seq, NackReason::BadParameter, result);
            }
            return replyWithParamState(frame.seq, result);

        case MessageType::SetPid: {
            SetPid payload;
            if (decodeSetPid(frame.payload.data(), frame.payload.size(), payload) !=
                    PayloadError::None ||
                !isSetPidValid(payload)) {
                ++result.malformedPayloads;
                return replyWithNack(frame.seq, NackReason::BadParameter, result);
            }
            pid_ = payload;
            return replyWithAck(MessageType::SetPidAck, frame.seq, result);
        }

        case MessageType::Home: {
            HomeParam param;
            if (decodeHomeParam(frame.payload.data(), frame.payload.size(), param) !=
                    PayloadError::None ||
                !isHomeParamValid(param)) {
                ++result.malformedPayloads;
                return replyWithNack(frame.seq, NackReason::BadParameter, result);
            }
            // 设备**自己**还要再夹一层：报文里的速度是上位机说的，
            // 而"这台机器能接受多快"是设备的知识（配置里的 maxVelocity）。
            // 取小值，不取大值 —— 回零是朝一个机械限位走过去，快不是优点。
            homeSpeedLimit_ = std::min(param.speedDegPerSec, config_.maxVelocityDegPerSec);
            homeTimeoutMs_ = param.timeoutMs;
            homeStartedMs_ = lastTickMs_;
            // 一开始就把"本轮回零已建立零点"清掉：位说的是**这一轮**。
            // 不清的后果很具体 —— 一台刚回完零的机器再回一次、半路被停住，
            // 位还留着上一次的 1，上位机会把这一轮读成成功。
            homingAttained_ = false;
            // 回零期间设备**自己**切到位置环、目标设为零点：这是设备侧的一段流程，
            // 不是"上位机设了一串参数"。所以它不经过 SET_MODE 的门禁，也不需要
            // 上位机先选对模式 —— 停机回零本来就该在未知模式下也能做。
            //
            // 它**保持**在位置环（不回退到回零前的模式）：模式是状态，
            // 状态要能被看见。"回零把它改回去了"这种事没人能事后查出来。
            runMode_ = SimRunMode::Position;
            target_ = 0.0F;
            resetPidState();
            if (!applyEvent(DeviceEvent::HomeAccepted).accepted) {
                // 门禁放行了、事件却被拒 —— 说明状态在这两处之间变了（不可能），
                // 或者本模块有 bug。回一个状态非法让上位机看得见，不静默吞掉。
                return replyWithNack(frame.seq, NackReason::IllegalState, result);
            }
            return replyWithAck(MessageType::HomeAck, frame.seq, result);
        }

        case MessageType::SetLimits: {
            SoftLimits payload;
            if (decodeSoftLimits(frame.payload.data(), frame.payload.size(), payload) !=
                    PayloadError::None ||
                !isSoftLimitsValid(payload)) {
                ++result.malformedPayloads;
                return replyWithNack(frame.seq, NackReason::BadParameter, result);
            }
            // 一条额外的业务校验，而不是结构校验：**把当前位置关在门外的限位**
            // 是一句与事实矛盾的声明 —— 操作员在说"这台轴只许在 A~B 之间"，
            // 而轴此刻就在 C。它多半意味着符号、单位或轴号搞错了，而后果是
            // 这台设备从此**永远不能合法运行**（每次 START 都会在限位那一刻停机）。
            // 当场拒掉比"设完之后每次一开就报故障"清楚得多。
            //
            // ⚠️ 刻意**不**连带检查当前目标值：目标是一个随时会被下一条命令覆盖的
            // 数值（停机后它往往还是上一段的 0），拿它去挡限位会让一次正常的下发
            // 卡在一个马上就要改掉的数字上 —— 而那种拒绝只会回一条 "参数不合法"，
            // 说不清是哪一个字段。限位本身照样会在运行时执法：目标真在界外，
            // 轴撞到边界就会停，那正是限位该做的事。
            if (payload.enabled && !softLimitAllows(payload, measuredPosition_)) {
                ++result.malformedPayloads;
                return replyWithNack(frame.seq, NackReason::BadParameter, result);
            }
            softLimits_ = payload;
            return replyWithAck(MessageType::SetLimitsAck, frame.seq, result);
        }

        case MessageType::ClearFault: {
            // 门禁已保证此刻处于 Fault。这里还要看**载荷里的故障码**是不是
            // 当前锁存的那个：不是就说明操作员确认的是另一个故障（多半屏幕停在
            // 旧状态上），拒绝比"顺手把当前这个也清掉"安全。
            ClearFault payload;
            if (decodeClearFault(frame.payload.data(), frame.payload.size(), payload) !=
                    PayloadError::None ||
                !isClearFaultValid(payload)) {
                ++result.malformedPayloads;
                return replyWithNack(frame.seq, NackReason::BadParameter, result);
            }
            if (payload.faultCode != static_cast<std::uint8_t>(faultCode_)) {
                return replyWithNack(frame.seq, NackReason::BadParameter, result);
            }

            // 真机上这是两步：人工确认（RecoveryConfirmed）→ 设备自检通过
            // （RecoveryOk）。模拟器**没有可自检的硬件**，所以两步在这里连续走完 ——
            // "确认即恢复"是模拟器的局限，不是协议少了一步；接真设备时
            // Recovery 态会真的停留若干拍，而界面本来就按遥测显示状态，无须改动。
            const bool confirmed = applyEvent(DeviceEvent::RecoveryConfirmed).accepted;
            const bool cleared = confirmed && applyEvent(DeviceEvent::RecoveryOk).accepted;
            if (!cleared) {
                // 走不到这里（门禁已保证 Fault），但绝不能"没清掉还回 ACK"。
                return replyWithNack(frame.seq, NackReason::IllegalState, result);
            }
            return replyWithAck(MessageType::ClearFaultAck, frame.seq, result);
        }

        default:
            // 未知类型、以及设备不该收到的 ACK 帧，一律回 NAK 并说明原因。
            ++result.unknownTypes;
            return replyWithNack(frame.seq, NackReason::UnknownType, result);
    }
}

DeviceSimulator::Reply DeviceSimulator::replyWithAck(MessageType ackType, std::uint16_t seq,
                                                     TickResult& result) {
    queueAck(ackType, seq, result);
    Reply reply;
    reply.kind = Reply::Kind::Ack;
    reply.ackType = ackType;
    return reply;
}

DeviceSimulator::Reply DeviceSimulator::replyWithParamState(std::uint16_t seq, TickResult& result) {
    ParamState state;
    state.mode = static_cast<std::uint8_t>(runMode_);
    // 报的是**存着的**目标，不是"此刻在追的" —— 后者在非 Running 时恒为 0，
    // 拿它做回读核对等于永远核对不上（见 emitTelemetry 里那段说明）。
    state.target = target_;
    state.kp = pid_.kp;
    state.ki = pid_.ki;
    state.kd = pid_.kd;
    // 软限位也一起回读：上位机要能显示"设备**现在**拿哪一组在拦"，
    // 而不是"我以为刚才设成了什么"。
    state.limits = softLimits_;

    const std::vector<std::uint8_t> payload = encodeParamState(state);
    queueFrame(MessageType::ReadParamsAck, seq, payload);
    ++result.acksSent;

    Reply reply;
    reply.kind = Reply::Kind::Ack;
    reply.ackType = MessageType::ReadParamsAck;
    reply.payload = payload;
    return reply;
}

DeviceSimulator::Reply DeviceSimulator::replyWithNack(std::uint16_t seq, NackReason reason,
                                                      TickResult& result) {
    queueNack(seq, reason, result);
    Reply reply;
    reply.kind = Reply::Kind::Nack;
    reply.reason = reason;
    return reply;
}

void DeviceSimulator::replayReply(const DedupEntry& entry, TickResult& result) {
    switch (entry.reply.kind) {
        case Reply::Kind::Ack:
            if (entry.reply.payload.empty()) {
                queueAck(entry.reply.ackType, entry.seq, result);
            } else {
                // 带载荷的应答按**原样**重放：重传的参数回读必须拿回同样的那份参数。
                queueFrame(entry.reply.ackType, entry.seq, entry.reply.payload);
                ++result.acksSent;
            }
            break;
        case Reply::Kind::Nack:
            queueNack(entry.seq, entry.reply.reason, result);
            break;
        case Reply::Kind::None:
            // STOP：单向、无应答，且本身幂等 —— 重放就是"什么都不做"。
            break;
    }
}

void DeviceSimulator::rememberReply(MessageType type, std::uint16_t seq, const Reply& reply) {
    DedupEntry entry;
    entry.type = type;
    entry.seq = seq;
    entry.reply = reply;
    recent_.push_back(entry);

    if (recent_.size() > kSimulatorDedupWindowSize) {
        recent_.erase(recent_.begin());
    }
}

const DeviceSimulator::DedupEntry* DeviceSimulator::findRecent(MessageType type,
                                                              std::uint16_t seq) const noexcept {
    for (const DedupEntry& entry : recent_) {
        if (entry.type == type && entry.seq == seq) {
            return &entry;
        }
    }
    return nullptr;
}

void DeviceSimulator::emitTelemetry(TickResult& result) {
    Telemetry telemetry;
    telemetry.timestampMs = static_cast<std::uint32_t>(lastTickMs_);
    // 上报的是**测出来的**速度（编码器差分），不是仿真内部真值 ——
    // 真设备报的也只能是这个。
    telemetry.velocityDegPerSec = measuredVelocity_;
    telemetry.positionDeg = measuredPosition_;
    // 上报的是**有效目标**：Stopping / Fault 期间设备实际在追 0，
    // 若照实报旧目标，界面会显示"设备还在往那个值跑"。
    telemetry.target =
        (stateMachine_.is(DeviceState::Running) && runMode_ != SimRunMode::Stopped) ? target_ : 0.0F;
    telemetry.state = static_cast<std::uint8_t>(stateMachine_.state());
    telemetry.faultCode = static_cast<std::uint8_t>(faultCode_);
    // 状态位组：目前只有"本轮回零是否已建立零点"这一位。
    // 位是**持续**报的（不是一次性事件），因为上位机要在"离开回零态的那一刻"
    // 才能读到它 —— 而那一刻可能落在两拍之间（见 payload.hpp）。
    telemetry.statusFlags =
        homingAttained_ ? kTelemetryFlagHomingAttained : static_cast<std::uint8_t>(0U);

    // 占空比由**这一拍的指令**算：它是设备自己知道、也自己写下去的量。
    // 拿测出来的速度反推占空比会把"指令"与"结果"混为一谈（停机后滑行的电机
    // 会显示一个非零占空比，那是不对的）。
    float ratio = std::fabs(lastCommand_) / config_.maxVelocityDegPerSec;
    ratio = std::max(0.0F, std::min(1.0F, ratio));
    telemetry.pwmOutput =
        static_cast<std::uint16_t>(ratio * static_cast<float>(config_.pwmFullScale) + 0.5F);

    lastTelemetry_ = telemetry;

    queueFrame(MessageType::Telemetry, outboundSeq_, encodeTelemetry(telemetry));
    ++outboundSeq_;
    ++result.telemetrySent;
}

void DeviceSimulator::queueFrame(MessageType type, std::uint16_t seq,
                                const std::vector<std::uint8_t>& payload) {
    Frame frame;
    frame.version = kProtocolVersion;
    frame.type = type;
    frame.seq = seq;
    frame.payload = payload;

    const EncodeResult encoded = encodeFrame(frame);
    if (!encoded) {
        // 不静默吞掉：计入内部不变式计数器，由测试断言它为 0。
        ++encodeFailures_;
        return;
    }
    outgoing_.insert(outgoing_.end(), encoded.bytes.begin(), encoded.bytes.end());
}

void DeviceSimulator::queueAck(MessageType ackType, std::uint16_t seq, TickResult& result) {
    queueFrame(ackType, seq, {});
    ++result.acksSent;
}

void DeviceSimulator::queueNack(std::uint16_t rejectedSeq, NackReason reason, TickResult& result) {
    Nack nack;
    nack.rejectedSeq = rejectedSeq;
    nack.reason = reason;

    nacksSent_.push_back(nack);

    // 帧头 SEQ 也回填被拒的 seq —— 这样上位机可以用**统一规则**做相关：
    // "任何响应帧的帧头 SEQ 都指向它对应的请求"。载荷里那一份是权威记录，
    // 两者必须一致（由端到端用例断言）。
    queueFrame(MessageType::Nack, rejectedSeq, encodeNack(nack));
    ++result.nacksSent;
}

std::size_t DeviceSimulator::readOutgoing(std::uint8_t* out, std::size_t maxLen) {
    if (out == nullptr || maxLen == 0U) {
        return 0U;
    }
    const std::size_t count = std::min(maxLen, outgoing_.size());
    std::copy_n(outgoing_.begin(), static_cast<std::ptrdiff_t>(count), out);
    outgoing_.erase(outgoing_.begin(), outgoing_.begin() + static_cast<std::ptrdiff_t>(count));
    return count;
}

}  // namespace mc
