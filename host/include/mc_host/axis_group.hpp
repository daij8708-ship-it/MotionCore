#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include <QObject>
#include <QString>
#include <QTimer>
#include <QVector>

#include "mc/axis_group.hpp"

#include "mc_host/device.hpp"
#include "mc_host/device_manager.hpp"

namespace mchost {

// ---------------------------------------------------------------------------
// 双轴协调（P1-7）：把两台**单轴**设备当一个组来用。
//
// 为什么是"两台单轴设备"而不是"一台双轴设备"：真机上多轴就是**多台驱动器** ——
// 各挂各的链路（本项目里各是一路串口 / 一个进程内模拟器），协议里每条命令也只认
// 一个轴。所以"轴组"是**上位机侧**的一层编排：协议不用改，设备侧甚至不需要知道
// "组"这件事。把多轴塞进协议才是把简单问题做复杂 —— 那要动帧格式、动遥测布局、
// 动去重窗口，而换来的是"设备必须理解组"，这没有必要的理由。
//
// 它做三件事：
//   ① **同步启动**：同一拍里按轴序把启动命令发出去（单线程下这就是"同时"的
//      全部含义 —— 两轴之间的偏差不超过一拍 10 ms，这一点必须如实说，不能说成"严格同步"）；
//   ② **同步到达**：按 `mc::planSynchronizedMove` 分配各轴速度（速度缩放式），
//      公共时长一到**自动全组停**。不停的话速度环会一直转下去；
//   ③ **全组安全闸**：任一轴进 Fault / 链路失效 → 立刻 STOP 全组，并记住原因。
//      这一条比"协调得准不准"重要得多：一台轴停了、另一台还在走，
//      对机械来说就是被拧了一下。
//
// ⚠️ 它**不是插补**。中间不约束两轴的轨迹关系，只保证同时出发、同时到达。
// ---------------------------------------------------------------------------
class AxisGroup : public QObject {
    Q_OBJECT

public:
    static constexpr std::size_t kAxisCount = 2U;
    // 轴组自己的节拍：只管两件事 —— "到点该停了吗"与"有轴出事了吗"。
    // 与 DeviceManager 的 tick 是**两个独立的时钟**，刻意不合并：
    // 合起来就得让设备管理器知道轴组，而轴组是它上面的东西。
    static constexpr int kGroupTickMs = 10;

    enum class Phase : std::uint8_t {
        Idle = 0U,      // 没有运动在进行
        Starting,       // 命令已发出，等应答
        Moving,         // 正在按计划走
        Done,           // 计划的那一段走完了，已发出全组停止
        Aborted,        // 中途被停下（人工停止 / 安全闸）
        Failed,         // 有轴被拒 / 超时 / 报错，已发出全组停止
    };

    struct Status {
        Phase phase = Phase::Idle;
        QString detail;
        double plannedDurationMs = 0.0;
        double elapsedMs = 0.0;
        QVector<CommandStatus> axisOutcomes;   // 与轴序一致；尚未结算时为 NotSent
        QString safetyDetail;                  // 安全闸触发的原因；没触发为空
    };

    explicit AxisGroup(QObject* parent = nullptr);
    ~AxisGroup() override;

    // 两轴都用进程内模拟器（没有硬件时唯一的对端）。
    void useSimulatedAxes();
    // 换某一轴的数据源（真串口）。与 DeviceManager 同一条姿态：只允许未连接时换。
    bool useSerialAxis(std::size_t index, const SerialTransport::Config& config, QString& error);

    DeviceManager& axis(std::size_t index);
    const DeviceManager& axis(std::size_t index) const;
    std::size_t axisCount() const noexcept { return kAxisCount; }

    bool connectAll();
    void disconnectAll();
    bool allReady() const;
    // 现在能不能开动：返回空 = 可以。它同时是 moveTo 的前置判据。
    QString startBlockReason() const;

    // 协调移动：两轴各自从**当前位置**走到给定目标。
    // 返回 false = **没有开动**，原因在 lastRefusalText() 里（不可用时的原因，
    // 或规划失败的那一句中文）。
    //
    // 注意它返回 true 只是"这一趟开始了"：真正的启动是**跨拍**推进的
    // （先切速度环、等设备报上新模式，再 START + 给速度），见 advanceStarting。
    bool moveTo(double targetXDeg, double targetYDeg, double maxSpeedDegPerSec);
    // 停下。**停不等于撤销**：已经走过的行程发生在过去，收不回来。
    bool stopAll();

    const Status& status() const noexcept { return status_; }
    // 最近一次的规划（失败时也保留，界面据此说明"为什么没开动"）。
    const mc::GroupMovePlan& lastPlan() const noexcept { return plan_; }
    // 只算不动：给界面做"按下之前先看看会怎么走"的预览用。
    mc::GroupMovePlan previewMove(double targetXDeg, double targetYDeg,
                                  double maxSpeedDegPerSec) const;
    QString lastRefusalText() const { return lastRefusal_; }
    QString phaseText() const;

signals:
    // 组状态有任何变化（阶段、结局、安全闸）。界面据此重画，而不是自己轮询猜。
    void groupChanged();

private slots:
    void onGroupTick();

private:
    // 把两轴一起切到速度环，并把状态推入 Starting。返回 false = 命令没发出去。
    //
    // 为什么不在这里顺手把 START 也发了：启动联锁里"已选控制模式"读的是
    // **设备回读回来的**模式（见 DeviceManager::interlockSnapshot），
    // 同一拍里连着发两条，第二条会被本地联锁当场拦下 —— 那时快照还没更新。
    // 所以启动必须跨拍：先切模式、等设备报上来，再 START + 给速度。
    bool beginStartSequence();
    // 推进 Starting 阶段：切模式 → 启动 → 给速度，每一步都等上一步真的落地。
    // 返回 true = 这一拍已经进入 Moving（可以开始结算目标那条命令并计时了）。
    bool advanceStarting();
    // 两轴是否都已经把指定模式报回来了（读的是设备状态，不是"我们发过了"）。
    bool allAxesInMode(std::uint8_t mode) const;
    // 某一轴"切模式"那条命令的结局。
    CommandStatus modeOutcome(std::size_t index) const;
    // 给每一轴发 STOP，返回"是不是每一条都发出去了"。
    bool stopAllAxes();
    void setPhase(Phase phase, const QString& detail);
    void settleOutcomes();
    bool anyAxisFaulted() const;
    bool anyAxisFailed() const;
    const CommandRecord* findCommand(const DeviceManager& manager, std::uint16_t seq) const;

    // 轴组统一用速度环：协调移动下发的是各轴的速度（见 mc::planSynchronizedMove）。
    static constexpr std::uint8_t kGroupMode = 1U;
    // 切模式 / 启动的兜底时限。会话就绪时模式应当几拍内就报回来；
    // 一直等不到说明对面不理我们了 —— 不能无声地挂在 Starting 上。
    static constexpr int kStartTimeoutTicks = 500;   // 5 s

    enum class StartStage : std::uint8_t { None = 0U, WaitMode, WaitTarget };

    std::unique_ptr<DeviceManager> axes_[kAxisCount];
    QTimer timeline_;
    Status status_;
    mc::GroupMovePlan plan_;
    QString lastRefusal_;
    StartStage startStage_ = StartStage::None;
    int startingTicks_ = 0;
    // "切模式"那两条命令的序号：被拒时要当场收摊，而不是干等到超时。
    std::uint16_t modeSeq_[kAxisCount] = {0U, 0U};
    // 每一轴"这一步要结算的那条命令"的序号。moveTo 是 SET_TARGET ——
    // 按 (seq, 名字) 一起匹配才不会认错人（心跳与命令共用同一个序号空间，
    // 只按 seq 会撞上）。
    std::uint16_t outcomeSeq_[kAxisCount] = {0U, 0U};
    double plannedDurationMs_ = 0.0;
    double elapsedMs_ = 0.0;
    bool safetyTripped_ = false;
};

}  // namespace mchost
