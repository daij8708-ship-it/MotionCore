#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "mc/device_state.hpp"
#include "mc/frame_codec.hpp"
#include "mc/payload.hpp"
#include "mc/protocol.hpp"

namespace mc {

// ---------------------------------------------------------------------------
// 最小可用版虚拟设备（协议对端）
//
// 这是"没有硬件时唯一的对端"，因此在**协议层面**必须与真实设备等价：
// 同一套帧格式、同一套校验、同一套拒绝规则、**同一个状态机**。
// 差别只在运动模型 —— 下面是**声明式的仿真模型，不是实物测量结果**：
//
//   速度：v 以 accelDegPerSec2 的速率限幅地趋近目标（一阶速率限制）
//   位置：p 对 v 积分
//   PWM ：与 |v| 成正比
//
// 完整版（惯性、编码器量化、PID 跟随、设备侧故障注入）属阶段 8。
//
// 阶段 4 起，本类的状态**一律由 DeviceStateMachine 裁决**，不再自己现算：
// 开局处于 Initializing（构造等价于"链路已打开、尚未握手"），必须走完一次
// HELLO 往返才进 Ready，此后才接受控制命令。
// ---------------------------------------------------------------------------

// 运行模式与设备状态是**正交**的两件事：
//   DeviceState 回答"现在允不允许被控制"，SimRunMode 回答"用哪个环去控制"。
enum class SimRunMode : std::uint8_t {
    Stopped = 0U,
    Velocity = 1U,
    Position = 2U,
};

static_assert(static_cast<std::uint8_t>(SimRunMode::Position) == kMaxRunMode,
              "SimRunMode 必须与 SET_MODE 载荷允许的模式编号一一对应");

// 设备侧序列号去重窗口：记住最近处理过的这么多个命令帧及其应答。
//
// 为什么是"窗口"而不是"只记最后一个"：应答在途时可能有多条命令重叠。
// 为什么取 32 而不更大：重传只可能发生在应答超时窗口之内（毫秒级），
// 32 条命令的跨度远超该窗口，再大只是白占内存。
inline constexpr std::size_t kSimulatorDedupWindowSize = 32U;

// 单拍最多产多少帧遥测。这是**链路约束倒推出来的**，不是拍的数：
// 一帧遥测 30 B，上位机单拍最多读 kMaxBytesPerExchange = 512 B，
// 16 × 30 = 480 B 留出了应答帧的余量。超过上限的部分丢弃并计数 ——
// 静默丢弃会让"设备欠着没发"和"上位机没读走"这两件事再也分不清。
inline constexpr std::size_t kMaxTelemetryFramesPerTick = 16U;

struct SimulatorConfig {
    float maxVelocityDegPerSec = 384.0F;   // 与上位机的量程一致
    float accelDegPerSec2 = 1200.0F;
    // 设备**自己**的产帧周期（毫秒）。上位机取得晚，设备不会因此少发：
    // 每次来取都把这段时间该发的帧补出来（上限见 kMaxTelemetryFramesPerTick）。
    // 0 是特例：不做补发，来取就发一帧（照补发算会变成死循环）。
    std::uint32_t telemetryPeriodMs = 10U; // 100 Hz
    std::uint16_t pwmFullScale = 99U;      // 对应固件定时器的 ARR = 99

    // Stopping → Ready 的判据：|v| 落在这个门限内就算停稳。
    // 取满量程的 1%（3.84 deg/s ≈ 0.64 rpm）：远低于任何有意义的指令速度，
    // 又大于速率限幅一拍的步长（1200 × 0.01 / 2 = 6 deg/s）——
    // 取太小"停稳"永远等不到，取太大把"还在滑行"报成"已停"。
    float stopVelocityThresholdDegPerSec = 3.84F;

    // --- 运动模型（阶段 8：从"声明式最小模型"升级为完整模型）--------------------
    //
    // 下面三个参数描述的是**机械与传感**，不是控制律：
    //   inertiaTimeConstantMs  实际速度对速度指令的一阶滞后（转子+负载的惯性）。
    //                          0 = 无惯性（瞬时跟随），只用于对照实验。
    //   encoderCountsPerRev    编码器每转计数（4 倍频后）。位置按它**量化**，
    //                          速度则由量化后的位置差分得到 —— 设备只看得到量化值。
    //   velocityWindowMs       速度估计的差分窗口。真固件不会用"单周期差分"
    //                          （100 Hz 下抖到没法用），而是在固定窗口上做差分。
    //                          窗口越长越平滑、越滞后；这里取 50 ms（5 拍）。
    float inertiaTimeConstantMs = 40.0F;
    float encoderCountsPerRev = 4096.0F;
    float velocityWindowMs = 50.0F;

    // 设备侧的整定初值。**全 0 是有明确含义的**：速度环退化成纯前馈（照常跟随），
    // 位置环因为没有反馈增益而不会动 —— 真设备上一台没有整定过的位置环也是这样。
    // 要跑位置环就显式给一组增益（`mc_sim --kp/--ki/--kd`，或上位机 SET_PID）。
    SetPid initialPid{};

    // 设备侧通信丢失保护（F7）的超时窗口。0 = 关闭（仅供对照实验）。
    std::uint64_t commLossTimeoutMs = kDeviceCommLossTimeoutMs;

    // --- 回零（P1-6）-----------------------------------------------------------
    //
    // 回零的判据是"位置进入容差带"，而位置是**编码器量化过**的读数。
    // 容差必须大于一个计数（4096 计数/转 ≈ 0.088 deg），否则伺服永远等不到
    // "到位"—— 这一条是真设备上同样存在的性质，不是仿真的毛病。
    float homeToleranceDeg = 0.25F;
    // 回零的接近速度与位置误差成正比（v = 误差 / 这个时间常数），并受报文里
    // 给的速度上限约束。用"误差/时间常数"而不是"全速撞过去再刹"的理由：
    // 前者速度随误差一起衰减，接近零位时自然减速，**不会冲过去再来回摆**。
    float homeApproachSeconds = 0.2F;

    // --- 软限位（P1-6）---------------------------------------------------------
    //
    // 出厂**未启用**。这不是"默认值随便取一个"：限位是每台机器的行程，
    // 没有人能替现场定它，而一个"看起来有、其实是编的"限位比没有更危险。
    // 上位机可以下发（SET_LIMITS），也可以回读（READ_PARAMS）核对。
    SoftLimits softLimits{};

    // 故障注入：非 None 时设备进入 Fault 并持续上报该故障码。
    FaultCode forcedFaultCode = FaultCode::None;
};

class DeviceSimulator {
public:
    explicit DeviceSimulator(SimulatorConfig config = SimulatorConfig{});

    struct TickResult {
        std::size_t framesReceived = 0;
        std::size_t acksSent = 0;
        std::size_t nacksSent = 0;
        std::size_t telemetrySent = 0;
        std::size_t unknownTypes = 0;
        std::size_t malformedPayloads = 0;
        std::size_t duplicatesSuppressed = 0;   // 判为重传、只重放应答未重复执行的帧数
        std::size_t telemetryBacklogDropped = 0; // 本拍因超过上限而丢弃的待发遥测帧数
        bool commLossFaulted = false;           // 本拍因通信丢失进入 Fault（F7）
    };

    // 推进一个 tick：先积分运动，再处理收到的字节，再做安全检查，
    // 最后按设备自己的周期补发这段时间该发的遥测。
    TickResult tick(const std::uint8_t* incoming, std::size_t len, std::uint64_t nowMs);

    // 取出本设备待发出的字节。
    std::size_t readOutgoing(std::uint8_t* out, std::size_t maxLen);

    // --- 观测（供测试与上位机断言）------------------------------------------
    SimRunMode runMode() const noexcept { return runMode_; }
    DeviceState state() const noexcept { return stateMachine_.state(); }

    // "输出带电中" = Running 或 Stopping（Stopping 期间电机还在减速，仍带电）。
    // 用 isEnabled() 判断"电机是不是在动"是不够的：Fault 之后速度还在限幅收敛。
    bool isEnabled() const noexcept {
        return stateMachine_.is(DeviceState::Running) || stateMachine_.is(DeviceState::Stopping);
    }

    bool faultLatched() const noexcept { return stateMachine_.faultLatched(); }
    FaultCode faultCode() const noexcept { return faultCode_; }
    const DeviceStateMachine& stateMachine() const noexcept { return stateMachine_; }

    // **本轮回零**是否已经建立零点（遥测状态位组里的那一位，见 payload.hpp）。
    // 进入回零时清掉、零点建立时置位 —— 与 CiA402 的 *Homing attained* 同义。
    // 它说的是"这一次回零成没成"，不是"这台机器历史上有没有回过零"。
    bool homingAttained() const noexcept { return homingAttained_; }

    float target() const noexcept { return target_; }
    // **设备报出来的**速度：由编码器量化后的位置在固定窗口上差分得到。
    // 它与内部真实速度不是一回事（真实速度是仿真里才有的"上帝视角"），
    // 控制器使用的反馈也是这一个 —— 真设备看不到未量化的值。
    float velocityDegPerSec() const noexcept { return measuredVelocity_; }
    // 内部真实速度。**刻意只在仿真里存在**：给测试做"控制律到底控住了没有"的对照，
    // 不参与遥测、也不进控制回路。
    float trueVelocityDegPerSec() const noexcept { return velocity_; }
    // 同样是**设备看得见的**位置（编码器量化后的读数），不是机械真值。
    float positionDeg() const noexcept { return measuredPosition_; }
    const SetPid& pid() const noexcept { return pid_; }
    // 设备**存着**的软限位。上位机回读到的同一个来源。
    const SoftLimits& softLimits() const noexcept { return softLimits_; }
    // 本次回零已经走了多久（没在回零时为 0）。供"回零是不是卡住了"的断言用。
    std::uint64_t homeElapsedMs(std::uint64_t nowMs) const noexcept;
    const StreamDecoder& decoder() const noexcept { return decoder_; }

    // 本设备发出的最近 N 条 NAK（含拒绝原因），便于断言"该拒的拒了"。
    const std::vector<Nack>& nacksSent() const noexcept { return nacksSent_; }

    // 自身编码失败的次数。设备自己的载荷都是定长小对象，理论上永远是 0 ——
    // 因此这是一个**内部不变式**：非 0 就说明本模块有 bug，不能静默吞掉。
    std::uint64_t encodeFailures() const noexcept { return encodeFailures_; }

    Telemetry lastTelemetry() const noexcept { return lastTelemetry_; }

    // 因超过单拍产帧上限而被丢弃的累计帧数。
    // 它非 0 只说明"上位机取得太慢"，不是设备故障 —— 但必须看得到：
    // 否则上位机侧的丢帧统计会背上不属于它的账。
    std::uint64_t telemetryBacklogDropped() const noexcept { return telemetryBacklogDropped_; }

    // 把状态机事件喂进来（链路层、故障检测、以及测试都走这一个窄口）。
    // 刻意只开这一个口，而不是把 DeviceStateMachine& 整个暴露出去 ——
    // 否则"谁能改状态"就说不清了。
    TransitionResult applyEvent(DeviceEvent event) noexcept;

private:
    // 对一帧请求作出的应答。去重命中时用它**重放**上一次的应答 ——
    // 只去重不重放是不行的：对端丢了 ACK 后重传，会一个应答都拿不到，
    // 幂等命令的重传机制就被去重机制废掉了。
    struct Reply {
        enum class Kind : std::uint8_t { None = 0U, Ack = 1U, Nack = 2U };
        Kind kind = Kind::None;                        // None = 该请求本来就不应答（只有 STOP）
        MessageType ackType = MessageType::Nack;       // 仅 Ack 时有效
        NackReason reason = NackReason::IllegalState;  // 仅 Nack 时有效
        // 带载荷的应答（目前只有 READ_PARAMS_ACK）。**去重命中时要按原样重放**：
        // 重传的读请求若收到一个空应答，会被上位机判成坏数据 —— 那等于让
        // "读参数可重传"这条性质在去重窗口里失效。
        std::vector<std::uint8_t> payload;             // 仅 Ack 且非空时有效
    };

    struct DedupEntry {
        MessageType type = MessageType::Nack;
        std::uint16_t seq = 0U;
        Reply reply{};
    };

    void integrate(std::uint64_t nowMs);
    // 把**当前**位置定义成零点：位置归零、速度归零，并重置测量窗口。
    // 测量窗口必须一起重置：里面存的是"零点之前的"位置样本，
    // 拿它去差分会得到一个凭空出现的高速读数。
    void defineOriginHere(std::uint64_t nowMs);
    // 位置量化 + 速度估计（真设备只能看见编码器给的东西）。
    void updateMeasurement(std::uint64_t nowMs);
    // 一阶 PID：输出是**速度指令**（deg/s），带抗积分饱和。
    float pidOutput(float error, double dt, float clampAbs) noexcept;
    // 清控制器状态。换环、进故障、停机时都要清 —— 见实现里的理由。
    void resetPidState() noexcept;
    void runSafetyChecks(std::uint64_t nowMs, TickResult& result);
    void handleFrame(const Frame& frame, TickResult& result);
    Reply executeFrame(const Frame& frame, TickResult& result);
    Reply replyWithAck(MessageType ackType, std::uint16_t seq, TickResult& result);
    // 参数回读的应答：载荷是设备**实存**的那一组参数（见 payload.hpp 的 ParamState）。
    Reply replyWithParamState(std::uint16_t seq, TickResult& result);
    Reply replyWithNack(std::uint16_t seq, NackReason reason, TickResult& result);
    void replayReply(const DedupEntry& entry, TickResult& result);
    void rememberReply(MessageType type, std::uint16_t seq, const Reply& reply);
    const DedupEntry* findRecent(MessageType type, std::uint16_t seq) const noexcept;
    void emitTelemetry(TickResult& result);
    // 按 telemetryPeriodMs 把这一拍该产的帧补出来（上限 kMaxTelemetryFramesPerTick）。
    void emitDueTelemetry(std::uint64_t nowMs, TickResult& result);
    void queueFrame(MessageType type, std::uint16_t seq, const std::vector<std::uint8_t>& payload);
    void queueAck(MessageType ackType, std::uint16_t seq, TickResult& result);
    void queueNack(std::uint16_t rejectedSeq, NackReason reason, TickResult& result);

    SimulatorConfig config_;
    StreamDecoder decoder_;
    std::vector<std::uint8_t> outgoing_;

    // 开局 Initializing：构造等价于"链路已打开、尚未握手"。
    DeviceStateMachine stateMachine_{DeviceState::Initializing};

    SimRunMode runMode_ = SimRunMode::Stopped;
    FaultCode faultCode_ = FaultCode::None;
    float target_ = 0.0F;

    // 回零的进行态：速度上限与时限（都来自那一条 HOME 报文），以及本次开始时刻。
    float homeSpeedLimit_ = 0.0F;
    std::uint64_t homeTimeoutMs_ = 0U;
    std::uint64_t homeStartedMs_ = 0U;
    // 本轮回零有没有建立零点（遥测状态位组的那一位）。进回零时清、零点建立时置 ——
    // 它必须由设备**持续地**报出去：被 STOP 打断的那条路径里，状态在两拍之间就
    // 从 Stopping 回到了 Ready，上位机采不到"被打断"这一格（见 payload.hpp）。
    bool homingAttained_ = false;
    // 设备存下来的软限位（SET_LIMITS 落地的地方，也会随 READ_PARAMS 回读出去）。
    SoftLimits softLimits_{};
    float velocity_ = 0.0F;          // 真实机械速度（仿真独有）
    float measuredVelocity_ = 0.0F;  // 量化位置差分出来的速度（设备报这个）
    float position_ = 0.0F;          // 真实机械位置（仿真独有，连续量）
    float measuredPosition_ = 0.0F;  // 编码器读数（量化的位置，设备报这个）
    SetPid pid_{};

    // 速度估计用的位置窗口。存 (时刻, 量化后位置)，
    // 只保留 velocityWindowMs 内的点（至少两个）。
    struct PositionSample {
        std::uint64_t tMs = 0U;
        float positionDeg = 0.0F;
    };
    std::vector<PositionSample> positionWindow_;

    // PID 的内部状态。放在设备里而不是调用方：这是**控制器自己的记忆**，
    // 丢掉它等于每次都在重启控制器。
    double integralError_ = 0.0;
    float lastPidError_ = 0.0F;
    bool pidPrimed_ = false;   // 第一次调用不算微分：否则上电瞬间会有一个微分尖峰
    // 这一拍发给执行器的速度指令。PWM 由它算 —— 占空比是**设备自己知道**的量，
    // 拿测出来的速度去算占空比等于把"指令"和"结果"混为一谈。
    float lastCommand_ = 0.0F;

    bool hasTicked_ = false;
    std::uint64_t lastTickMs_ = 0U;
    std::uint64_t lastTelemetryMs_ = 0U;

    // 最近一次收到**任何 CRC 通过的帧**的时刻 —— F7 判据用的就是它。
    // 用"收到帧"而不是"收到字节"：字节流一直有噪声、但没有一帧能过 CRC，
    // 那不叫通信正常。
    bool hasReceivedFrame_ = false;
    std::uint64_t lastValidFrameMs_ = 0U;

    Telemetry lastTelemetry_{};
    std::vector<DedupEntry> recent_;
    std::vector<Nack> nacksSent_;
    std::uint16_t outboundSeq_ = 0U;
    std::uint64_t encodeFailures_ = 0U;
    std::uint64_t telemetryBacklogDropped_ = 0U;
};

}  // namespace mc
