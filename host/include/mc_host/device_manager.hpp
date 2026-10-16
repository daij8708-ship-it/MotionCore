#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVector>

#include "mc_host/device.hpp"
#include "mc_host/telemetry_ring.hpp"

namespace mchost {

// 一条报警记录。故障报警页随后就用这份数据，所以在这里就带时刻 ——
// 事后追查时必须知道"什么时候出的事"，只留文本是查不出东西的。
struct AlarmRecord {
    double tMs = 0.0;
    QString text;
};

// ---------------------------------------------------------------------------
// 层 2（应用服务）
//
// 职责只有三件：
//   ① 用**定时器**驱动 `IDevice::tick()`（本层是唯一调用 tick 的地方）；
//   ② 把设备产出的遥测塞进层 5 的环形缓冲；
//   ③ 把核心层的事件翻译成 Qt 信号 + 报警记录。
//
// 它**不做协议判断**：命令能不能发、超时怎么处置、链路何时算断，全部在
// libmc_core 里已经判定过。这里再判一次就会出现第二套规则，迟早两边不一致。
//
// 刷新节流：tick 是 100 Hz，但标签密集的界面按 20 Hz 刷新（`statsRefreshed`）。
// 让 100 Hz 的数据去驱动 100 Hz 的界面重绘，是把 CPU 花在看不见的差别上。
// ---------------------------------------------------------------------------
class DeviceManager : public QObject {
    Q_OBJECT

public:
    explicit DeviceManager(QObject* parent = nullptr);
    ~DeviceManager() override;

    // 没注入设备时，构造一个进程内模拟设备（这样"什么都不配"也能跑起来）。
    // 注入只允许在未连接时做 —— 否则旧设备的链路没人关。
    void setDevice(std::unique_ptr<IDevice> device);

    IDevice* device() noexcept { return device_.get(); }
    const IDevice* device() const noexcept { return device_.get(); }

    const TelemetryRing& telemetry() const noexcept { return telemetry_; }

    bool isConnected() const noexcept { return connected_; }
    std::uint64_t ticks() const noexcept { return ticks_; }
    double elapsedMs() const noexcept { return elapsedMs_; }

    const QVector<AlarmRecord>& alarms() const noexcept { return alarms_; }

    // --- 计时诊断（默认关闭，只给压测工具用）---------------------------------
    //
    // 记两件事：**这一拍占住调用线程多久**（busy）与**两拍之间实际隔了多久**
    // （interval）。前者回答"UI 线程够不够用"，后者回答"定时器准不准"。
    //
    // 默认关闭是因为它本身有成本（每拍两次 QElapsedTimer 读 + 一次写）——
    // 常开等于让被测对象替我们分担测量误差。百分位不在产品代码里算：
    // 这里只如实记样本，怎么统计是测量端的事。
    void enableTimingDiagnostics(bool enabled, std::size_t capacity = 8192U);
    bool timingDiagnosticsEnabled() const noexcept { return timingEnabled_; }
    // 有效样本数 = min(已记拍数, 容量)：报告里能看到的就只有这么多，
    // 不能给一个"看起来在涨、其实已经绕圈覆盖"的数。
    std::size_t timingSampleCount() const noexcept {
        return (timingSampleCount_ < timingCapacity_) ? timingSampleCount_ : timingCapacity_;
    }
    const QVector<qint64>& tickBusyUs() const noexcept { return tickBusyUs_; }
    const QVector<qint64>& tickIntervalUs() const noexcept { return tickIntervalUs_; }

    // 每拍固定步长（毫秒）。与设备的遥测周期一致 → 100 Hz。
    static constexpr int kTickIntervalMs = 10;
    // 界面刷新分频：每 5 拍刷一次 = 20 Hz。
    static constexpr int kUiRefreshDivider = 5;

public slots:
    void connectDevice();
    void disconnectDevice();

    // 清空实时缓冲（只清元素，不清"因覆盖而丢弃"的累计计数 ——
    // 否则点一下"清屏"就把丢帧统计抹了，等于自己把证据删掉）。
    void clearTelemetry();

    // 命令。返回 false 表示**没有发出去**（未连接 / 会话未就绪）。
    bool sendSetMode(int mode);
    bool sendStart();
    bool sendStop();
    bool sendSetTarget(double target);
    bool sendSetPid(double kp, double ki, double kd);

    // 故障注入：用来在**没有硬件**的情况下演示断链、重连、心跳与故障保护。
    // 打的正是测试里那套注入，不是另做一套。
    void injectUplinkDrop(bool enabled);
    void injectDownlinkDrop(bool enabled);
    void injectUplinkLatency(int polls);

signals:
    void connectionChanged(bool connected);
    // 20 Hz 的界面刷新信号（不是每拍都发）。
    void statsRefreshed();
    void commandLogChanged();
    void alarmRaised(const QString& text);

private slots:
    void onTick();

private:
    void raiseAlarm(const QString& text);
    // 一拍的全部工作。计时外壳只负责"什么时候测"，不掺进业务。
    void tickOnce();
    void recordTimingSample(qint64 busyUs, qint64 intervalUs);

    std::unique_ptr<IDevice> device_;
    TelemetryRing telemetry_;
    QTimer timer_;
    QElapsedTimer clock_;
    // 单独一个计时器：clock_ 给的是"连接以来的毫秒数"（仿真时钟），
    // 而拍耗时要用纳秒级的单调读数，两者用途不同，混用会把时钟精度当成了精度上限。
    QElapsedTimer timingClock_;

    bool connected_ = false;
    std::uint64_t ticks_ = 0U;
    double elapsedMs_ = 0.0;

    bool timingEnabled_ = false;
    QVector<qint64> tickBusyUs_;
    QVector<qint64> tickIntervalUs_;
    std::size_t timingCapacity_ = 0U;
    std::size_t timingSampleCount_ = 0U;
    qint64 lastTimingTickMs_ = -1;

    bool lastFaultLatched_ = false;
    QVector<AlarmRecord> alarms_;
    static constexpr int kMaxAlarms = 200;
};

}  // namespace mchost
