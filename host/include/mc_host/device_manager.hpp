#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVector>

#include "mc/alarm.hpp"
#include "mc/audit.hpp"
#include "mc/interlock.hpp"
#include "mc/log.hpp"
#include "mc/recipe.hpp"
#include "mc/sequence.hpp"

#include "mc_host/device.hpp"
#include "mc_host/history_store.hpp"
#include "mc_host/recipe_store.hpp"
#include "mc_host/sequence_store.hpp"
#include "mc_host/telemetry_ring.hpp"

namespace mchost {

// 一条报警记录（分级、代码、确认状态）现在住在核心库的 `mc::AlarmLog` 里。
// 理由不是"分层好看"：**"未确认的报警阻止启动"是一条规则**，
// 规则写在控件里就只能靠人眼看，写在这里才能被穷举验证 —— 而且将来固件侧
// 要问同一个问题（"现在允许启动吗"），那时它也得有一份同样的判据。

// 一次配方下装进行到哪儿了。面向界面：只讲"做到哪一步、结果如何"。
enum class RecipeDownloadPhase : std::uint8_t {
    Idle = 0U,     // 没有进行中的下装，也不记得上一次的结果
    Sending,       // 正在逐条下发（要不要重传由协议层决定，这里只负责推进）
    Verifying,     // 三条都被确认了，正等一帧**新**遥测来核对回读的 target
    Succeeded,
    Failed,
};

struct RecipeDownloadStatus {
    RecipeDownloadPhase phase = RecipeDownloadPhase::Idle;
    int commandsAcked = 0;   // 0..3：三条**配置命令**里已确认的条数（回读那一条另算）
    QString detail;          // 在做什么 / 结束原因（含设备给的拒绝原因）
    mc::Recipe recipe;       // 正在（或刚）下装的配方
};

// 回零进行到哪儿了。**回零的成功不由应答决定**：应答只说明设备接受了，
// 到位与否是设备侧的一段流程，只能看状态（Home → Ready / Fault）。
// 所以这里是一台小状态机，而不是一个"成功/失败"的布尔量。
enum class HomePhase : std::uint8_t {
    Idle = 0U,     // 没在回零，也不记得上一次
    Started,       // 已发出并被确认，设备正在朝零点走
    Succeeded,     // 设备从 Home 回到了 Ready：零点**已经建立**
    // 被 STOP / 中止打断。**零点没有建立** —— 这一格必须与 Succeeded 分开：
    // 两者在曲线页上都表现为"停下来了"，含义却完全相反。
    Aborted,
    Failed,        // 走不到零点（超时/故障），设备进了 Fault
};

struct HomeStatus {
    HomePhase phase = HomePhase::Idle;
    QString detail;
    double elapsedMs = 0.0;   // 本次回零已经走了多久（结束之后是总时长）
    double speedDegPerSec = 0.0;
    // 这一趟的时限（0 = 不是本机发起的）。界面据此显示"走到哪儿了"，
    // 也是"设备一直没进回零态"时那条兜底判据的判据。
    std::uint32_t timeoutMs = 0U;
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
    // 三个库（历史 / 配方 / 程序）要不要在构造时就打开。
    //
    // 默认打开，落在用户数据目录下。轴组的从动轴用 `Disabled` —— 它们只是
    // "又一台驱动器"：各写一份历史没有意义，更**不能**去开同一份用户库
    // （那会把轴组的数据混进设备的运行记录里，事后谁也分不清哪些是实际运行产生的）。
    enum class StorePolicy : std::uint8_t {
        OpenDefaultPaths = 0U,
        Disabled = 1U,
    };

    explicit DeviceManager(QObject* parent = nullptr);
    explicit DeviceManager(StorePolicy policy, QObject* parent = nullptr);
    ~DeviceManager() override;

    // 没注入设备时，构造一个进程内模拟设备（这样"什么都不配"也能跑起来）。
    // 注入只允许在未连接时做 —— 否则旧设备的链路没人关。
    void setDevice(std::unique_ptr<IDevice> device);

    // 换数据源（系统设置页用）。**只在未连接时**允许：已连接时换设备会把
    // 一条正在跑的链路的关闭责任丢掉。返回 false = 没换（原因写进 error）。
    bool useSimulatedDevice();
    bool useSerialDevice(const SerialTransport::Config& config, QString& error);
    IDevice* device() noexcept { return device_.get(); }
    const IDevice* device() const noexcept { return device_.get(); }

    const TelemetryRing& telemetry() const noexcept { return telemetry_; }

    // --- 历史落库（阶段 7）------------------------------------------------------
    // 默认落在用户数据目录（见 historyPath()）。落库失败**不影响采集**：
    // 实时界面不该因为磁盘写不进去就停摆；但历史页必须把"没在落库"显示出来。
    bool openHistory(const QString& dbPath);
    bool historyOpen() const noexcept { return history_ != nullptr && history_->isOpen(); }
    HistoryStore* history() noexcept { return history_.get(); }
    static QString defaultHistoryPath();

    // --- 配方库（P0）------------------------------------------------------------
    // 与历史库同一个姿态：打不开只是"没有配方库"，不是"不能用"。
    bool openRecipes(const QString& dbPath);
    bool recipesOpen() const noexcept { return recipes_ != nullptr && recipes_->isOpen(); }
    RecipeStore* recipes() noexcept { return recipes_.get(); }
    static QString defaultRecipePath();

    // --- 配方下装 ---------------------------------------------------------------
    //
    // 把一份配方下发到设备：SET_MODE → SET_PID → SET_TARGET，三条都被确认之后，
    // 再拿一帧**新**遥测核对回读的 target。
    //
    // 顺序不是随意的：SET_MODE 决定 target 的**单位语义**（deg/s 还是 deg），
    // 所以模式必须先落地 —— 先发目标值的话，那一段时间里"200 是什么意思"说不清。
    //
    // 返回 false = 这次下装**没有开始**，原因在 recipeDownload().detail 里
    // （此时 phase 已经是 Failed）。已经开始的下装是异步的，结果同样从
    // recipeDownload() 读，另有 recipeDownloadChanged() 通知进度。
    bool sendRecipe(const mc::Recipe& recipe);
    void cancelRecipeDownload();
    const RecipeDownloadStatus& recipeDownload() const noexcept { return recipeStatus_; }

    bool isConnected() const noexcept { return connected_; }
    std::uint64_t ticks() const noexcept { return ticks_; }
    double elapsedMs() const noexcept { return elapsedMs_; }

    // --- 程序库（P0）------------------------------------------------------------
    // 与历史库、配方库同一个姿态：打不开只是"没有程序库"，不是"不能用"。
    bool openSequences(const QString& dbPath);
    bool sequencesOpen() const noexcept { return sequences_ != nullptr && sequences_->isOpen(); }
    SequenceStore* sequences() noexcept { return sequences_.get(); }
    static QString defaultSequencePath();

    // --- 自动任务调度（P0）------------------------------------------------------
    //
    // 引擎住在核心库（`mc::Sequencer`），这里只做**编排**：把引擎要发的那一步
    // 真发出去、把设备给的结局报回去。判定规则（一步在飞、不确定就停、
    // 运动指令不重发）全部由引擎判定，这里不复制第二份。
    //
    // 启动门禁在这一层，因为它只有在这里才齐全：报警未确认、会话是否就绪、
    // 设备停在哪一个状态 —— 引擎不认识设备，判不了这些。
    //
    // 返回 false = **没有启动**，原因在 sequenceStartError() 里（人话）。
    bool startSequence(const mc::SequenceProgram& program);
    // 中止。**中止不等于撤销**：已经下发并被确认的命令发生在过去，收不回来，
    // 设备保持在那时的状态。界面必须把这句话说出来。
    // 返回 false = 没停下（没权限 / 本来就没在跑），原因在 lastRefusalText()。
    bool abortSequence();
    const mc::Sequencer& sequencer() const noexcept { return sequencer_; }
    QString sequenceStartError() const { return sequenceStartError_; }
    // 上一次运行留下的每一步流水（给人看，也是测试的判据）。
    const std::vector<mc::StepReport>& sequenceReports() const noexcept {
        return sequencer_.reports();
    }

    // 报警簿。**它是规则，不是显示效果**：分级、确认状态、"未确认就拦启动"
    // 都由核心库判定，这里只负责持有并把它接到设备事件上。
    const mc::AlarmLog& alarms() const noexcept { return alarms_; }
    std::size_t unacknowledgedAlarms() const noexcept { return alarms_.unacknowledgedRequiringAck(); }

    // --- 启动联锁（P0）----------------------------------------------------------
    //
    // 「启动之前必须成立什么」列成一张表，一次判完，把没满足的**全部**列出来。
    // 规则表与判定住在核心库（`mc::InterlockTable`），这里只做两件事：
    // 把现场如实填进快照、把判定结果说成人话。
    //
    // ⚠️ 这是**软件**联锁。它代替不了任何物理安全回路 —— 急停、限位开关、
    //    安全继电器都在电气回路上，与这里无关。
    mc::InterlockSnapshot interlockSnapshot() const;
    // 一条规则 + "现在这条对这台设备意味着什么"。
    struct InterlockView {
        std::uint16_t code = 0U;
        mc::InterlockSeverity severity = mc::InterlockSeverity::Block;
        bool satisfied = false;
        QString text;   // 满足与不满足都有一条能读懂的话
    };
    std::vector<InterlockView> interlockView() const;
    // 现在允不允许启动：返回空 = 各条拦启动的规则都满足。
    // 它就是 `sendStart()` / `startSequence()` 用的那一条判据（**唯一一条**）。
    //
    // programOwnsMode = true 只有自动程序会传：程序自己会走"设模式"这一步，
    // 所以 MODE_SELECTED 对它不是前置条件 —— 它此刻当然还没选环，选环是它第一步要做的事。
    // 摘掉的**只有这一条**：程序不该比人手多过一道闸，也不该少过一道。
    QString startBlockReason(bool programOwnsMode = false) const;

    // --- 操作员权限与操作审计（P1）----------------------------------------------
    //
    // 「谁能做什么」与「谁在什么时候试过什么」这两件事都在核心库
    // （`mc::OperatorRole` / `mc::AuditLog`），这一层只做两件事：
    // 把当前角色接上、把每一次尝试如实登记（**包括被拒的**）。
    //
    // ⚠️ 这是**软件**权限：它挡的是误操作，不是人。能改数据库文件的人就能改流水，
    //    真正的准入控制靠操作系统账户与物理钥匙 —— 设置页上必须把这句话写出来。
    mc::OperatorRole operatorRole() const noexcept { return role_; }
    // 换角色。**它自己也过权限、也进审计** —— 这是最该留下记录的一条：
    // "谁把自己升成了工程师"。
    bool setOperatorRole(mc::OperatorRole role);
    // 出厂默认角色。见 setOperatorRole 的说明：默认值是**看得见**的
    // （设置页上写着、每次操作都带进流水），不是一条藏起来的后门。
    static constexpr mc::OperatorRole kDefaultRole = mc::OperatorRole::Engineer;

    const mc::AuditLog& audit() const noexcept { return audit_; }
    static constexpr std::size_t kMaxAuditRows = mc::AuditLog::kDefaultCapacity;
    // 上一条流水的单行描述；还没有流水时返回空。
    QString lastAuditText() const;
    // 最近一次被拒的操作为什么被拒。**同一个回合里读**：`sendX()` 返回 false 之后
    // 立刻读它，读到的就是那一次的原因（界面就是这么用的，不跨回合）。
    // 它把三类拒绝（没权限 / 没连上·会话没就绪 / 联锁没满足）收成一句话，
    // 页面不必自己去分辨是哪一类。
    QString lastRefusalText() const { return lastRefusal_; }

    // --- 库的写操作（配方 / 程序）------------------------------------------------    //
    // 页面**不许**直接拿 store 去改：配方与程序是"这台设备能干什么"的定义，
    // 谁来改、什么时候改的，事后必须查得出来 —— 所以写入口收在这一层，
    // 权限与审计才有唯一的一处。
    bool saveRecipe(const mc::Recipe& recipe, qint64* outId, QString& error);
    bool deleteRecipe(qint64 id, QString& error);
    bool saveSequence(const mc::SequenceProgram& program, qint64* outId, QString& error);
    bool deleteSequence(qint64 id, QString& error);

    // --- 回零与软限位（P1-6）----------------------------------------------------
    //
    // 回零是**设备侧的一段流程**（零点由设备定义），这一层只做两件事：
    // 发那条命令、把设备随后走的状态如实翻译成"到没到位"。
    // 过的是和 START 同一道闸（见 startBlockReason）：它同样是"从静止开始让电机动"。
    bool sendHome(const mc::HomeParam& param);
    const HomeStatus& homeStatus() const noexcept { return homeStatus_; }
    static constexpr double kDefaultHomeSpeedDegPerSec = 60.0;

    // 软限位。**权威在设备**：这里存的是"设备最近一次回读回来的那一组"，
    // 界面上据此显示与预判；真正的拦截由设备做（它才是那个能让电机停下的）。
    bool sendSoftLimits(const mc::SoftLimits& limits);
    // 设备**实际存着**的软限位。返回 false = 还没回读过，此时它给出的值没有意义 ——
    // 调用方必须把这个"不知道"显示出来，而不是拿一组 0 当真值用。
    bool deviceSoftLimits(mc::SoftLimits& out) const;
    // 回读设备**实存**的整组参数（模式 / 目标 / 三个增益 / 软限位）。
    // 返回 false = 还没回读过，out 原封不动。
    //
    // 为什么不按"操作"审计、也不做权限检查：它**什么都不改变**。往里塞一道权限门
    // 只会让一个只读动作在低权限下静默失败，而失败的样子与"设备没回话"一模一样。
    bool deviceParamState(mc::ParamState& out) const;
    // 发起一次回读。**没有本地前置条件以外的门禁**：读取不改变设备状态，
    // 唯一的前提是链路上发得出去。
    bool requestParamReadback();

    // --- 运行日志（P1-8）--------------------------------------------------------
    //
    // 日志**不持有**：它由应用程序建一条（一个进程一条日志流），设备管理器只是
    // 往里写。这样换去处（写文件 / 只留内存 / 测试里用一个假的）不用动这一层。
    //
    // ⚠️ 日志与报警、审计是三件事：报警要人处理、审计要追责、日志是排查用的。
    //    这里写日志**不影响**那两个 —— 报警照旧要确认，审计照旧要落库，
    //    改成 Error 级也不会多出一条报警。反过来更关键：把日志调到"只记错误"
    //    也不会让任何一条报警消失。
    void setLogger(mc::Logger* logger) noexcept { logger_ = logger; }
    mc::Logger* logger() const noexcept { return logger_; }

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
    // 报警清单上限。是**有界**的：长时间运行的界面不能靠"反正没人看"来无限增长。
    // 公开它，是为了让报警页能如实写出"最多保留多少条"，而不是自己抄一个数字。
    // 上限策略在 AlarmLog 里 —— 那里还管着"不许挤掉未确认的拦启动报警"。
    static constexpr std::size_t kMaxAlarms = mc::AlarmLog::kDefaultCapacity;

public slots:
    bool connectDevice();
    bool disconnectDevice();

    // 清空实时缓冲（只清元素，不清"因覆盖而丢弃"的累计计数 ——
    // 否则点一下"清屏"就把丢帧统计抹了，等于自己把证据删掉）。
    void clearTelemetry();

    // 清空**报警清单**。注意它只清这份记录，**不碰设备上的故障锁存** ——
    // 两件事差着一个数量级：一个是把屏幕擦干净，一个是让设备重新允许启动。
    // 合成一个按钮就是安全设计上的错误。
    bool clearAlarms();
    // 人工确认一条 / 全部确认。确认的意思是"我看见了"，
    // **不代表物理原因已经排除** —— 那件事只有现场的人能判断。
    bool acknowledgeAlarm(std::uint16_t code);
    bool acknowledgeAllAlarms();

    // 命令。返回 false 表示**没有发出去**，原因在 lastRefusalText() 里
    // （没权限 / 未连接 / 会话未就绪 / 联锁没满足 —— 三类拒绝收在同一处）。
    bool sendSetMode(int mode);
    bool sendStart();
    bool sendStop();
    bool sendSetTarget(double target);
    bool sendSetPid(double kp, double ki, double kd);
    bool sendClearFault(mc::FaultCode code);

    // 故障注入：用来在**没有硬件**的情况下演示断链、重连、心跳与故障保护。
    // 打的正是测试里那套注入，不是另做一套。
    //
    // 参数化接口（0 = 关闭），而且这里是**唯一**的设置处：注入状态只有一个真源。
    // 两处按钮改同一个状态，迟早会出现"界面显示断着、实际没断"—— 而这类误判
    // 恰好会让人得出"断链重连失效"这种反向结论。诊断页拿到的必须是真相。
    void setUplinkDropEveryNthByte(int n);
    void setDownlinkDropEveryNthByte(int n);
    void setUplinkLatencyPolls(int n);
    void clearInjectedFaults();

signals:
    void connectionChanged(bool connected);
    // 20 Hz 的界面刷新信号（不是每拍都发）。
    void statsRefreshed();
    void commandLogChanged();
    // 新出一条报警（带代码与算好的文本，状态栏可以直接用）。
    void alarmRaised(std::uint16_t code, const QString& text);
    // 报警簿发生了任何变化（新增、确认、标记已恢复、清清单）：
    // 界面据此重建表格，而不是自己猜。
    void alarmsChanged();
    // 配方下装的进度或结果变了（每次推进一格、以及结束时各发一次）。
    void recipeDownloadChanged();
    // 任务调度的状态变了（每一步的推进、结束、中止）。
    void sequenceChanged();
    // 一次操作被拒（没权限 / 没连上 / 联锁没满足）。带的是**完整的句子**，
    // 状态栏可以直接显示。页面自己有结果标签的，就显示在自己的标签上。
    void operationRefused(const QString& text);
    // 审计流水变了（新增一条）。界面据此重画那一张表，而不是自己猜。
    void auditChanged();
    // 回零的进度或结果变了。
    void homeChanged();

private slots:
    void onTick();

private:
    void raiseAlarm(std::uint16_t code, const QString& detail = QString());

    // 写一条日志。没挂日志器时是空操作 —— 让每个调用点都写 `if (logger_)`
    // 等于把"忘了判空"这件事留给每一处。
    void logEvent(mc::LogLevel level, const char* tag, const QString& text);

    // --- 权限与审计的统一入口 ---------------------------------------------------
    //
    // 每个会产生后果的公开接口都走这三步，一步都不许跳：
    //   permitted() → 干活 → audited()
    // 或者在中途用 refuse() 返回。**审计只有一处写入点**：分散着写，
    // 迟早会出现"某条路径忘了记"，而漏记的恰恰是没人测到的那条路径。
    //
    // 判定顺序也是定死的：**权限在前**。"你没这个权限"比"设备没连上"更根本 ——
    // 反过来先报链路问题，会让人以为"连上就能按了"。
    bool permitted(mc::OperatorAction action, const QString& subject);
    bool refuse(mc::OperatorAction action, const QString& subject, const QString& reason);
    void audited(mc::OperatorAction action, const QString& subject, bool dispatched,
                 const QString& notDispatchedReason = QString());
    void recordAudit(mc::OperatorAction action, const QString& subject, bool dispatched,
                     const QString& reason);

    // 把某一条报警的**当前状态**同步进历史库。raise 的时候顺带写了一次，
    // 但"已恢复""已确认"是后来才发生的，那时得再写一次 —— 否则库里留下的是
    // 一份过期状态，而事后追查恰恰看的就是那份。
    void syncAlarmToHistory(std::uint16_t code);
    // 未确认报警里最严重的那一条（给联锁的说明用）。没有就返回空。
    QString worstUnacknowledgedAlarmText() const;
    // 一拍的全部工作。计时外壳只负责"什么时候测"，不掺进业务。
    void tickOnce();
    void recordTimingSample(qint64 busyUs, qint64 intervalUs);

    // --- 配方下装的推进（每拍调一次）--------------------------------------------
    // 命令能不能发、超时怎么算，全部由协议层判定；这里只做**编排**：
    // 看当前这条的结局，决定是推进、等待还是中止。判定规则不复制第二份。
    void driveRecipeDownload();
    // 发下一条命令。返回 false = 本地就没发出去（记录里没有可用的 seq）。
    bool startRecipeStep();
    void failRecipeDownload(const QString& reason);
    void succeedRecipeDownload();
    // 找到某条命令在本拍日志里的记录（按 (seq, 名字) 一起匹配）。
    const CommandRecord* findRecipeCommand() const;

    // 回零的推进（每拍调一次）：看设备状态与那条 HOME 的结局，判定这一趟走到哪一步了。
    // 实现在实现文件里，和 sendHome 放在一起。
    const CommandRecord* findHomeCommand() const;

    // --- 任务调度的推进（每拍调一次）--------------------------------------------
    // 顺序很重要：先推进引擎（Wait 步到点、摆出该发的步），再结算**上一条**命令的
    // 结局，最后发**新摆出来的**那一步。结算必须在发送之前 —— 否则同一拍里
    // 刚发出去的命令会被当成"已结清"，而那正是"一步在飞"这条规矩要防的事。
    void driveSequence();
    // 把引擎摆出来的那一步真发出去，并告诉它发没发成功。
    bool dispatchSequenceStep();
    // 上一条命令的结局到了没有；到了就报回去。
    bool reportSequenceOutcome();
    void setSequenceStartError(const QString& reason);

    // 回零的推进（每拍调一次）：只看设备状态，不改设备。
    // 它**不驱动任何命令** —— 回零是设备自己走完的，上位机在这里是观察者。
    void driveHoming();

    std::unique_ptr<IDevice> device_;
    TelemetryRing telemetry_;
    std::unique_ptr<HistoryStore> history_;
    std::unique_ptr<RecipeStore> recipes_;
    std::unique_ptr<SequenceStore> sequences_;
    QTimer timer_;
    QElapsedTimer clock_;
    // 单独一个计时器：clock_ 给的是"连接以来的毫秒数"（仿真时钟），
    // 而拍耗时要用纳秒级的单调读数，两者用途不同，混用会把时钟精度当成了精度上限。
    QElapsedTimer timingClock_;

    bool connected_ = false;
    std::uint64_t ticks_ = 0U;
    double elapsedMs_ = 0.0;

    mc::Logger* logger_ = nullptr;
    // 每一条命令上一次被写进日志时的归宿。日志只在**变化**的那一刻记一次 ——
    // 命令结算是按拍把整本日志过一遍的，不过滤就会把同一件事写几百遍。
    std::map<std::uint16_t, CommandStatus> loggedCommandStatus_;

    bool timingEnabled_ = false;
    QVector<qint64> tickBusyUs_;
    QVector<qint64> tickIntervalUs_;
    std::size_t timingCapacity_ = 0U;
    std::size_t timingSampleCount_ = 0U;
    qint64 lastTimingTickMs_ = -1;

    bool lastFaultLatched_ = false;
    // 只在**跳变沿**上记一次链路失效 / 记一次恢复：链路是一段状态，
    // 而报警记的是"发生了一件事"。按拍记会把 occurrences 刷成几百次。
    bool lastLinkDown_ = false;
    bool lastSessionReady_ = false;
    // 当前锁存着的设备故障对应的报警代码。降沿时要用它去标"已恢复"——
    // 那一刻设备已经把 faultCode 清成 0 了，从设备身上问不出"刚才出的是哪一条"。
    std::uint16_t latchedAlarmCode_ = 0U;
    mc::AlarmLog alarms_;

    // --- 操作员权限与审计 -------------------------------------------------------
    mc::OperatorRole role_ = kDefaultRole;
    mc::AuditLog audit_;
    QString lastRefusal_;

    // --- 回零 -------------------------------------------------------------------
    // 只存"开始那一刻"的现场，加上当前状态。**终点由设备状态判定**：
    // 上位机不自己算"到位了没有" —— 那需要知道零点在哪，而那件事只有设备知道。
    HomeStatus homeStatus_;
    double homeStartedMs_ = 0.0;
    bool homeSawDeviceHoming_ = false;   // 见过设备进 Home 状态没有
    // 本机发出的那条 HOME 的序号（0 = 这一趟不是本机发起的，例如程序里的回零步）。
    // 它决定"终点怎么判"：命令自己就被拒/超时了，结局该用**命令那条**说；
    // 命令被确认了而设备已经不在回零态，则这一趟走完了。
    std::uint16_t homeSeq_ = 0U;

    // 配方下装的进行态。与 recipeStatus_ 分开：status 是**给人看的**，
    // 这几个是编排自己需要的（当前等下一条命令的结局、核对用的基线时刻）。
    RecipeDownloadStatus recipeStatus_;
    int recipeStep_ = 0;                       // 0..2 = 三条配置命令；3 = 回读
    std::uint16_t recipeSeq_ = 0U;             // 当前这一步的命令序号
    const char* recipeStepName_ = nullptr;     // 当前这一步的命令名（与 seq 一起匹配）
    std::uint64_t recipeParamBaseline_ = 0U;        // 发回读请求时已收到的回读条数
    std::uint64_t recipeParamRejectBaseline_ = 0U;  // 发回读请求时已判为坏数据的条数

    // 任务调度的引擎与它自己需要的编排状态。
    mc::Sequencer sequencer_;
    QString sequenceStartError_;
    // 当前这一步对应的命令行序号与名字。找到它才能知道结局 ——
    // 与配方下装同一套做法：按 (seq, 名字) 一起匹配，只按 seq 会认错人
    // （心跳与命令共用同一个序号空间）。
    // seq == 0 表示"现在没有在飞的那一步"，结局已经报过了 —— 它同时就是
    // "不许报第二遍"的标记，不再另设一个布尔量去和它保持同步。
    std::uint16_t sequenceSeq_ = 0U;
    const char* sequenceStepName_ = nullptr;
};

}  // namespace mchost
