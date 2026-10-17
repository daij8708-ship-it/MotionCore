#include "mc/sequence.hpp"

#include <cmath>
#include <utility>

#include "mc/crc16.hpp"
#include "mc/payload.hpp"
#include "mc/protocol.hpp"

namespace mc {
namespace {

// 三个增益共用一条判定，与配方那边同一条规则、同一个上界。
// 分别写三遍就会出现"给 Kp 加了界、Ki/Kd 忘了加"这种最常见的漏法。
SequenceError validateGain(float gain) noexcept {
    if (!std::isfinite(gain)) {
        return SequenceError::GainNotFinite;
    }
    if (gain < 0.0F) {
        return SequenceError::GainNegative;
    }
    if (gain > kSanityMaxPidGain) {
        return SequenceError::GainTooLarge;
    }
    return SequenceError::None;
}

}  // namespace

const char* sequenceStepKindToken(SequenceStepKind kind) noexcept {
    switch (kind) {
        case SequenceStepKind::SetMode:   return "SET_MODE";
        case SequenceStepKind::SetPid:    return "SET_PID";
        case SequenceStepKind::SetTarget: return "SET_TARGET";
        case SequenceStepKind::Start:     return "START";
        case SequenceStepKind::Stop:      return "STOP";
        case SequenceStepKind::Wait:      return "WAIT";
        case SequenceStepKind::WaitReady: return "WAIT_READY";
        case SequenceStepKind::Home:      return "HOME";
    }
    return "?";
}

const char* sequenceErrorName(SequenceError error) noexcept {
    switch (error) {
        case SequenceError::None:                return "None";
        case SequenceError::Truncated:           return "Truncated";
        case SequenceError::BadStepCount:        return "BadStepCount";
        case SequenceError::BadKind:             return "BadKind";
        case SequenceError::TrailingBytes:       return "TrailingBytes";
        case SequenceError::EmptyName:           return "EmptyName";
        case SequenceError::NameTooLong:         return "NameTooLong";
        case SequenceError::BadVersion:          return "BadVersion";
        case SequenceError::NoSteps:             return "NoSteps";
        case SequenceError::TooManySteps:        return "TooManySteps";
        case SequenceError::BadMode:             return "BadMode";
        case SequenceError::TargetNotFinite:     return "TargetNotFinite";
        case SequenceError::TargetOutOfRange:    return "TargetOutOfRange";
        case SequenceError::GainNotFinite:       return "GainNotFinite";
        case SequenceError::GainNegative:        return "GainNegative";
        case SequenceError::GainTooLarge:        return "GainTooLarge";
        case SequenceError::WaitWithoutDuration: return "WaitWithoutDuration";
        case SequenceError::WaitTooLong:         return "WaitTooLong";
        case SequenceError::NoteTooLong:         return "NoteTooLong";
        case SequenceError::HomeSpeedMissing:    return "HomeSpeedMissing";
        case SequenceError::HomeSpeedTooLarge:   return "HomeSpeedTooLarge";
        case SequenceError::HomeTimeoutMissing:  return "HomeTimeoutMissing";
        case SequenceError::HomeTimeoutTooLarge: return "HomeTimeoutTooLarge";
        case SequenceError::HomeWithoutWaitReady: return "HomeWithoutWaitReady";
    }
    return "?";
}

SequenceError validateSequenceStep(const SequenceStep& step, std::uint8_t effectiveMode) noexcept {
    if (step.note.size() > kMaxStepNoteBytes) {
        return SequenceError::NoteTooLong;
    }

    switch (step.kind) {
        case SequenceStepKind::SetMode:
            if (step.mode > kMaxRunMode) {
                return SequenceError::BadMode;
            }
            return SequenceError::None;

        case SequenceStepKind::SetTarget:
            if (!std::isfinite(step.target)) {
                return SequenceError::TargetNotFinite;
            }
            // 按**当时生效的模式**判量纲：速度环的上界是 deg/s，位置环是 deg。
            // 不判这一条的话，一个把 200 当成 deg 写进速度环的程序会一路跑到设备那里
            // 才被拒，而那时操作员看到的只是"某一步被拒"。
            if (effectiveMode == 1U) {
                if (std::fabs(step.target) > kSanityMaxAbsVelocityDegPerSec) {
                    return SequenceError::TargetOutOfRange;
                }
            } else if (effectiveMode == 2U) {
                if (std::fabs(step.target) > kSanityMaxAbsPositionDeg) {
                    return SequenceError::TargetOutOfRange;
                }
            }
            // effectiveMode == 0（停止控制）时不判上界：这个目标值本来就不参与运算，
            // 它到底有没有意义是"模式选错了"那个问题，不在这条规则的管辖范围里。
            return SequenceError::None;

        case SequenceStepKind::SetPid: {
            for (const float gain : {step.kp, step.ki, step.kd}) {
                const SequenceError error = validateGain(gain);
                if (error != SequenceError::None) {
                    return error;
                }
            }
            return SequenceError::None;
        }

        case SequenceStepKind::Start:
        case SequenceStepKind::Stop:
            return SequenceError::None;

        case SequenceStepKind::Home:
            // 回零速度必须**显式给一个正数**：0 不是"慢慢走"，是永远走不到，
            // 而它在界面上看起来和填对了没什么两样。
            if (!std::isfinite(step.target) || step.target <= 0.0F) {
                return SequenceError::HomeSpeedMissing;
            }
            if (step.target > kSanityMaxHomeSpeedDegPerSec) {
                return SequenceError::HomeSpeedTooLarge;
            }
            if (step.waitMs == 0U) {
                return SequenceError::HomeTimeoutMissing;
            }
            if (step.waitMs > kMaxWaitMs) {
                return SequenceError::HomeTimeoutTooLarge;
            }
            return SequenceError::None;

        case SequenceStepKind::Wait:
            // 零等待是"写漏了"的典型样子，而且它跑起来毫无症状 —— 直接拒掉。
            if (step.waitMs == 0U) {
                return SequenceError::WaitWithoutDuration;
            }
            if (step.waitMs > kMaxWaitMs) {
                return SequenceError::WaitTooLong;
            }
            return SequenceError::None;

        case SequenceStepKind::WaitReady:
            // 这里的 waitMs 是**超时上限**而不是"要等多久"：等到就往下走，
            // 等不到就得有个头，否则一部程序可能永远停在"等设备停下"上。
            if (step.waitMs == 0U) {
                return SequenceError::WaitWithoutDuration;
            }
            if (step.waitMs > kMaxWaitMs) {
                return SequenceError::WaitTooLong;
            }
            return SequenceError::None;
    }
    return SequenceError::None;
}

SequenceError validateSequence(const SequenceProgram& program, std::uint8_t initialMode) noexcept {
    if (program.name.empty()) {
        return SequenceError::EmptyName;
    }
    if (program.name.size() > kMaxSequenceNameBytes) {
        return SequenceError::NameTooLong;
    }
    if (program.version == 0U) {
        return SequenceError::BadVersion;
    }
    if (program.steps.empty()) {
        return SequenceError::NoSteps;
    }
    if (program.steps.size() > kMaxSequenceSteps) {
        return SequenceError::TooManySteps;
    }

    std::uint8_t mode = initialMode;
    for (std::size_t index = 0U; index < program.steps.size(); ++index) {
        const SequenceStep& step = program.steps[index];
        const SequenceError error = validateSequenceStep(step, mode);
        if (error != SequenceError::None) {
            return error;
        }
        // 模式按程序顺序往下传：同一个数字在不同模式里是两种量纲，
        // 所以"当前模式"必须随步推进，不能整份程序用启动那一刻的那一个。
        if (step.kind == SequenceStepKind::SetMode) {
            mode = step.mode;
        }
        // 回零的完成只能从设备状态看出来（应答只说明"开始了"），
        // 所以它后面必须有一等在 Ready 上的那一步。见头文件的说明。
        if (step.kind == SequenceStepKind::Home) {
            const bool hasWaitReady = (index + 1U < program.steps.size()) &&
                                      program.steps[index + 1U].kind == SequenceStepKind::WaitReady;
            if (!hasWaitReady) {
                return SequenceError::HomeWithoutWaitReady;
            }
        }
    }
    return SequenceError::None;
}

const char* stepOutcomeName(StepOutcome outcome) noexcept {
    switch (outcome) {
        case StepOutcome::Acked:    return "Acked";
        case StepOutcome::Nacked:   return "Nacked";
        case StepOutcome::TimedOut: return "TimedOut";
        case StepOutcome::NotSent:  return "NotSent";
        case StepOutcome::Waiting:  return "Waiting";
    }
    return "?";
}

const char* sequencePhaseName(SequencePhase phase) noexcept {
    switch (phase) {
        case SequencePhase::Idle:      return "Idle";
        case SequencePhase::Running:   return "Running";
        case SequencePhase::Succeeded: return "Succeeded";
        case SequencePhase::Failed:    return "Failed";
        case SequencePhase::Aborted:   return "Aborted";
    }
    return "?";
}

bool Sequencer::load(SequenceProgram program, std::uint8_t initialMode) noexcept {
    const SequenceError error = validateSequence(program, initialMode);
    loadError_ = error;
    // 只说错误名，不猜"是第几步"：错误名本身就是具体的（BadMode / WaitWithoutDuration…），
    // 而"第几步"由界面按行显示，那里本来就能一行行看。猜错了比不说更糟。
    loadErrorDetail_ = error == SequenceError::None ? std::string()
                                                    : sequenceErrorName(error);
    if (error != SequenceError::None) {
        return false;
    }

    program_ = std::move(program);
    loaded_ = true;
    // 装载会**清掉上一次的运行态**：上一部程序的结局留在屏幕上是好事，
    // 但把它当成"这一部也这样跑了"就是另一回事了。
    status_ = SequenceStatus{};
    reports_.clear();
    currentStepIndex_ = 0U;
    awaitingDispatch_ = false;
    return true;
}

bool Sequencer::start(std::uint64_t nowMs) {
    if (!loaded_) {
        status_.detail = "没有装载任何程序";
        return false;
    }
    if (status_.phase == SequencePhase::Running) {
        // 一部没跑完就再启一部，等于"上一条命令还在飞的时候又发一条"。
        // 铁律①在这里守门。
        status_.detail = "已经有一部程序在跑：先等它结束或中止它";
        return false;
    }

    status_ = SequenceStatus{};
    status_.phase = SequencePhase::Running;
    status_.stepIndex = 0U;
    status_.detail = "开始执行";
    reports_.clear();
    currentStepIndex_ = 0U;
    awaitingDispatch_ = false;
    nowMs_ = nowMs;
    armCurrentStep();
    return true;
}

void Sequencer::abort(std::string reason) {
    if (status_.phase != SequencePhase::Running) {
        return;
    }
    // 中止不是撤销：已经下发过并被确认的命令发生在过去，收不回来。
    // 这句话由界面负责说给操作员听（见任务页）。
    finish(SequencePhase::Aborted, StepOutcome::NotSent,
           reason.empty() ? std::string("人工中止") : std::move(reason));
}

bool Sequencer::awaitingDispatch() const noexcept {
    if (status_.phase != SequencePhase::Running || !awaitingDispatch_) {
        return false;
    }
    // Wait / WaitReady 不产生下行报文，所以它们永远不"等发送"。
    const SequenceStepKind kind = program_.steps[currentStepIndex_].kind;
    return kind != SequenceStepKind::Wait && kind != SequenceStepKind::WaitReady;
}

void Sequencer::reportDispatched(bool sent, std::string detail) {
    if (status_.phase != SequencePhase::Running || !awaitingDispatch_) {
        return;
    }
    const SequenceStepKind kind = program_.steps[currentStepIndex_].kind;
    awaitingDispatch_ = false;
    if (!sent) {
        finish(SequencePhase::Failed, StepOutcome::NotSent,
               detail.empty() ? std::string("这一步没有发出去") : std::move(detail));
        return;
    }
    status_.waitingOutcome = true;
    status_.detail =
        std::string("已发出 ") + sequenceStepKindToken(kind) + "，等设备确认…";
}

void Sequencer::reportOutcome(StepOutcome outcome, std::string detail) {
    if (status_.phase != SequencePhase::Running || !status_.waitingOutcome) {
        return;
    }
    status_.waitingOutcome = false;

    if (outcome != StepOutcome::Acked) {
        // 记录这一步的结论文本：连同"哪一步"一起说，否则操作员只知道
        // "程序失败了"，不知道该去看哪一步。
        finish(SequencePhase::Failed, outcome,
               std::string(sequenceStepKindToken(program_.steps[currentStepIndex_].kind)) +
                   " 未获确认：" + (detail.empty() ? stepOutcomeName(outcome) : detail));
        return;
    }

    reports_.push_back(StepReport{currentStepIndex_, program_.steps[currentStepIndex_].kind,
                                  StepOutcome::Acked, detail});
    advance();
}

void Sequencer::reportDeviceReady(bool ready) noexcept {
    deviceReady_ = ready;
}

void Sequencer::tick(std::uint64_t nowMs) {
    // 引擎没有自己的钟：每一次推进都记住调用方给的时刻，等待步的到点时刻由它算。
    nowMs_ = nowMs;
    if (status_.phase != SequencePhase::Running) {
        return;
    }

    const SequenceStep& step = program_.steps[currentStepIndex_];

    if (step.kind == SequenceStepKind::Wait || step.kind == SequenceStepKind::WaitReady) {
        const bool conditional = (step.kind == SequenceStepKind::WaitReady);
        // WaitReady 满足条件就地成功，而且**不等满超时**：超时只是上限。
        if (conditional && deviceReady_) {
            reports_.push_back(
                StepReport{currentStepIndex_, step.kind, StepOutcome::Acked, "设备已回到就绪"});
            status_.waitUntilMs = 0U;
            advance();
            return;
        }
        if (status_.waitUntilMs == 0U) {
            status_.waitUntilMs = nowMs_ + step.waitMs;
            status_.detail = conditional
                                 ? std::string("等设备回到就绪，最多 ") +
                                       std::to_string(step.waitMs) + " ms"
                                 : std::string("等待 ") + std::to_string(step.waitMs) + " ms";
            return;
        }
        if (nowMs_ < status_.waitUntilMs) {
            return;
        }
        if (conditional) {
            // 等不到就得停：一部程序永远停在"等设备停下"上，比报错更难查。
            finish(SequencePhase::Failed, StepOutcome::TimedOut,
                   "等设备回到就绪超时（设备可能还在减速、或者已经不在就绪态）");
            return;
        }
        // 盲等到点了：这一步没有设备参与，所以它就地成功。
        reports_.push_back(StepReport{currentStepIndex_, step.kind, StepOutcome::Acked, "等待结束"});
        status_.waitUntilMs = 0U;
        advance();
        return;
    }

    if (!awaitingDispatch_ && !status_.waitingOutcome) {
        // 该发这一步了。**必须由调用方主动来取**：引擎不自己发命令，
        // 因为它不知道链路、会话、门禁现在是什么样。
        awaitingDispatch_ = true;
    }
}

void Sequencer::advance() {
    status_.waitUntilMs = 0U;
    ++status_.completedSteps;
    ++currentStepIndex_;

    if (currentStepIndex_ >= program_.steps.size()) {
        finish(SequencePhase::Succeeded, StepOutcome::Acked,
               std::string("全部 ") + std::to_string(program_.steps.size()) + " 步都已确认");
        return;
    }

    status_.stepIndex = currentStepIndex_;
    status_.detail = std::string("下一步：") +
                     sequenceStepKindToken(program_.steps[currentStepIndex_].kind);
    armCurrentStep();
}

void Sequencer::armCurrentStep() {
    const SequenceStepKind kind = program_.steps[currentStepIndex_].kind;
    if (kind == SequenceStepKind::Wait || kind == SequenceStepKind::WaitReady) {
        // 等待类步骤不走"发送"这条路：它们只等条件/时间。等 tick 把它们推到点。
        status_.waitUntilMs = 0U;
        awaitingDispatch_ = false;
        return;
    }
    awaitingDispatch_ = true;
}

void Sequencer::finish(SequencePhase phase, StepOutcome outcome, std::string detail) {
    if (status_.phase != SequencePhase::Running) {
        return;
    }
    status_.phase = phase;
    status_.waitingOutcome = false;
    status_.waitUntilMs = 0U;
    awaitingDispatch_ = false;
    status_.detail = detail;

    // 只有失败与中止才记一条流水（成功那条在 advance 里已经写过了）。
    // 这里访问 currentStepIndex_ 是安全的：Succeeded 那条路不会走到下面，
    // 而其余三种终态的 currentStepIndex_ 都指向一部真实存在的步。
    if (phase != SequencePhase::Succeeded) {
        reports_.push_back(StepReport{currentStepIndex_, program_.steps[currentStepIndex_].kind,
                                      outcome, detail});
    }
}

std::uint16_t sequenceChecksum(const SequenceProgram& program) noexcept {
    const std::vector<std::uint8_t> bytes = encodeSequence(program);
    return crc16(bytes.data(), bytes.size());
}

// ---------------------------------------------------------------------------
// 规范字节
// ---------------------------------------------------------------------------
namespace {

// u64 的小端读写。协议头里只有 16/32 位那一对（帧里没有 64 位字段），
// 所以在这里就地写一份 —— 但**只写一份**，编码与解码共用，避免两边错开。
void writeLe64(std::uint8_t* p, std::uint64_t v) noexcept {
    writeLe32(p, static_cast<std::uint32_t>(v & 0xFFFFFFFFULL));
    writeLe32(p + 4U, static_cast<std::uint32_t>((v >> 32U) & 0xFFFFFFFFULL));
}

std::uint64_t readLe64(const std::uint8_t* p) noexcept {
    return static_cast<std::uint64_t>(readLe32(p)) |
           (static_cast<std::uint64_t>(readLe32(p + 4U)) << 32U);
}

bool kindIsKnown(std::uint8_t raw) noexcept {
    // **按枚举的最后一项判**，不写死数字：加了新步类型却忘了改这里，
    // 会让新步子存得进去、读不出来（而且读到的是 BadKind，看起来像数据坏了）。
    return raw <= static_cast<std::uint8_t>(SequenceStepKind::Home);
}

}  // namespace

std::vector<std::uint8_t> encodeSequence(const SequenceProgram& program) {
    // 两遍算长度：先算总长再填。逐字段显式序列化，**不对结构体做 memcpy** ——
    // 结构体里的 padding 是 ABI 细节，把它写进文件等于让库文件绑死编译器。
    std::size_t total = 2U + program.name.size() + 2U + 2U;
    for (const SequenceStep& step : program.steps) {
        total += kSequenceStepFixedBytes + step.note.size();
    }

    std::vector<std::uint8_t> bytes(total, 0U);
    std::size_t p = 0U;

    writeLe16(bytes.data() + p, static_cast<std::uint16_t>(program.name.size()));
    p += 2U;
    for (const char raw : program.name) {
        bytes[p++] = static_cast<std::uint8_t>(raw);
    }

    writeLe16(bytes.data() + p, program.version);
    p += 2U;
    writeLe16(bytes.data() + p, static_cast<std::uint16_t>(program.steps.size()));
    p += 2U;

    for (const SequenceStep& step : program.steps) {
        bytes[p++] = static_cast<std::uint8_t>(step.kind);
        bytes[p++] = step.mode;
        writeLe64(bytes.data() + p, step.waitMs);
        p += 8U;
        writeLeFloat(bytes.data() + p, step.target);
        p += 4U;
        writeLeFloat(bytes.data() + p, step.kp);
        p += 4U;
        writeLeFloat(bytes.data() + p, step.ki);
        p += 4U;
        writeLeFloat(bytes.data() + p, step.kd);
        p += 4U;
        writeLe16(bytes.data() + p, static_cast<std::uint16_t>(step.note.size()));
        p += 2U;
        for (const char raw : step.note) {
            bytes[p++] = static_cast<std::uint8_t>(raw);
        }
    }
    return bytes;
}

SequenceError decodeSequence(const std::uint8_t* data, std::size_t len,
                             SequenceProgram& out) noexcept {
    out = SequenceProgram{};
    if (len < 6U) {
        return SequenceError::Truncated;
    }

    std::size_t p = 0U;
    const std::size_t nameLen = readLe16(data + p);
    p += 2U;
    if (p + nameLen + 4U > len) {
        return SequenceError::Truncated;
    }
    out.name.assign(reinterpret_cast<const char*>(data + p), nameLen);
    p += nameLen;

    out.version = readLe16(data + p);
    p += 2U;

    const std::size_t stepCount = readLe16(data + p);
    p += 2U;
    // 步数是格式层面的界，不是语义规则：一个坏字节读出来的可能是 65535，
    // 不去拦它就会按那个数去 reserve 一大块内存。
    if (stepCount == 0U || stepCount > kMaxSequenceSteps) {
        return SequenceError::BadStepCount;
    }

    out.steps.reserve(stepCount);
    for (std::size_t i = 0U; i < stepCount; ++i) {
        if (p + kSequenceStepFixedBytes > len) {
            return SequenceError::Truncated;
        }
        SequenceStep step;
        const std::uint8_t kind = data[p++];
        if (!kindIsKnown(kind)) {
            // 不认识的类型**不猜成一某个默认值**：猜出来的那一步会照着错的意思跑。
            return SequenceError::BadKind;
        }
        step.kind = static_cast<SequenceStepKind>(kind);
        step.mode = data[p++];
        step.waitMs = readLe64(data + p);
        p += 8U;
        step.target = readLeFloat(data + p);
        p += 4U;
        step.kp = readLeFloat(data + p);
        p += 4U;
        step.ki = readLeFloat(data + p);
        p += 4U;
        step.kd = readLeFloat(data + p);
        p += 4U;

        const std::size_t noteLen = readLe16(data + p);
        p += 2U;
        if (p + noteLen > len) {
            return SequenceError::Truncated;
        }
        step.note.assign(reinterpret_cast<const char*>(data + p), noteLen);
        p += noteLen;
        out.steps.push_back(std::move(step));
    }

    if (p != len) {
        return SequenceError::TrailingBytes;
    }
    return SequenceError::None;
}

}  // namespace mc
