#include "mc_host/device_manager.hpp"

#include <cmath>
#include <utility>

#include <QDateTime>
#include <QDir>
#include <QStandardPaths>

#include "mc/device_state.hpp"
#include "mc_host/alarm_text.hpp"
#include "mc_host/audit_text.hpp"
#include "mc_host/interlock_text.hpp"
#include "mc_host/log_text.hpp"
#include "mc_host/recipe_text.hpp"
#include "mc_host/sequence_text.hpp"

namespace mchost {
namespace {

// 回读核对的容差。设备存下来的就是我们发过去的那个 float（线上就是 float32），
// 所以理论上应当逐位相等；留 1e-3 是为了不在"两处各自格式化后看着差一位"上误报。
// 它**不是**"允许有偏差"的余量：真出现偏差，这里就该红。
constexpr float kRecipeReadBackTolerance = 1.0e-3F;

}  // namespace

// --- 权限与审计的统一入口 -----------------------------------------------------

bool DeviceManager::permitted(mc::OperatorAction action, const QString& subject) {
    if (mc::roleHasPermission(role_, mc::requiredPermission(action))) {
        return true;
    }
    const QString reason = permissionRefusalText(role_, action);
    recordAudit(action, subject, false, reason);
    lastRefusal_ = reason;
    emit operationRefused(reason);
    return false;
}

bool DeviceManager::refuse(mc::OperatorAction action, const QString& subject,
                           const QString& reason) {
    // 权限是放行的，但这一步**没做**（没连上、联锁没满足、参数不合法……）。
    // 它同样要进流水：流水回答的是"有没有真的发生"，而"试了但没发生"
    // 恰恰是排查误操作时最要看的一类。
    recordAudit(action, subject, false, reason);
    lastRefusal_ = reason;
    emit operationRefused(reason);
    return false;
}

void DeviceManager::audited(mc::OperatorAction action, const QString& subject, bool dispatched,
                            const QString& notDispatchedReason) {
    recordAudit(action, subject, dispatched, notDispatchedReason);
}

void DeviceManager::logEvent(mc::LogLevel level, const char* tag, const QString& text) {
    if (logger_ == nullptr) {
        return;
    }
    logger_->log(level, tag, text.toStdString());
}

void DeviceManager::recordAudit(mc::OperatorAction action, const QString& subject, bool dispatched,
                                const QString& reason) {
    const qint64 wallMs = QDateTime::currentMSecsSinceEpoch();
    audit_.record(static_cast<std::uint64_t>(wallMs),
                  static_cast<std::uint64_t>(elapsedMs_), role_, action, subject.toStdString(),
                  dispatched, reason.toStdString());
    // 审计与日志**同时**写，但落点完全不同：审计进内存环形 + 数据库（长期、可追责），
    // 日志进级别过滤的那条流（可随时丢、用来排查）。这里顺手写一条日志，
    // 是为了让日志里也能看出"是谁按的这一个按钮" —— 否则排查时日志只说
    // "命令被拒了"，却不说是谁在按。
    if (!dispatched) {
        // 被拒的走 Warn：它在日志里必须显眼，但**不是**报警（不拦启动、不需要确认）。
        logEvent(mc::LogLevel::Warn, "audit",
                 QStringLiteral("%1 「%2」未执行：%3")
                     .arg(operatorActionText(action), subject, reason));
    } else {
        logEvent(mc::LogLevel::Debug, "audit",
                 QStringLiteral("%1 「%2」").arg(operatorActionText(action), subject));
    }
    if (history_ != nullptr) {
        // 与内存里那本流水记的是同一件事。库里那份是**长期的**：
        // 重启之后内存里那条就没了，而"上个月是谁改的配方"只能从库里查。
        history_->addAudit(static_cast<std::uint64_t>(elapsedMs_), wallMs,
                           static_cast<int>(role_), static_cast<int>(action), subject,
                           dispatched, reason);
    }
    emit auditChanged();
}

QString DeviceManager::lastAuditText() const {
    const mc::AuditEntry* entry = audit_.latest();
    if (entry == nullptr) {
        return QString();
    }
    return auditSentence(*entry);
}

bool DeviceManager::setOperatorRole(mc::OperatorRole role) {
    // 换角色本身也过权限：一个观察者不该能把自己升上去。
    // 用的是**切换后**的角色去判 —— 先把 role_ 改掉再判会留下一个
    // "已经被改掉、但登记为被拒"的状态，那种状态比不记录更坏。
    if (!permitted(mc::OperatorAction::SetRole, operatorRoleText(role))) {
        return false;
    }
    const mc::OperatorRole previous = role_;
    role_ = role;
    audited(mc::OperatorAction::SetRole, QStringLiteral("%1 → %2")
                                             .arg(operatorRoleText(previous))
                                             .arg(operatorRoleText(role)),
            true);
    return true;
}

// --- 库的写操作（配方 / 程序）--------------------------------------------------

bool DeviceManager::deviceSoftLimits(mc::SoftLimits& out) const {
    if (device_ == nullptr || !device_->hasParamState()) {
        return false;
    }
    out = device_->lastParamState().limits;
    return true;
}

bool DeviceManager::deviceParamState(mc::ParamState& out) const {
    if (device_ == nullptr || !device_->hasParamState()) {
        return false;
    }
    out = device_->lastParamState();
    return true;
}

bool DeviceManager::requestParamReadback() {
    if (!connected_ || device_ == nullptr) {
        lastRefusal_ = QStringLiteral("还没连上设备");
        return false;
    }
    if (!device_->readParams()) {
        lastRefusal_ = QStringLiteral("本地没能发出去（会话未就绪或链路已失效）");
        return false;
    }
    emit commandLogChanged();
    return true;
}

bool DeviceManager::sendSoftLimits(const mc::SoftLimits& limits) {
    const QString subject = limits.enabled
                                ? QStringLiteral("软限位 %1 ~ %2 deg")
                                      .arg(limits.minDeg, 0, 'f', 1)
                                      .arg(limits.maxDeg, 0, 'f', 1)
                                : QStringLiteral("软限位：关闭");
    if (!permitted(mc::OperatorAction::SetLimits, subject)) {
        return false;
    }
    if (!connected_ || device_ == nullptr) {
        return refuse(mc::OperatorAction::SetLimits, subject, QStringLiteral("还没连上设备"));
    }
    // 结构合法性先判一遍：不合法的值发出去只会换来一条 BadParameter，
    // 而那条 NAK 在界面上说不清"是哪一项不合法"。
    if (!mc::isSoftLimitsValid(limits)) {
        return refuse(mc::OperatorAction::SetLimits, subject,
                      QStringLiteral("限位本身不合法：最小值必须小于最大值，而且两者都要是有限的"
                                     "（min > max 的限位一打开就是「哪里都不许去」）"));
    }
    const bool sent = device_->setLimits(limits);
    audited(mc::OperatorAction::SetLimits, subject, sent,
            sent ? QString() : QStringLiteral("本地没发出去（会话未就绪或链路已失效）"));
    emit commandLogChanged();
    return sent;
}

bool DeviceManager::sendHome(const mc::HomeParam& param) {
    const QString subject = QStringLiteral("回零（速度 %1 deg/s，时限 %2 s）")
                                .arg(param.speedDegPerSec, 0, 'f', 1)
                                .arg(param.timeoutMs / 1000.0, 0, 'f', 1);
    if (!permitted(mc::OperatorAction::Home, subject)) {
        return false;
    }
    // 回零是"从静止开始让电机动起来"，与 START 走**同一道闸**。
    // 联锁表里那些条件（链路、会话、报警、故障、Ready、选环）一条都不该少：
    // 回零中电机会真的转，把闸门开一半等于给了一条绕过门禁的启动路径。
    if (!connected_ || device_ == nullptr) {
        return refuse(mc::OperatorAction::Home, subject, QStringLiteral("还没连上设备"));
    }
    const QString blocked = startBlockReason();
    if (!blocked.isEmpty()) {
        return refuse(mc::OperatorAction::Home, subject, blocked);
    }
    if (!mc::isHomeParamValid(param)) {
        return refuse(mc::OperatorAction::Home, subject,
                      QStringLiteral("回零参数不合法：速度必须大于 0（0 是永远走不到），"
                                     "时限也必须有（没有时限的回零会一直挂着）"));
    }
    if (!device_->home(param)) {
        return refuse(mc::OperatorAction::Home, subject,
                      QStringLiteral("本地没能发出去（会话未就绪或链路已失效）"));
    }
    // 记下**这一条**的序号：终点判定要用它去找命令的结局（见 driveHoming）。
    // 与配方下装同一条道理 —— 不从本层另维护一个平行计数器。
    const std::deque<CommandRecord>& log = device_->commandLog();
    homeSeq_ = log.empty() ? 0U : log.front().seq;
    homeStatus_ = HomeStatus{};
    homeStatus_.phase = HomePhase::Started;
    homeStatus_.speedDegPerSec = param.speedDegPerSec;
    homeStatus_.timeoutMs = param.timeoutMs;
    homeStatus_.detail = QStringLiteral("已下发 HOME，等设备确认并开始走…");
    homeStartedMs_ = elapsedMs_;
    homeSawDeviceHoming_ = false;
    audited(mc::OperatorAction::Home, subject, true);
    emit commandLogChanged();
    emit homeChanged();
    return true;
}

const CommandRecord* DeviceManager::findHomeCommand() const {
    if (device_ == nullptr || homeSeq_ == 0U) {
        return nullptr;
    }
    // 与 findRecipeCommand 同样的匹配方式：seq 与命令名一起匹配。
    // 只按 seq 找会认错人（心跳与命令共用同一个序号空间）。
    for (const CommandRecord& record : device_->commandLog()) {
        if (record.seq == homeSeq_ && record.name == "HOME") {
            return &record;
        }
    }
    return nullptr;
}

void DeviceManager::driveHoming() {
    if (device_ == nullptr) {
        return;
    }
    const mc::DeviceState state = device_->deviceState();
    const bool homing = (state == mc::DeviceState::Home);

    if (homeStatus_.phase == HomePhase::Started) {
        homeStatus_.elapsedMs = elapsedMs_ - homeStartedMs_;
        if (homing) {
            homeSawDeviceHoming_ = true;
            homeStatus_.detail = QStringLiteral("设备正在朝零点走（已走 %1 s）")
                                     .arg(homeStatus_.elapsedMs / 1000.0, 0, 'f', 1);
            emit homeChanged();
            return;
        }

        // 设备已经不在回零态了。这一趟**结束了没有**？两个证据之一成立就算结束：
        //   ① 亲眼见过它进 Home（homeSawDeviceHoming_）；
        //   ② 那条 HOME 拿到了应答 —— 应答的语义就是"回零开始了"（见 protocol.hpp），
        //      所以"已确认 + 现在已经不在 Home"= 这一趟已经走完。
        //
        // 只用 ① 不够：零点就在脚下时，整趟回零可以完整地落在两拍之间，
        // 于是"没见过"被永远读成"还在走"，界面卡在"回零进行中"。
        // 只用 ② 也不够：程序里的那一趟回零不是本机发的（homeSeq_ == 0），
        // 它没有本机记的那条命令。
        const CommandRecord* command = findHomeCommand();

        // 命令自己就没成：这是**明说得出原因**的结局，优先用它。
        // （设备把回零拒了的时候，设备根本不会进 Home，靠状态判会判成"还在等"。）
        if (command != nullptr && command->status == CommandStatus::Nacked) {
            homeStatus_.phase = HomePhase::Failed;
            homeStatus_.detail = QStringLiteral("回零命令被设备拒绝：%1（零点**没有**建立）")
                                     .arg(QString::fromUtf8(nackReasonName(command->nackReason)));
            homeStatus_.elapsedMs = elapsedMs_ - homeStartedMs_;
            emit homeChanged();
            return;
        }
        if (command != nullptr &&
            (command->status == CommandStatus::TimedOut ||
             command->status == CommandStatus::NotSent)) {
            homeStatus_.phase = HomePhase::Failed;
            homeStatus_.detail = QStringLiteral(
                "回零命令没有拿到确认（%1）：设备有没有在动**无法判定**，"
                "别把它当成回零成功（零点没有建立）")
                                     .arg(QString::fromUtf8(commandStatusName(command->status)));
            homeStatus_.elapsedMs = elapsedMs_ - homeStartedMs_;
            emit homeChanged();
            return;
        }

        const bool accepted = (command != nullptr && command->status == CommandStatus::Acked);
        if (!homeSawDeviceHoming_ && !accepted) {
            // 命令还在路上：此刻什么都说不准。等下一拍 —— 但**不能无限等**：
            // 超过这一趟给的时限还没见设备动，就该按"没成"结清，而不是把界面
            // 永远挂在"回零进行中"上（那是"挂着"与"还在走"分不出来的那一类状态）。
            if (homeStatus_.timeoutMs > 0U && homeStatus_.elapsedMs > homeStatus_.timeoutMs) {
                homeStatus_.phase = HomePhase::Failed;
                homeStatus_.detail = QStringLiteral(
                    "等到时限（%1 s）也没见设备进入回零态，也没有可查的命令结局 —— "
                    "按未完成处置（零点没有建立）")
                                         .arg(homeStatus_.timeoutMs / 1000.0, 0, 'f', 1);
                emit homeChanged();
            }
            return;
        }

        // 走到头了。"零点建立没有"这件事**只认设备报的那一位**
        // （遥测里的 kTelemetryFlagHomingAttained）：
        // 状态分不出"走到位的 Ready"与"被 STOP 打断后回到的 Ready" ——
        // 打断那条路径里的 Stopping 会在两拍之间过去（回零本来就是低速，
        // 那一拍的 |v| 常常已经在停机门限内）。拿状态猜等于把"没发生的事"说成发生了。
        const bool attained = device_->homingAttained();
        if (attained) {
            homeStatus_.phase = HomePhase::Succeeded;
            homeStatus_.detail = QStringLiteral("零点已建立：设备把到位的位置定义成了 0，"
                                                "现在停在 %1（用时 %2 s）")
                                     .arg(QString::fromUtf8(mc::deviceStateName(state)))
                                     .arg(homeStatus_.elapsedMs / 1000.0, 0, 'f', 1);
        } else if (state == mc::DeviceState::Fault) {
            homeStatus_.phase = HomePhase::Failed;
            homeStatus_.detail = QStringLiteral("回零失败：%1（零点**没有**建立）")
                                     .arg(alarmText(mc::alarmCodeForFault(device_->faultCode())));
            raiseAlarm(mc::kAlarmCodeHomeFailed, homeStatus_.detail);
        } else if (state == mc::DeviceState::Stopping || state == mc::DeviceState::Ready) {
            homeStatus_.phase = HomePhase::Aborted;
            homeStatus_.detail = QStringLiteral(
                "这一轮回零没有走完（设备停在 %1）：**零点没有重新建立**，"
                "当前坐标仍然是上一次建立的基准。要基准就重新回零")
                                     .arg(QString::fromUtf8(mc::deviceStateName(state)));
        } else {
            homeStatus_.phase = HomePhase::Failed;
            homeStatus_.detail = QStringLiteral("回零没走完就离开了回零态（当前 %1，零点没有建立）")
                                     .arg(QString::fromUtf8(mc::deviceStateName(state)));
        }
        homeStatus_.elapsedMs = elapsedMs_ - homeStartedMs_;
        // 结果进历史：回零成没成是"事后要查"的事，而它多半不是一条报警 ——
        // 成功的回零、被人工中止的回零都不该出现在报警列表里，但"什么时候回过零、
        // 结果是什么"必须查得到（它是后面所有位置读数的基准从哪来的唯一线索）。
        if (history_ != nullptr) {
            history_->addEvent(static_cast<std::uint64_t>(elapsedMs_),
                               QDateTime::currentMSecsSinceEpoch(),
                               QStringLiteral("回零：%1").arg(homeStatus_.detail));
        }
        emit homeChanged();
        return;
    }

    // 设备自己进了回零（比如程序里的 HOME 步，或者别的主机发的），
    // 而上位机这边没有在跟踪：补一条记录，让界面上这趟回零有一个"谁在什么时候发起的"。
    if (homing && homeStatus_.phase != HomePhase::Started) {
        homeStatus_ = HomeStatus{};
        homeStatus_.phase = HomePhase::Started;
        homeStatus_.detail = QStringLiteral("设备正在回零（不是由本机上这个按钮发起的）");
        homeStartedMs_ = elapsedMs_;
        homeSawDeviceHoming_ = true;
        homeSeq_ = 0U;   // 这一趟没有本机发的命令，终点只能按"见过 Home"来判
        emit homeChanged();
    }
}


bool DeviceManager::saveRecipe(const mc::Recipe& recipe, qint64* outId, QString& error) {
    const QString subject = QStringLiteral("配方 %1 v%2")
                                .arg(QString::fromStdString(recipe.name))
                                .arg(recipe.version);
    if (!permitted(mc::OperatorAction::SaveRecipe, subject)) {
        error = lastRefusal_;
        return false;
    }
    RecipeStore* store = recipes_.get();
    if (store == nullptr || !store->isOpen()) {
        const QString reason = QStringLiteral("配方库没打开，没什么可写的（原因见配方页）。");
        refuse(mc::OperatorAction::SaveRecipe, subject, reason);
        error = reason;
        return false;
    }
    // outId 可以传空指针（调用者只关心成没成）；> 0 表示更新已有的那一行。
    const qint64 id = (outId != nullptr) ? *outId : 0;
    const bool ok = id > 0 ? store->update(id, recipe, error) : store->insert(recipe, outId, error);
    audited(mc::OperatorAction::SaveRecipe, subject, ok, ok ? QString() : error);
    return ok;
}

bool DeviceManager::deleteRecipe(qint64 id, QString& error) {
    const QString subject = QStringLiteral("配方 id=%1").arg(id);
    if (!permitted(mc::OperatorAction::DeleteRecipe, subject)) {
        error = lastRefusal_;
        return false;
    }
    RecipeStore* store = recipes_.get();
    if (store == nullptr || !store->isOpen()) {
        const QString reason = QStringLiteral("配方库没打开，没什么可删的（原因见配方页）。");
        refuse(mc::OperatorAction::DeleteRecipe, subject, reason);
        error = reason;
        return false;
    }
    const bool ok = store->remove(id, error);
    audited(mc::OperatorAction::DeleteRecipe, subject, ok, ok ? QString() : error);
    return ok;
}

bool DeviceManager::saveSequence(const mc::SequenceProgram& program, qint64* outId, QString& error) {
    const QString subject = QStringLiteral("程序 %1 v%2")
                                .arg(QString::fromStdString(program.name))
                                .arg(program.version);
    if (!permitted(mc::OperatorAction::SaveProgram, subject)) {
        error = lastRefusal_;
        return false;
    }
    SequenceStore* store = sequences_.get();
    if (store == nullptr || !store->isOpen()) {
        const QString reason = QStringLiteral("程序库没打开，没什么可写的（原因见任务页）。");
        refuse(mc::OperatorAction::SaveProgram, subject, reason);
        error = reason;
        return false;
    }
    const qint64 id = (outId != nullptr) ? *outId : 0;
    const bool ok = id > 0 ? store->update(id, program, error) : store->insert(program, outId, error);
    audited(mc::OperatorAction::SaveProgram, subject, ok, ok ? QString() : error);
    return ok;
}

bool DeviceManager::deleteSequence(qint64 id, QString& error) {
    const QString subject = QStringLiteral("程序 id=%1").arg(id);
    if (!permitted(mc::OperatorAction::DeleteProgram, subject)) {
        error = lastRefusal_;
        return false;
    }
    SequenceStore* store = sequences_.get();
    if (store == nullptr || !store->isOpen()) {
        const QString reason = QStringLiteral("程序库没打开，没什么可删的（原因见任务页）。");
        refuse(mc::OperatorAction::DeleteProgram, subject, reason);
        error = reason;
        return false;
    }
    const bool ok = store->remove(id, error);
    audited(mc::OperatorAction::DeleteProgram, subject, ok, ok ? QString() : error);
    return ok;
}

QString DeviceManager::defaultHistoryPath() {
    // 用户数据目录而不是当前目录：双击启动时"当前目录"是哪儿只有天知道，
    // 历史文件散落在下载目录/桌面/安装目录里是查不出来的。
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dir.isEmpty()) {
        dir = QDir::homePath() + QStringLiteral("/.mc_host");
    }
    QDir().mkpath(dir);
    return dir + QStringLiteral("/history.db");
}

DeviceManager::DeviceManager(QObject* parent)
    : DeviceManager(StorePolicy::OpenDefaultPaths, parent) {}

DeviceManager::DeviceManager(StorePolicy policy, QObject* parent) : QObject(parent) {
    device_ = std::make_unique<SimulatedDevice>();

    // 默认就把历史库与配方库打开。打不开只是"没有历史/没有配方"，
    // 不是"不能用"：这种情况下 historyOpen()/recipesOpen() 为假、error() 有原因，
    // 对应页面会照实显示。
    if (policy == StorePolicy::OpenDefaultPaths) {
        history_ = std::make_unique<HistoryStore>();
        (void)history_->open(defaultHistoryPath());

        recipes_ = std::make_unique<RecipeStore>();
        (void)recipes_->open(defaultRecipePath());

        sequences_ = std::make_unique<SequenceStore>();
        (void)sequences_->open(defaultSequencePath());
    }

    timer_.setInterval(kTickIntervalMs);
    // 精确定时器：默认的粗粒度定时器在 Windows/WSL 上有 ±十几毫秒的抖动，
    // 而它直接就是仿真的积分步长（dt），抖动会变成速度曲线的毛刺。
    timer_.setTimerType(Qt::PreciseTimer);
    connect(&timer_, &QTimer::timeout, this, &DeviceManager::onTick);
}

DeviceManager::~DeviceManager() = default;

void DeviceManager::setDevice(std::unique_ptr<IDevice> device) {
    if (connected_) {
        // 先断开：否则旧设备的链路会被悄悄丢掉（而且测试里注入的假设备也一样）。
        disconnectDevice();
    }
    device_ = std::move(device);
}

bool DeviceManager::openHistory(const QString& dbPath) {
    if (history_ == nullptr) {
        history_ = std::make_unique<HistoryStore>();
    }
    return history_->open(dbPath);
}

QString DeviceManager::defaultSequencePath() {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dir.isEmpty()) {
        dir = QDir::homePath() + QStringLiteral("/.mc_host");
    }
    QDir().mkpath(dir);
    return dir + QStringLiteral("/sequences.db");
}

bool DeviceManager::openSequences(const QString& dbPath) {
    if (sequences_ == nullptr) {
        sequences_ = std::make_unique<SequenceStore>();
    }
    return sequences_->open(dbPath);
}

bool DeviceManager::startSequence(const mc::SequenceProgram& program) {
    const QString subject = QStringLiteral("程序 %1 v%2")
                                .arg(QString::fromStdString(program.name))
                                .arg(program.version);
    if (!permitted(mc::OperatorAction::RunProgram, subject)) {
        setSequenceStartError(lastRefusal_);
        return false;
    }

    // --- 门禁：与手动启动**用同一条判据** ---------------------------------------
    // 自动流程不该绕过人手按启动时要过的同一道闸。判据只有一条：
    // 联锁表里拦启动的规则全都满足（见 startBlockReason）。
    const auto refuse = [this, &subject](const QString& reason) {
        setSequenceStartError(reason);
        // 跑不起来的程序也要留痕：它是"有人想让它跑，但被拦住了"。
        DeviceManager::refuse(mc::OperatorAction::RunProgram, subject, reason);
        return false;
    };
    // "连设备对象都没有"是退化状态，不属于现场条件（联锁表管的是现场）。
    // 联锁表其实也会拦下它（链路那一条），但那要靠"空指针时快照全是默认值"
    // 这个巧合成立 —— 不值得把安全建立在巧合上。
    if (device_ == nullptr) {
        return refuse(QStringLiteral("没有设备：先选数据源并连上"));
    }
    const QString blocked = startBlockReason(true);
    if (!blocked.isEmpty()) {
        return refuse(blocked);
    }

    // 量纲上界要按**设备此刻真实的模式**判：库里存的时候判不了这件事
    // （那时不知道设备停在哪个模式），只有现在才有资格判。
    // 用 runMode()（设备实际在用的环）而不是"上一次回读到的参数" ——
    // 后者可能从来没读过，那就等于在猜。
    const std::uint8_t mode = static_cast<std::uint8_t>(device_->runMode());
    const mc::SequenceError verdict = mc::validateSequence(program, mode);
    if (verdict != mc::SequenceError::None) {
        return refuse(QStringLiteral("程序不合法：%1")
                          .arg(sequenceErrorText(verdict)));
    }
    if (!sequencer_.load(program, mode)) {
        return refuse(QStringLiteral("装载失败：%1")
                          .arg(sequenceErrorText(sequencer_.loadError())));
    }

    sequenceSeq_ = 0U;
    sequenceStepName_ = nullptr;
    sequenceStartError_.clear();
    if (!sequencer_.start(static_cast<std::uint64_t>(elapsedMs_))) {
        return refuse(QString::fromStdString(sequencer_.status().detail));
    }

    // 起跑也进历史：这是"这台设备这一段按什么顺序跑了什么"的起点。
    if (history_ != nullptr) {
        history_->addEvent(static_cast<std::uint64_t>(elapsedMs_),
                           QDateTime::currentMSecsSinceEpoch(),
                           QStringLiteral("程序开始执行：%1 v%2（共 %3 步）")
                               .arg(QString::fromStdString(program.name))
                               .arg(program.version)
                               .arg(static_cast<int>(program.steps.size())));
    }
    audited(mc::OperatorAction::RunProgram, subject, true);
    emit sequenceChanged();
    return true;
}

bool DeviceManager::abortSequence() {
    const QString subject = QStringLiteral("中止当前程序");
    if (!permitted(mc::OperatorAction::AbortProgram, subject)) {
        return false;
    }
    if (sequencer_.status().phase != mc::SequencePhase::Running) {
        return refuse(mc::OperatorAction::AbortProgram, subject,
                      QStringLiteral("现在没有在跑的程序"));
    }
    sequencer_.abort("操作员中止。注意：已经下发并被确认的命令收不回来，设备保持在那时的状态。");
    sequenceSeq_ = 0U;
    sequenceStepName_ = nullptr;
    if (history_ != nullptr) {
        history_->addEvent(static_cast<std::uint64_t>(elapsedMs_),
                           QDateTime::currentMSecsSinceEpoch(),
                           QStringLiteral("程序已中止：%1")
                               .arg(QString::fromStdString(sequencer_.status().detail)));
    }
    audited(mc::OperatorAction::AbortProgram, subject, true);
    emit sequenceChanged();
    return true;
}

void DeviceManager::setSequenceStartError(const QString& reason) {
    sequenceStartError_ = reason;
    emit sequenceChanged();
}

bool DeviceManager::dispatchSequenceStep() {
    IDevice* device = device_.get();
    if (device == nullptr) {
        return false;
    }

    const mc::SequenceStep& step = sequencer_.currentStep();
    bool sent = false;
    const char* name = "?";
    switch (step.kind) {
        case mc::SequenceStepKind::SetMode:
            name = "SET_MODE";
            sent = device->setMode(step.mode);
            break;
        case mc::SequenceStepKind::SetPid:
            name = "SET_PID";
            sent = device->setPid(mc::SetPid{step.kp, step.ki, step.kd});
            break;
        case mc::SequenceStepKind::SetTarget:
            name = "SET_TARGET";
            sent = device->setTarget(step.target);
            break;
        case mc::SequenceStepKind::Start:
            name = "START";
            // 这里**不**再判一次报警门禁：启动那一下已经判过了，而中途新出的报警
            // 会把整部程序在中止那条路上停掉（见 driveSequence）。
            // 在每一步上都挂一串门禁判断，等于把同一套规则写了两处。
            sent = device->start();
            break;
        case mc::SequenceStepKind::Home: {
            name = "HOME";
            // 程序里的回零走的是**同一条协议命令**（HOME），参数从步骤里取：
            //   target = 回零速度，waitMs = 超时上限（见 sequence.hpp 的 Home 说明）。
            // 刻意不复用"设模式 + 设目标 + 启动"去拼一个回零 —— 那样拼出来的
            // 只是"走去 0 度"，而零点在哪是设备侧的知识（见 protocol.hpp 的 Home）。
            mc::HomeParam param;
            param.speedDegPerSec = step.target;
            param.timeoutMs = static_cast<std::uint32_t>(step.waitMs);
            sent = device->home(param);
            break;
        }
        case mc::SequenceStepKind::Stop:
            name = "STOP";
            sent = device->stop();
            break;
        case mc::SequenceStepKind::Wait:
        case mc::SequenceStepKind::WaitReady:
            // 等待类步骤不产生下行报文，永远走不到这里
            // （引擎也不会把它们摆成待发，见 Sequencer::awaitingDispatch）。
            sequencer_.reportDispatched(false, "内部错误：等待类步骤不该被当成待发命令");
            return false;
    }
    emit commandLogChanged();

    const std::deque<CommandRecord>& log = device->commandLog();
    const std::uint16_t seq = log.empty() ? 0U : log.front().seq;
    if (!sent || seq == 0U) {
        // 本地就没发出去（设备侧状态非法、或链路在发的那一刻断了）。
        sequencer_.reportDispatched(false, QStringLiteral("%1 本地没能发出去")
                                                .arg(QString::fromUtf8(name))
                                                .toStdString());
        emit sequenceChanged();
        return false;
    }

    sequenceSeq_ = seq;
    sequenceStepName_ = name;
    sequencer_.reportDispatched(true, "");
    emit sequenceChanged();
    return true;
}

bool DeviceManager::reportSequenceOutcome() {
    IDevice* device = device_.get();
    if (device == nullptr || sequenceSeq_ == 0U || sequenceStepName_ == nullptr) {
        return false;
    }

    const CommandRecord* record = nullptr;
    for (const CommandRecord& candidate : device->commandLog()) {
        if (candidate.seq == sequenceSeq_ && candidate.name == sequenceStepName_) {
            record = &candidate;
            break;
        }
    }

    mc::StepOutcome outcome = mc::StepOutcome::Waiting;
    std::string detail;
    if (record == nullptr) {
        // 记录已经滚出命令行（上限 64 条），没法判定结果。**按"结局未知"处置**：
        // 未知就停是这套流程的铁律之一，而"猜它成功了"是最不能选的那一个猜测。
        outcome = mc::StepOutcome::TimedOut;
        detail = "这一步的命令记录已滚出命令行，无法判定结果";
    } else {
        switch (record->status) {
            case CommandStatus::Awaiting:
                return false;   // 还在飞，下一拍再看
            case CommandStatus::Acked:
                outcome = mc::StepOutcome::Acked;
                break;
            case CommandStatus::Nacked:
                outcome = mc::StepOutcome::Nacked;
                detail = QStringLiteral("设备拒绝：%1")
                             .arg(QString::fromUtf8(nackReasonName(record->nackReason)))
                             .toStdString();
                break;
            case CommandStatus::TimedOut:
                outcome = mc::StepOutcome::TimedOut;
                detail = "等应答超时（运动指令不自动重发）";
                break;
            case CommandStatus::NotSent:
                outcome = mc::StepOutcome::NotSent;
                detail = "本地没能发出去";
                break;
            case CommandStatus::Unidirectional:
                outcome = mc::StepOutcome::Acked;
                break;
        }
    }

    sequenceSeq_ = 0U;
    sequenceStepName_ = nullptr;
    sequencer_.reportOutcome(outcome, detail);
    // 每一步的结局都进历史：事后要查的正是"它到底走到哪一步、为什么停"。
    if (history_ != nullptr) {
        const mc::SequenceStatus& status = sequencer_.status();
        history_->addEvent(static_cast<std::uint64_t>(elapsedMs_),
                           QDateTime::currentMSecsSinceEpoch(),
                           QStringLiteral("程序第 %1 步：%2")
                               .arg(static_cast<int>(status.stepIndex) + 1)
                               .arg(QString::fromStdString(status.detail)));
    }
    emit sequenceChanged();
    return true;
}

void DeviceManager::driveSequence() {
    // 先推进引擎（等待类步骤到点、摆出该发的步），再结算上一条的结局，
    // 最后发新摆出来的那一步。顺序见头文件里的说明。
    //
    // "设备是不是停在 Ready"必须在推进**之前**告诉它：引擎自己看不见设备，
    // 而这个条件是"等设备就绪"那一步唯一的判据。
    sequencer_.reportDeviceReady(device_ != nullptr &&
                                 device_->deviceState() == mc::DeviceState::Ready);
    sequencer_.tick(static_cast<std::uint64_t>(elapsedMs_));

    if (sequencer_.status().phase != mc::SequencePhase::Running) {
        return;
    }

    // 链路/会话在跑的过程中没了：及时停下并把原因说清，而不是停在一个
    // 永远等不到结局的"等确认…"上。
    if (device_ == nullptr || !device_->isOpen() || !device_->sessionReady()) {
        sequenceSeq_ = 0U;
        sequenceStepName_ = nullptr;
        sequencer_.abort("链路或会话已失效，程序中止");
        if (history_ != nullptr) {
            history_->addEvent(static_cast<std::uint64_t>(elapsedMs_),
                               QDateTime::currentMSecsSinceEpoch(),
                               QStringLiteral("程序已中止：链路或会话失效"));
        }
        emit sequenceChanged();
        return;
    }

    // 跑的中途新出了需要确认的报警：停。**不静默继续** —— 自动流程在报警之后
    // 继续往下跑，等于把"有人得看一眼"这件事跳过去了。
    if (alarms_.blocksStart()) {
        sequenceSeq_ = 0U;
        sequenceStepName_ = nullptr;
        sequencer_.abort(QStringLiteral("执行途中出现未确认的报警：%1").arg(startBlockReason(true))
                             .toStdString());
        if (history_ != nullptr) {
            history_->addEvent(static_cast<std::uint64_t>(elapsedMs_),
                               QDateTime::currentMSecsSinceEpoch(),
                               QStringLiteral("程序已中止：出现未确认的报警"));
        }
        emit sequenceChanged();
        return;
    }

    if (sequencer_.status().waitingOutcome) {
        (void)reportSequenceOutcome();
        // 结局报完可能已经推进/终止了，所以重新取一次状态再决定发不发。
    }

    if (sequencer_.status().phase == mc::SequencePhase::Running &&
        sequencer_.awaitingDispatch()) {
        (void)dispatchSequenceStep();
    }
}

QString DeviceManager::defaultRecipePath() {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dir.isEmpty()) {
        dir = QDir::homePath() + QStringLiteral("/.mc_host");
    }
    QDir().mkpath(dir);
    return dir + QStringLiteral("/recipes.db");
}

bool DeviceManager::openRecipes(const QString& dbPath) {
    if (recipes_ == nullptr) {
        recipes_ = std::make_unique<RecipeStore>();
    }
    return recipes_->open(dbPath);
}

bool DeviceManager::connectDevice() {
    if (!permitted(mc::OperatorAction::Connect, QStringLiteral("连接当前数据源"))) {
        return false;
    }
    if (connected_ || device_ == nullptr) {
        return false;
    }
    if (!device_->open()) {
        // 打开失败在真硬件上是常态（端口被占用、线没插、权限不够）。
        // 这里**不启动定时器、也不进入"已连接"**：一个打不开的端口被显示成
        // "已连接"，会让所有后续判断都建立在假前提上。
        const QString reason =
            QStringLiteral("打不开设备：%1").arg(QString::fromUtf8(openErrorName(device_->openError())));
        raiseAlarm(mc::kAlarmCodeOpenFailed, reason);
        audited(mc::OperatorAction::Connect, QStringLiteral("连接当前数据源"), false, reason);
        lastRefusal_ = reason;
        emit operationRefused(reason);
        emit connectionChanged(false);
        return false;
    }    clock_.start();
    timingClock_.start();
    lastTimingTickMs_ = -1;
    elapsedMs_ = 0.0;
    ticks_ = 0U;
    lastFaultLatched_ = false;
    connected_ = true;

    if (history_ != nullptr && history_->isOpen()) {
        (void)history_->beginSession(QString::fromUtf8(device_->kindName()), device_->isSimulated(),
                                     QDateTime::currentMSecsSinceEpoch());
    }

    timer_.start();
    audited(mc::OperatorAction::Connect,
            QStringLiteral("连接 %1").arg(QString::fromUtf8(device_->kindName())), true);
    logEvent(mc::LogLevel::Info, "link",
             QStringLiteral("已连接：%1").arg(QString::fromUtf8(device_->configSummary())));
    emit connectionChanged(true);
    return true;
}

bool DeviceManager::useSimulatedDevice() {
    const QString subject = QStringLiteral("数据源 = 进程内模拟设备");
    if (!permitted(mc::OperatorAction::SetDataSource, subject)) {
        return false;
    }
    if (connected_) {
        return refuse(mc::OperatorAction::SetDataSource, subject,
                      QStringLiteral("已连接中：换数据源会把一条正在跑的链路悄悄丢掉，"
                                     "请先断开再换"));
    }
    setDevice(std::make_unique<SimulatedDevice>());
    audited(mc::OperatorAction::SetDataSource, subject, true);
    emit connectionChanged(false);
    return true;
}

bool DeviceManager::useSerialDevice(const SerialTransport::Config& config, QString& error) {
    const QString subject = QStringLiteral("数据源 = 串口 %1").arg(config.portName);
    if (!permitted(mc::OperatorAction::SetDataSource, subject)) {
        error = lastRefusal_;
        return false;
    }
    if (connected_) {
        error = QStringLiteral("已连接中：请先断开再换数据源");
        refuse(mc::OperatorAction::SetDataSource, subject, error);
        return false;
    }
    if (config.portName.isEmpty()) {
        error = QStringLiteral("没有选择端口");
        refuse(mc::OperatorAction::SetDataSource, subject, error);
        return false;
    }
    setDevice(std::make_unique<SerialDevice>(config, mc::LinkMonitor::Config{}));
    audited(mc::OperatorAction::SetDataSource, subject, true);
    emit connectionChanged(false);
    return true;
}

bool DeviceManager::disconnectDevice() {
    if (!permitted(mc::OperatorAction::Disconnect, QStringLiteral("断开当前数据源"))) {
        return false;
    }
    if (!connected_) {
        return false;
    }
    timer_.stop();
    // 正在跑的程序：断开就是"这条链路没了"，程序停在这里。
    // 不在这里停的话，界面会留着一个永远不会推进的"执行中"。
    if (sequencer_.status().phase == mc::SequencePhase::Running) {
        sequenceSeq_ = 0U;
        sequenceStepName_ = nullptr;
        sequencer_.abort("已断开连接，程序中止");
        emit sequenceChanged();
    }
    if (device_ != nullptr) {
        device_->close();
    }
    connected_ = false;

    // 收摊前把这一段的尾巴落盘并结束会话。少了这一步，最后不到半个刷写周期的
    // 样本会永远留在内存里，而历史页上那一段会显示成"数据到此为止"。
    if (history_ != nullptr && history_->isOpen()) {
        history_->endSession(QDateTime::currentMSecsSinceEpoch());
    }

    audited(mc::OperatorAction::Disconnect, QStringLiteral("断开当前数据源"), true);
    logEvent(mc::LogLevel::Info, "link",
             QStringLiteral("已断开（本会话共 %1 拍）").arg(static_cast<qulonglong>(ticks_)));
    emit connectionChanged(false);
    emit commandLogChanged();
    return true;
}

void DeviceManager::clearTelemetry() {
    telemetry_.clear();
}

bool DeviceManager::clearAlarms() {
    const QString subject = QStringLiteral("清空报警清单");
    if (!permitted(mc::OperatorAction::ClearAlarmList, subject)) {
        return false;
    }
    // 只清清单：把屏幕擦干净。**不碰累计计数、也不碰设备状态** ——
    // 清屏与"让设备重新允许启动"差着一个数量级。
    const std::size_t before = alarms_.entries().size();
    alarms_.clearList();
    audited(mc::OperatorAction::ClearAlarmList, subject, true,
            QStringLiteral("清了 %1 条（清单上限 %2）")
                .arg(static_cast<int>(before))
                .arg(static_cast<int>(alarms_.capacity())));
    emit alarmsChanged();
    return true;
}

bool DeviceManager::acknowledgeAlarm(std::uint16_t code) {
    const QString subject = QStringLiteral("报警 %1").arg(alarmCodeText(code));
    if (!permitted(mc::OperatorAction::AcknowledgeAlarm, subject)) {
        return false;
    }
    const bool ok =
        alarms_.acknowledge(code, static_cast<std::uint64_t>(QDateTime::currentMSecsSinceEpoch()));
    if (ok) {
        syncAlarmToHistory(code);
        emit alarmsChanged();
    }
    // 确认一个不存在的码也要留痕：那说明有人按错了地方，而"按错了"正是审计要看的事。
    audited(mc::OperatorAction::AcknowledgeAlarm, subject, ok,
            ok ? QString() : QStringLiteral("清单里没有这个代码的条目"));
    return ok;
}

bool DeviceManager::acknowledgeAllAlarms() {
    const QString subject = QStringLiteral("确认全部报警");
    if (!permitted(mc::OperatorAction::AcknowledgeAllAlarms, subject)) {
        return false;
    }
    const std::size_t count =
        alarms_.acknowledgeAll(static_cast<std::uint64_t>(QDateTime::currentMSecsSinceEpoch()));
    if (count > 0U) {
        for (const mc::AlarmEntry& entry : alarms_.entries()) {
            syncAlarmToHistory(entry.code);
        }
        emit alarmsChanged();
    }
    audited(mc::OperatorAction::AcknowledgeAllAlarms, subject, count > 0U,
            count > 0U ? QString()
                       : QStringLiteral("没有需要确认的报警（已经都确认过了）"));
    return count > 0U;
}

QString DeviceManager::worstUnacknowledgedAlarmText() const {
    // 说清"最严重的是哪一条"。只说"有报警"等于让人自己去一页页翻 ——
    // 而这条说明的全部意义就是让人知道现在为什么按不动启动。
    mc::AlarmLevel worst = mc::AlarmLevel::Info;
    std::uint16_t worstCode = 0U;
    bool found = false;
    for (const mc::AlarmEntry& entry : alarms_.entries()) {
        if (entry.acknowledged) {
            continue;
        }
        const mc::AlarmSpec* spec = mc::alarmSpec(entry.code);
        const bool needsAck = (spec == nullptr) ? true : spec->requiresAck;
        if (!needsAck) {
            continue;
        }
        if (!found ||
            static_cast<std::uint8_t>(entry.level) > static_cast<std::uint8_t>(worst)) {
            worst = entry.level;
            worstCode = entry.code;
            found = true;
        }
    }
    if (!found) {
        return QString();
    }
    return QStringLiteral("%1｜%2").arg(alarmLevelText(worst)).arg(alarmText(worstCode));
}

mc::InterlockSnapshot DeviceManager::interlockSnapshot() const {
    mc::InterlockSnapshot snapshot;
    if (device_ == nullptr) {
        return snapshot;
    }
    // 逐项按现场如实填。这里**不做任何判断**：判断是规则表的事，
    // 在这一层顺手判一下就会长出第二套判据。
    snapshot.connected = connected_ && device_->isOpen();
    // 链路监控只在传输开着的时候才有意义；没连上时"链路失效"是句废话。
    snapshot.linkDown = snapshot.connected && device_->linkDown();
    snapshot.sessionReady = device_->sessionReady();
    snapshot.deviceState = device_->deviceState();
    snapshot.faultLatched = device_->faultLatched();
    snapshot.deviceMode = static_cast<std::uint8_t>(device_->runMode());
    snapshot.telemetryCount = telemetry_.size();
    snapshot.unacknowledgedAlarms = alarms_.unacknowledgedRequiringAck();
    return snapshot;
}

std::vector<DeviceManager::InterlockView> DeviceManager::interlockView() const {
    const mc::InterlockSnapshot snapshot = interlockSnapshot();
    const QString worstAlarm = worstUnacknowledgedAlarmText();

    std::vector<InterlockView> view;
    for (const mc::InterlockResult& result : mc::evaluateInterlocks(snapshot)) {
        InterlockView row;
        row.code = result.code;
        row.severity = result.severity;
        row.satisfied = result.satisfied;
        row.text = interlockSentence(result, snapshot, worstAlarm);
        view.push_back(row);
    }
    return view;
}

QString DeviceManager::startBlockReason(bool programOwnsMode) const {
    const mc::InterlockSnapshot snapshot = interlockSnapshot();
    for (const mc::InterlockResult& result : mc::evaluateInterlocks(snapshot)) {
        if (result.severity != mc::InterlockSeverity::Block || result.satisfied) {
            continue;
        }
        // 自动程序自己会设模式，所以"现在选了环没有"不该拦它。
        if (programOwnsMode && result.code == mc::kInterlockModeSelected) {
            continue;
        }
        // 说清"哪一条没满足"，而不是笼统地"条件不满足"。规则表已经按重要性排过序，
        // 所以第一条没满足的就是现在最该处理的那一条。
        return QStringLiteral("启动联锁未满足：%1")
            .arg(interlockSentence(result, snapshot, worstUnacknowledgedAlarmText()));
    }
    return QString();
}

bool DeviceManager::sendSetMode(int mode) {
    const QString subject = QStringLiteral("模式 %1").arg(mode);
    if (!permitted(mc::OperatorAction::SetMode, subject)) {
        return false;
    }
    if (!connected_ || device_ == nullptr) {
        return refuse(mc::OperatorAction::SetMode, subject, QStringLiteral("还没连上设备"));
    }
    const bool sent = device_->setMode(static_cast<std::uint8_t>(mode));
    audited(mc::OperatorAction::SetMode, subject, sent,
            sent ? QString() : QStringLiteral("本地没发出去（会话未就绪或链路已失效）"));
    emit commandLogChanged();
    return sent;
}

bool DeviceManager::sendStart() {
    const QString subject = QStringLiteral("启动");
    if (!permitted(mc::OperatorAction::Start, subject)) {
        return false;
    }
    if (!connected_ || device_ == nullptr) {
        return refuse(mc::OperatorAction::Start, subject, QStringLiteral("还没连上设备"));
    }
    // 启动门禁：**唯一一条判据就是联锁表**（见 interlockSnapshot / startBlockReason）。
    // 以前这里自己判"报警有没有确认"、设备再判一次"状态允不允许"，结果"为什么按不动"
    // 分散在两处、一次只报一条。现在全部收进规则表：一次判完、按重要性列出没满足的。
    //
    // 设备侧那道门禁**依然在**，而且必须留着：它是最后一道闸，不能因为上位机判过
    // 就不再判 —— 上位机的快照永远可能比设备慢一拍。
    const QString blocked = startBlockReason();
    if (!blocked.isEmpty()) {
        return refuse(mc::OperatorAction::Start, subject, blocked);
    }
    const bool sent = device_->start();
    audited(mc::OperatorAction::Start, subject, sent,
            sent ? QString() : QStringLiteral("本地没发出去（会话未就绪或链路已失效）"));
    emit commandLogChanged();
    return sent;
}

bool DeviceManager::sendStop() {
    const QString subject = QStringLiteral("停止");
    if (!permitted(mc::OperatorAction::Stop, subject)) {
        return false;
    }
    if (!connected_ || device_ == nullptr) {
        return refuse(mc::OperatorAction::Stop, subject, QStringLiteral("还没连上设备"));
    }
    // STOP 是最不该被本地条件拦住的一条：链路开着就发。
    // （设备侧的状态机规定 STOP 在任何状态都接受。）
    const bool sent = device_->stop();
    audited(mc::OperatorAction::Stop, subject, sent,
            sent ? QString() : QStringLiteral("本地没发出去（会话未就绪或链路已失效）"));
    emit commandLogChanged();
    return sent;
}

bool DeviceManager::sendSetTarget(double target) {
    const QString subject = QStringLiteral("目标 %1").arg(target, 0, 'f', 1);
    if (!permitted(mc::OperatorAction::SetTarget, subject)) {
        return false;
    }
    if (!connected_ || device_ == nullptr) {
        return refuse(mc::OperatorAction::SetTarget, subject, QStringLiteral("还没连上设备"));
    }
    const bool sent = device_->setTarget(static_cast<float>(target));
    audited(mc::OperatorAction::SetTarget, subject, sent,
            sent ? QString() : QStringLiteral("本地没发出去（会话未就绪或链路已失效）"));
    emit commandLogChanged();
    return sent;
}

bool DeviceManager::sendSetPid(double kp, double ki, double kd) {
    const QString subject = QStringLiteral("Kp=%1 Ki=%2 Kd=%3")
                                .arg(kp, 0, 'f', 3)
                                .arg(ki, 0, 'f', 3)
                                .arg(kd, 0, 'f', 3);
    if (!permitted(mc::OperatorAction::SetPid, subject)) {
        return false;
    }
    if (!connected_ || device_ == nullptr) {
        return refuse(mc::OperatorAction::SetPid, subject, QStringLiteral("还没连上设备"));
    }
    mc::SetPid pid;
    pid.kp = static_cast<float>(kp);
    pid.ki = static_cast<float>(ki);
    pid.kd = static_cast<float>(kd);
    const bool sent = device_->setPid(pid);
    audited(mc::OperatorAction::SetPid, subject, sent,
            sent ? QString() : QStringLiteral("本地没发出去（会话未就绪或链路已失效）"));
    emit commandLogChanged();
    return sent;
}

bool DeviceManager::sendClearFault(mc::FaultCode code) {
    const QString subject = QStringLiteral("故障码 %1").arg(alarmCodeText(mc::alarmCodeForFault(code)));
    if (!permitted(mc::OperatorAction::ClearFault, subject)) {
        return false;
    }
    if (!connected_ || device_ == nullptr) {
        return refuse(mc::OperatorAction::ClearFault, subject, QStringLiteral("还没连上设备"));
    }
    const bool sent = device_->clearFault(code);
    audited(mc::OperatorAction::ClearFault, subject, sent,
            sent ? QString() : QStringLiteral("本地没发出去（会话未就绪或链路已失效）"));
    emit commandLogChanged();
    return sent;
}

bool DeviceManager::sendRecipe(const mc::Recipe& recipe) {
    const QString subject = QStringLiteral("配方 %1 v%2")
                                .arg(QString::fromStdString(recipe.name))
                                .arg(recipe.version);
    if (!permitted(mc::OperatorAction::DownloadRecipe, subject)) {
        recipeStatus_ = RecipeDownloadStatus{};
        recipeStatus_.phase = RecipeDownloadPhase::Failed;
        recipeStatus_.recipe = recipe;
        recipeStatus_.detail = lastRefusal_;
        emit recipeDownloadChanged();
        return false;
    }
    // 本地能挡的都先挡住，并且**说清为什么**。若放着第一条命令到设备那里才被拒，
    // 界面上留下的就只有一条 NAK 和一句无从解释的"下装失败"。
    const auto fail = [this, &recipe, &subject](const QString& reason) {
        recipeStatus_ = RecipeDownloadStatus{};
        recipeStatus_.phase = RecipeDownloadPhase::Failed;
        recipeStatus_.recipe = recipe;
        recipeStatus_.detail = reason;
        recipeStep_ = 0;
        // 下装的"被拒"要和权限那一关一样登记：**试过了、没发生**，
        // 理由原样进流水。
        refuse(mc::OperatorAction::DownloadRecipe, subject, reason);
        emit recipeDownloadChanged();
        return false;
    };

    if (device_ == nullptr || !connected_ || !device_->isOpen()) {
        return fail(QStringLiteral("没有连接的设备：先连上再下装"));
    }
    if (!device_->sessionReady()) {
        return fail(QStringLiteral("会话未就绪（正在握手 / 等链路恢复）：此时发的命令到不了设备"));
    }
    // 要求停在 Ready 不是多一道手续：SET_MODE 与 SET_TARGET 都只在 Ready 放行，
    // 而 SET_MODE 改的是 target 的**单位语义**（deg/s 还是 deg）——
    // 运动当中换单位等于让同一个数字换个意思。设备会拒，但拒在第一条命令上，
    // 界面只能说"下装失败"；在这里拦下才能给出"先停机"这个可执行的建议。
    if (device_->deviceState() != mc::DeviceState::Ready) {
        return fail(QStringLiteral("设备当前是 %1，配方下装要求停在 Ready（就地待命、输出未使能）："
                                   "请先停机再来下装")
                        .arg(QString::fromUtf8(mc::deviceStateName(device_->deviceState()))));
    }
    const mc::RecipeError verdict = mc::validateRecipe(recipe);
    if (verdict != mc::RecipeError::None) {
        return fail(QStringLiteral("配方本身不合法：%1").arg(recipeErrorText(verdict)));
    }

    recipeStatus_ = RecipeDownloadStatus{};
    recipeStatus_.phase = RecipeDownloadPhase::Sending;
    recipeStatus_.recipe = recipe;
    recipeStep_ = 0;
    if (!startRecipeStep()) {
        // startRecipeStep 已经把原因写成 Failed 并发过信号；这里只补一条流水。
        audited(mc::OperatorAction::DownloadRecipe, subject, false, recipeStatus_.detail);
        return false;
    }
    audited(mc::OperatorAction::DownloadRecipe, subject, true);
    emit recipeDownloadChanged();
    return true;
}

bool DeviceManager::startRecipeStep() {
    IDevice* device = device_.get();
    if (device == nullptr) {
        failRecipeDownload(QStringLiteral("设备不在了"));
        return false;
    }

    bool sent = false;
    switch (recipeStep_) {
        case 0:
            recipeStepName_ = "SET_MODE";
            sent = device->setMode(recipeStatus_.recipe.mode);
            break;
        case 1:
            recipeStepName_ = "SET_PID";
            sent = device->setPid(mc::SetPid{recipeStatus_.recipe.kp, recipeStatus_.recipe.ki,
                                             recipeStatus_.recipe.kd});
            break;
        case 2:
            recipeStepName_ = "SET_TARGET";
            sent = device->setTarget(recipeStatus_.recipe.target);
            break;
        case 3:
            recipeStepName_ = "READ_PARAMS";
            // 记下基线：要核对的是**这一条请求新带回**的那份参数，
            // 而不是之前某一次读到的旧值。
            recipeParamBaseline_ = device->paramStatesReceivedTotal();
            recipeParamRejectBaseline_ = device->paramStatesRejectedTotal();
            sent = device->readParams();
            recipeStatus_.phase = RecipeDownloadPhase::Verifying;
            break;
        default:
            failRecipeDownload(QStringLiteral("内部步骤越界（%1）").arg(recipeStep_));
            return false;
    }

    // seq 从这个设备的命令行里**当场取**（界面上那条最新记录就是它），
    // 不在本层另维护一个平行计数器 —— 两套序号迟早对不上，而这一层还判不了重。
    const std::deque<CommandRecord>& log = device->commandLog();
    const std::uint16_t seq = log.empty() ? 0U : log.front().seq;
    if (!sent || seq == 0U) {
        failRecipeDownload(QStringLiteral("%1 没有发出去（本地就被拦下了）")
                               .arg(QString::fromUtf8(recipeStepName_)));
        return false;
    }

    recipeSeq_ = seq;
    if (recipeStep_ < 3) {
        recipeStatus_.detail = QStringLiteral("已下发 %1/3：%2（seq %3），等设备确认…")
                                   .arg(recipeStep_ + 1)
                                   .arg(QString::fromUtf8(recipeStepName_))
                                   .arg(seq);
    } else {
        recipeStatus_.detail =
            QStringLiteral("三条配置命令已确认，已请求回读（seq %1），等设备把参数报回来…").arg(seq);
    }
    return true;
}

const CommandRecord* DeviceManager::findRecipeCommand() const {
    if (device_ == nullptr || recipeStepName_ == nullptr) {
        return nullptr;
    }
    // 按 (seq, 名字) 一起匹配：seq 相同但类型不同的记录确实存在
    // （心跳与命令共用同一个序号空间），只按 seq 找会认错人。
    for (const CommandRecord& record : device_->commandLog()) {
        if (record.seq == recipeSeq_ && record.name == recipeStepName_) {
            return &record;
        }
    }
    return nullptr;
}

void DeviceManager::driveRecipeDownload() {
    const RecipeDownloadPhase phase = recipeStatus_.phase;
    if (phase != RecipeDownloadPhase::Sending && phase != RecipeDownloadPhase::Verifying) {
        return;
    }
    if (device_ == nullptr || !device_->isOpen()) {
        failRecipeDownload(QStringLiteral("链路已关闭，下装中止"));
        return;
    }
    if (!device_->sessionReady()) {
        failRecipeDownload(QStringLiteral("会话已失效（链路中断或正在重新握手），下装中止"));
        return;
    }

    // --- 命令结算：Sending（三条配置命令）与 Verifying（回读）共用这一段 ---------
    // 回读也是**一条命令**：它有请求、有应答、有超时，与前面三条走同一套结算。
    // 不另设"等一个被动到来的东西"的分支 —— 那种等待没有超时、也没有失败原因。
    const CommandRecord* record = findRecipeCommand();
    if (record == nullptr) {
        failRecipeDownload(
            QStringLiteral("第 %1 条命令的记录已滚出命令行（只保留最近 64 条），无法判定结果")
                .arg(recipeStep_ + 1));
        return;
    }

    switch (record->status) {
        case CommandStatus::Acked:
            break;
        case CommandStatus::Awaiting:
            // 等应答。超时怎么算由协议层的超时处置判定，这里**不另设一套**。
            return;
        case CommandStatus::Nacked:
            failRecipeDownload(QStringLiteral("设备拒绝了 %1：%2")
                                   .arg(QString::fromUtf8(recipeStepName_))
                                   .arg(QString::fromUtf8(nackReasonName(record->nackReason))));
            return;
        case CommandStatus::TimedOut:
            failRecipeDownload(QStringLiteral("%1 超时未确认。运动指令不自动重传（ADR-003），"
                                              "要继续就得再来一次下装")
                                   .arg(QString::fromUtf8(recipeStepName_)));
            return;
        case CommandStatus::NotSent:
            failRecipeDownload(
                QStringLiteral("%1 本地就没有发出去").arg(QString::fromUtf8(recipeStepName_)));
            return;
        case CommandStatus::Unidirectional:
            failRecipeDownload(QStringLiteral("%1 在协议上是单向的，不该出现在下装序列里")
                                   .arg(QString::fromUtf8(recipeStepName_)));
            return;
    }

    if (recipeStep_ == 3) {
        // 回读的应答到了。要核对的是**载荷**，不是"应答到了"这件事 ——
        // 后者只能证明设备收到了读请求，证明不了它存的是什么。
        if (device_->paramStatesRejectedTotal() > recipeParamRejectBaseline_) {
            failRecipeDownload(
                QStringLiteral("设备回的参数不合法（长度或取值不对，已被丢弃）：无法核对"));
            return;
        }
        if (device_->paramStatesReceivedTotal() == recipeParamBaseline_) {
            // 应答被认领了、却没有可用的载荷。这是"对端回了一份坏数据"，
            // 不是"还没回来" —— 两者的处置完全不同，混为一谈会把格式问题
            // 当成时序问题一直等下去。
            failRecipeDownload(QStringLiteral("回读应答里没有可用的参数载荷：对端回的格式不对"));
            return;
        }

        const mc::ParamState state = device_->lastParamState();
        const mc::Recipe& want = recipeStatus_.recipe;
        QString mismatch;
        if (static_cast<std::uint8_t>(state.mode) != want.mode) {
            mismatch = QStringLiteral("模式：设备是 %1，配方是 %2")
                           .arg(static_cast<int>(state.mode))
                           .arg(static_cast<int>(want.mode));
        } else if (std::fabs(state.target - want.target) > kRecipeReadBackTolerance) {
            mismatch = QStringLiteral("目标值：设备是 %1，配方是 %2")
                           .arg(static_cast<double>(state.target), 0, 'f', 4)
                           .arg(static_cast<double>(want.target), 0, 'f', 4);
        } else if (std::fabs(state.kp - want.kp) > kRecipeReadBackTolerance) {
            mismatch = QStringLiteral("Kp：设备是 %1，配方是 %2")
                           .arg(static_cast<double>(state.kp), 0, 'f', 4)
                           .arg(static_cast<double>(want.kp), 0, 'f', 4);
        } else if (std::fabs(state.ki - want.ki) > kRecipeReadBackTolerance) {
            mismatch = QStringLiteral("Ki：设备是 %1，配方是 %2")
                           .arg(static_cast<double>(state.ki), 0, 'f', 4)
                           .arg(static_cast<double>(want.ki), 0, 'f', 4);
        } else if (std::fabs(state.kd - want.kd) > kRecipeReadBackTolerance) {
            mismatch = QStringLiteral("Kd：设备是 %1，配方是 %2")
                           .arg(static_cast<double>(state.kd), 0, 'f', 4)
                           .arg(static_cast<double>(want.kd), 0, 'f', 4);
        }
        if (!mismatch.isEmpty()) {
            failRecipeDownload(QStringLiteral("回读与配方不一致（%1）").arg(mismatch));
            return;
        }

        recipeStatus_.phase = RecipeDownloadPhase::Succeeded;
        recipeStatus_.detail =
            QStringLiteral("下装完成：3/3 配置命令已确认，回读逐字段一致（模式 %1，目标 %2，PID %3/%4/%5）")
                .arg(static_cast<int>(state.mode))
                .arg(static_cast<double>(state.target), 0, 'f', 4)
                .arg(static_cast<double>(state.kp), 0, 'f', 4)
                .arg(static_cast<double>(state.ki), 0, 'f', 4)
                .arg(static_cast<double>(state.kd), 0, 'f', 4);
        if (history_ != nullptr) {
            history_->addEvent(static_cast<std::uint64_t>(elapsedMs_),
                               QDateTime::currentMSecsSinceEpoch(),
                               QStringLiteral("配方下装成功：%1 v%2（回读：模式 %3，目标 %4，"
                                              "PID %5/%6/%7）")
                                   .arg(QString::fromStdString(want.name))
                                   .arg(want.version)
                                   .arg(static_cast<int>(state.mode))
                                   .arg(static_cast<double>(state.target), 0, 'f', 4)
                                   .arg(static_cast<double>(state.kp), 0, 'f', 4)
                                   .arg(static_cast<double>(state.ki), 0, 'f', 4)
                                   .arg(static_cast<double>(state.kd), 0, 'f', 4));
        }
        emit recipeDownloadChanged();
        return;
    }

    // 配置命令（0..2）被确认：推进到下一条。第 3 条确认之后不会走到这里，
    // 上面那个分支已经处理掉了。
    ++recipeStatus_.commandsAcked;
    ++recipeStep_;

    if (!startRecipeStep()) {
        return;   // startRecipeStep 内部已经把原因写成 Failed
    }
    emit recipeDownloadChanged();
}

void DeviceManager::failRecipeDownload(const QString& reason) {
    if (recipeStatus_.phase != RecipeDownloadPhase::Sending &&
        recipeStatus_.phase != RecipeDownloadPhase::Verifying) {
        return;
    }
    recipeStatus_.phase = RecipeDownloadPhase::Failed;
    recipeStatus_.detail = reason;
    // 失败也要留痕：配方下装是"这台设备这一段按什么参数跑"的起点，
    // 它到底成没成，事后一定会被问到。
    if (history_ != nullptr) {
        history_->addEvent(static_cast<std::uint64_t>(elapsedMs_),
                           QDateTime::currentMSecsSinceEpoch(),
                           QStringLiteral("配方下装中止（%1 v%2，已确认 %3/3）：%4")
                               .arg(QString::fromStdString(recipeStatus_.recipe.name))
                               .arg(recipeStatus_.recipe.version)
                               .arg(recipeStatus_.commandsAcked)
                               .arg(reason));
    }
    emit recipeDownloadChanged();
}

void DeviceManager::cancelRecipeDownload() {
    // 中止**不等于撤销**：已经下发并被确认过的命令发生在过去，收不回来，
    // 设备上留下的就是最后一次生效的那个值。这句话必须说出来，
    // 否则"点了中止"会被理解成"什么都没变"。
    failRecipeDownload(QStringLiteral(
        "已人工中止。注意：已经下发并被确认的命令不会被撤回，设备保持在那时的值。"));
}

void DeviceManager::setUplinkDropEveryNthByte(int n) {
    const QString subject = QStringLiteral("上行每 %1 字节丢 1（0=关）").arg(n);
    if (!permitted(mc::OperatorAction::InjectFault, subject)) {
        return;
    }
    if (device_ != nullptr) {
        device_->setUplinkDropEveryNthByte(static_cast<std::uint32_t>(n < 0 ? 0 : n));
    }
    audited(mc::OperatorAction::InjectFault, subject, device_ != nullptr,
            device_ != nullptr ? QString() : QStringLiteral("没有设备"));
}

void DeviceManager::setDownlinkDropEveryNthByte(int n) {
    const QString subject = QStringLiteral("下行每 %1 字节丢 1（0=关）").arg(n);
    if (!permitted(mc::OperatorAction::InjectFault, subject)) {
        return;
    }
    if (device_ != nullptr) {
        device_->setDownlinkDropEveryNthByte(static_cast<std::uint32_t>(n < 0 ? 0 : n));
    }
    audited(mc::OperatorAction::InjectFault, subject, device_ != nullptr,
            device_ != nullptr ? QString() : QStringLiteral("没有设备"));
}

void DeviceManager::setUplinkLatencyPolls(int n) {
    const QString subject = QStringLiteral("上行延迟 %1 拍（0=关）").arg(n);
    if (!permitted(mc::OperatorAction::InjectFault, subject)) {
        return;
    }
    if (device_ != nullptr) {
        device_->setUplinkLatencyPolls(static_cast<std::uint32_t>(n < 0 ? 0 : n));
    }
    audited(mc::OperatorAction::InjectFault, subject, device_ != nullptr,
            device_ != nullptr ? QString() : QStringLiteral("没有设备"));
}

void DeviceManager::clearInjectedFaults() {
    const QString subject = QStringLiteral("清除全部注入");
    if (!permitted(mc::OperatorAction::ClearInjectedFaults, subject)) {
        return;
    }
    if (device_ != nullptr) {
        device_->clearInjectedFaults();
    }
    audited(mc::OperatorAction::ClearInjectedFaults, subject, device_ != nullptr,
            device_ != nullptr ? QString() : QStringLiteral("没有设备"));
}

void DeviceManager::enableTimingDiagnostics(bool enabled, std::size_t capacity) {
    tickBusyUs_.clear();
    tickIntervalUs_.clear();
    timingSampleCount_ = 0U;
    lastTimingTickMs_ = -1;
    timingEnabled_ = enabled;

    if (!enabled || capacity == 0U) {
        timingCapacity_ = 0U;
        return;
    }

    timingCapacity_ = capacity;
    // 预置容量：记样本时用下标写入而不是 append，避免"采样本身在分配内存"
    // 这件事污染被测量的那一拍。
    tickBusyUs_.resize(static_cast<int>(capacity));
    tickIntervalUs_.resize(static_cast<int>(capacity));
}

void DeviceManager::recordTimingSample(qint64 busyUs, qint64 intervalUs) {
    if (timingCapacity_ == 0U) {
        return;
    }
    const int index = static_cast<int>(timingSampleCount_ % timingCapacity_);
    tickBusyUs_[index] = busyUs;
    tickIntervalUs_[index] = intervalUs;
    ++timingSampleCount_;
}

void DeviceManager::raiseAlarm(std::uint16_t code, const QString& detail) {
    const mc::AlarmEntry& entry =
        alarms_.raise(code, detail.toStdString(), static_cast<std::uint64_t>(elapsedMs_),
                      static_cast<std::uint64_t>(QDateTime::currentMSecsSinceEpoch()));

    // 两处都记，分工不同：
    //   event 表 —— **按时间排的流水**，报警与配方下装等事件混在同一条时间线上
    //               （"出这条报警之前刚做了什么"只能从这里看出来）；
    //   alarm 表 —— **结构化的一条**，带代码/等级/确认状态（"这一段里有多少条
    //               Error 以上没确认"只能从这里查出来）。
    // 只留前者等于把代码和等级丢了，只留后者等于丢掉跨类事件的先后关系。
    if (history_ != nullptr) {
        const QString text = alarmText(code).append(
            entry.detail.empty() ? QString()
                                 : QStringLiteral("（%1）").arg(QString::fromStdString(entry.detail)));
        history_->addEvent(static_cast<std::uint64_t>(entry.lastTMs),
                           static_cast<qint64>(entry.wallMs), text);
        history_->upsertAlarm(entry);
    }

    emit alarmRaised(code, alarmText(code));
    emit alarmsChanged();

    // 日志的级别跟着报警**自己的等级**走，而不是一律 Error：
    // "心跳漏了一拍"与"设备故障锁存"在日志里长成同一个样子，就等于把
    // 日志里最有用的那个维度（严重程度）抹掉了。映射表在这里写一次，
    // 与报警簿里那份等级是同一个来源（alarmSpec）。
    const mc::AlarmSpec* spec = mc::alarmSpec(code);
    mc::LogLevel level = mc::LogLevel::Info;
    if (spec != nullptr) {
        switch (spec->level) {
            case mc::AlarmLevel::Info: level = mc::LogLevel::Info; break;
            case mc::AlarmLevel::Warning: level = mc::LogLevel::Warn; break;
            case mc::AlarmLevel::Error:
            case mc::AlarmLevel::Fatal: level = mc::LogLevel::Error; break;
        }
    }
    logEvent(level, "alarm",
             detail.isEmpty() ? alarmText(code)
                              : QStringLiteral("%1（%2）").arg(alarmText(code), detail));
}

void DeviceManager::syncAlarmToHistory(std::uint16_t code) {
    if (history_ == nullptr) {
        return;
    }
    // 确认状态是**会变**的（未确认 → 已确认），落库也要跟着变，否则重启之后
    // 从库里读回来的还是"未确认"，而现实里人早就确认过了。
    // 从后往前找：同码可能有多条（一条已恢复、另一条后来的），要同步的是最近那条。
    const std::vector<mc::AlarmEntry>& entries = alarms_.entries();
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        if (it->code == code) {
            history_->upsertAlarm(*it);
            return;
        }
    }
}

void DeviceManager::onTick() {
    if (device_ == nullptr) {
        return;
    }

    if (!timingEnabled_) {
        // 产品路径：一层都不多包。诊断关闭时这里与以前完全一样
        // —— 一个 nullptr 判断，不是"看起来关了其实还在测"。
        tickOnce();
        return;
    }

    const qint64 t0 = timingClock_.nsecsElapsed();
    tickOnce();
    const qint64 t1 = timingClock_.nsecsElapsed();

    const qint64 nowMs = clock_.elapsed();
    if (lastTimingTickMs_ >= 0) {
        // 第一拍没有"上一拍"，间隔样本必须从第二拍才开始记 —— 否则会凭空录进
        // 一个"自连接以来的时长"，把 P99 拉成一个毫无意义的数字。
        recordTimingSample((t1 - t0) / 1000, (nowMs - lastTimingTickMs_) * 1000);
    }
    lastTimingTickMs_ = nowMs;
}

void DeviceManager::tickOnce() {
    const double nowMs = static_cast<double>(clock_.elapsed());
    const DeviceTick tick = device_->tick(static_cast<std::uint64_t>(nowMs));
    elapsedMs_ = nowMs;
    ++ticks_;

    // 链路在下面被关掉（例如传输断开）时，这里就收摊 —— 不停在一个"连着但没数据"的假状态。
    if (!tick.linkOpen) {
        timer_.stop();
        connected_ = false;
        logEvent(mc::LogLevel::Warn, "link", QStringLiteral("链路已关闭（设备侧不再可读写）"));
        // 链路都没了，进行中的下装不可能再推进 —— 当场结清，不要让界面停在
        // "等确认…"上：那会让人以为还有戏。
        failRecipeDownload(QStringLiteral("链路已关闭，下装中止"));
        // 正在跑的程序同理，而且更要当场停：自动流程停在一个"等确认…"上，
        // 是最容易被误读成"它还在正常跑"的状态。
        if (sequencer_.status().phase == mc::SequencePhase::Running) {
            sequenceSeq_ = 0U;
            sequenceStepName_ = nullptr;
            sequencer_.abort("链路已关闭，程序中止");
            emit sequenceChanged();
        }
        emit connectionChanged(false);
        emit commandLogChanged();
        return;
    }

    if (tick.hasTelemetry) {
        TelemetrySample sample;
        sample.tMs = nowMs;
        sample.velocityDegPerSec = tick.telemetry.velocityDegPerSec;
        sample.positionDeg = tick.telemetry.positionDeg;
        sample.target = tick.telemetry.target;
        sample.state = tick.telemetry.state;
        sample.faultCode = tick.telemetry.faultCode;
        sample.pwmOutput = tick.telemetry.pwmOutput;
        telemetry_.push(sample);
        // 落库走**同一条**数据（不是另取一份遥测）：历史里的曲线与当时屏幕上
        // 看到的曲线必须是同一组数字，否则事后追查会追到一个界面从没显示过的值。
        if (history_ != nullptr) {
            history_->addSample(sample);
        }
    }

    if (history_ != nullptr) {
        history_->tick(QDateTime::currentMSecsSinceEpoch());
    }

    // --- 报警：只报"安全事件"，并按**等级**定处置 --------------------------
    // 链路失效 / 会话作废 / 命令超时：Error 级（拦启动，要人工确认）。
    if (tick.sessionLost) {
        raiseAlarm(mc::kAlarmCodeSessionLost);
    }
    if (tick.timeoutsFaulted > 0U) {
        raiseAlarm(mc::kAlarmCodeCommandTimeout,
                   QStringLiteral("本拍 %1 条").arg(static_cast<qulonglong>(tick.timeoutsFaulted)));
    }
    // 链路监控的判定是个**状态**，报警记的是"发生了一件事"：只在跳变沿上记一次。
    // 按拍记会把 occurrences 刷成几百次，那等于把"发生过几次"这个数字废掉。
    if (tick.linkDown != lastLinkDown_) {
        if (tick.linkDown) {
            raiseAlarm(mc::kAlarmCodeLinkLost);
        } else {
            // 条件消失：标成"已恢复"。条目**留在清单上**等人工确认 ——
            // 链路断过这件事不该因为链路自己好了就变得没人知道。
            if (alarms_.clear(mc::kAlarmCodeLinkLost)) {
                syncAlarmToHistory(mc::kAlarmCodeLinkLost);
                emit alarmsChanged();
            }
        }
        lastLinkDown_ = tick.linkDown;
    }
    // 心跳漏拍只是 Warning：记下来、提示一下，不要求确认、不拦启动。
    if (tick.droppedProbes > 0U) {
        raiseAlarm(mc::kAlarmCodeHeartbeatDropped,
                   QStringLiteral("本拍 %1 拍").arg(static_cast<qulonglong>(tick.droppedProbes)));
    }
    // 会话重新就绪 → "会话作废"这条标成已恢复（条目同样留着等确认）。
    const bool sessionReady = device_->sessionReady();
    if (sessionReady && !lastSessionReady_ && alarms_.clear(mc::kAlarmCodeSessionLost)) {
        syncAlarmToHistory(mc::kAlarmCodeSessionLost);
        emit alarmsChanged();
    }
    lastSessionReady_ = sessionReady;

    const bool latched = (device_->faultLatched());
    if (latched && !lastFaultLatched_) {
        latchedAlarmCode_ = mc::alarmCodeForFault(device_->faultCode());
        raiseAlarm(latchedAlarmCode_);
    } else if (!latched && lastFaultLatched_) {
        // 降沿要做**三件事**：把那条设备故障标成"已恢复"（它现在确实不挂着了）、
        // 顺手确认掉它、再补一条信息级的记录。
        //
        // 为什么可以顺手确认：锁存**只会**被人工确认解开（设备侧的状态机规定
        // 故障锁存仅由 RecoveryOk 清除，而 RecoveryOk 来自 CLEAR_FAULT 之后的
        // 自检）。也就是说这个降沿本身就是"有人确认过了"的产物 ——
        // 那条报警此刻再显示成"未确认、拦启动"，与事实相反的还会把启动闸门
        // 卡住，让人以为确认没生效。
        //
        // 只标恢复不确认，清单上看不出"曾经出过什么"；只记一条信息，那条告警
        // 会一直显示成"还挂着"。
        if (latchedAlarmCode_ != 0U) {
            alarms_.clear(latchedAlarmCode_);
            (void)alarms_.acknowledge(
                latchedAlarmCode_, static_cast<std::uint64_t>(QDateTime::currentMSecsSinceEpoch()));
            // "已恢复 + 已确认"这两笔改动都要落库：只落前者的话，重启后从库里
            // 读回来的是"恢复了但仍未确认"，而现实里人早就确认过了。
            syncAlarmToHistory(latchedAlarmCode_);
            latchedAlarmCode_ = 0U;
        }
        raiseAlarm(mc::kAlarmCodeFaultCleared);
    }
    lastFaultLatched_ = latched;

    if (tick.commandLogChanged) {
        // 命令记录会**变**（等待应答 → 已确认/被拒/超时），所以按 (会话, seq) 覆盖写。
        // 整本日志都过一遍（上限 64 条）：只处理"新增的那几条"要额外维护一套
        // "见到过哪些 seq"的状态，而那套状态一旦与日志的淘汰策略不一致就会漏记。
        if (history_ != nullptr) {
            for (const CommandRecord& record : device_->commandLog()) {
                history_->upsertCommand(record.sentAtMs, record.seq,
                                        QString::fromStdString(record.name),
                                        static_cast<int>(record.status),
                                        static_cast<int>(record.nackReason));
            }
        }
        // 命令的**归宿变化**写进日志。按 (seq → 上次记过的归宿) 过滤，
        // 只记变化的那一次：这本日志每拍都被整本走一遍，不过滤会把同一件事写几百遍。
        // 级别按归宿定：等待应答是 Debug（它后面一定还有一条"结果"），
        // 出了结果才是 Info/Warn —— 被拒与超时是 Warn，它们在排查时最常被找。
        for (const CommandRecord& record : device_->commandLog()) {
            const auto found = loggedCommandStatus_.find(record.seq);
            const CommandStatus previous =
                (found == loggedCommandStatus_.end()) ? CommandStatus::NotSent : found->second;
            if (found != loggedCommandStatus_.end() && previous == record.status) {
                continue;
            }
            loggedCommandStatus_[record.seq] = record.status;
            const QString line = commandEventText(record);
            switch (record.status) {
                case CommandStatus::Nacked:
                case CommandStatus::TimedOut:
                    logEvent(mc::LogLevel::Warn, "cmd", line);
                    break;
                case CommandStatus::Acked:
                    logEvent(mc::LogLevel::Info, "cmd", line);
                    break;
                case CommandStatus::Awaiting:
                case CommandStatus::Unidirectional:
                case CommandStatus::NotSent:
                    logEvent(mc::LogLevel::Debug, "cmd", line);
                    break;
            }
        }
        emit commandLogChanged();
    }

    // 配方下装的推进放在命令结算**之后**：这一拍的命令记录已经是最终状态，
    // 推进器看到的就是最终裁决，不会出现"同一拍里刚看到等待应答、转眼又看到已确认"
    // 这种自相矛盾的判断。
    driveRecipeDownload();

    // 任务调度同理，而且它更依赖这个顺序：一条命令的结局必须在**发下一步之前**
    // 被结算掉，否则同一条记录会被当成刚发出去的那一步的结局。
    driveSequence();

    // 回零的观察放在最后：它只看设备状态，不发任何命令，也不该影响上面两者的
    // 判定顺序。用户手动回零与程序里的 HOME 步走的是同一段观察逻辑。
    driveHoming();

    if ((ticks_ % static_cast<std::uint64_t>(kUiRefreshDivider)) == 0U) {
        emit statsRefreshed();
    }
}

}  // namespace mchost
