#include "mc_host/sequence_text.hpp"

namespace mchost {

QString sequenceErrorText(mc::SequenceError error) {
    switch (error) {
        case mc::SequenceError::None:
            return QStringLiteral("没有错误");
        case mc::SequenceError::Truncated:
            return QStringLiteral("字节流不完整：读到一个字段就没了");
        case mc::SequenceError::BadStepCount:
            return QStringLiteral("步数不合法（0 步或超过上限）");
        case mc::SequenceError::BadKind:
            return QStringLiteral("步类型不认识：这份字节流与程序结构对不上");
        case mc::SequenceError::TrailingBytes:
            return QStringLiteral("读完一部程序还有多余字节");
        case mc::SequenceError::EmptyName:
            return QStringLiteral("名称不能为空：名字是人选它来跑的依据");
        case mc::SequenceError::NameTooLong:
            return QStringLiteral("名称太长");
        case mc::SequenceError::BadVersion:
            return QStringLiteral("版本号不能是 0（0 只可能是「没填」）");
        case mc::SequenceError::NoSteps:
            return QStringLiteral("一部程序至少要一步");
        case mc::SequenceError::TooManySteps:
            return QStringLiteral("步数超过上限：太长就没人核得完了");
        case mc::SequenceError::BadMode:
            return QStringLiteral("模式越界（只有 0 停止控制 / 1 速度环 / 2 位置环）");
        case mc::SequenceError::TargetNotFinite:
            return QStringLiteral("目标值不是有限数（NaN / 无穷）");
        case mc::SequenceError::TargetOutOfRange:
            return QStringLiteral("目标值超出当前模式的上界");
        case mc::SequenceError::GainNotFinite:
            return QStringLiteral("PID 增益不是有限数");
        case mc::SequenceError::GainNegative:
            return QStringLiteral("PID 增益不能是负数");
        case mc::SequenceError::GainTooLarge:
            return QStringLiteral("PID 增益超出上界");
        case mc::SequenceError::WaitWithoutDuration:
            return QStringLiteral("等待步没写时长：零等待多半是漏填了");
        case mc::SequenceError::WaitTooLong:
            return QStringLiteral("等待步超过上限（10 分钟）");
        case mc::SequenceError::HomeSpeedMissing:
            return QStringLiteral("回零步没给速度（或给了 0 / 负数）：0 是永远走不到的回零");
        case mc::SequenceError::HomeSpeedTooLarge:
            return QStringLiteral("回零速度超出上限：回零是朝机械零点走，快不是优点");
        case mc::SequenceError::HomeTimeoutMissing:
            return QStringLiteral("回零步没给时限：走不到的时候它会一直挂着，而"
                                  "「挂着」和「还在走」在界面上分不出来");
        case mc::SequenceError::HomeTimeoutTooLarge:
            return QStringLiteral("回零时限超过上限（10 分钟）");
        case mc::SequenceError::HomeWithoutWaitReady:
            return QStringLiteral("回零步后面必须紧跟一步「等设备就绪」："
                                  "HOME 的应答只说明回零开始了，"
                                  "「零点建立」只能从设备状态看出来");
        case mc::SequenceError::NoteTooLong:
            return QStringLiteral("这一步的说明太长");
    }
    return QStringLiteral("未知错误");
}

QString sequenceStepKindText(mc::SequenceStepKind kind) {
    switch (kind) {
        case mc::SequenceStepKind::SetMode:   return QStringLiteral("设置模式");
        case mc::SequenceStepKind::SetPid:    return QStringLiteral("设置 PID");
        case mc::SequenceStepKind::SetTarget: return QStringLiteral("设置目标值");
        case mc::SequenceStepKind::Start:     return QStringLiteral("启动");
        case mc::SequenceStepKind::Stop:      return QStringLiteral("停止");
        case mc::SequenceStepKind::Wait:      return QStringLiteral("等待");
        case mc::SequenceStepKind::WaitReady: return QStringLiteral("等设备就绪");
        case mc::SequenceStepKind::Home:      return QStringLiteral("回零");
    }
    return QStringLiteral("未知步骤");
}

QString stepOutcomeText(mc::StepOutcome outcome) {
    switch (outcome) {
        case mc::StepOutcome::Acked:    return QStringLiteral("已确认");
        case mc::StepOutcome::Nacked:   return QStringLiteral("被设备拒绝");
        case mc::StepOutcome::TimedOut: return QStringLiteral("超时未确认");
        case mc::StepOutcome::NotSent:  return QStringLiteral("本地没发出去");
        case mc::StepOutcome::Waiting:  return QStringLiteral("等待中");
    }
    return QStringLiteral("未知结局");
}

QString sequencePhaseText(mc::SequencePhase phase) {
    switch (phase) {
        case mc::SequencePhase::Idle:      return QStringLiteral("待命");
        case mc::SequencePhase::Running:   return QStringLiteral("执行中");
        case mc::SequencePhase::Succeeded: return QStringLiteral("全部完成");
        case mc::SequencePhase::Failed:    return QStringLiteral("已停下（某一步没拿到确认）");
        case mc::SequencePhase::Aborted:   return QStringLiteral("已中止");
    }
    return QStringLiteral("未知状态");
}

QString sequenceStepSummary(const mc::SequenceStep& step) {
    switch (step.kind) {
        case mc::SequenceStepKind::SetMode:
            switch (step.mode) {
                case 0U: return QStringLiteral("模式 = 0（停止控制）");
                case 1U: return QStringLiteral("模式 = 1（速度环，deg/s）");
                case 2U: return QStringLiteral("模式 = 2（位置环，deg）");
                default: return QStringLiteral("模式 = %1（越界）").arg(static_cast<int>(step.mode));
            }
        case mc::SequenceStepKind::SetPid:
            return QStringLiteral("Kp = %1，Ki = %2，Kd = %3")
                .arg(static_cast<double>(step.kp), 0, 'f', 4)
                .arg(static_cast<double>(step.ki), 0, 'f', 4)
                .arg(static_cast<double>(step.kd), 0, 'f', 4);
        case mc::SequenceStepKind::SetTarget:
            // 单位取决于当时生效的模式，所以这里**不写单位** ——
            // 写一个猜出来的单位比不写更糟（那会让人以为自己看懂了）。
            return QStringLiteral("目标 = %1（单位取决于上一条设置模式）")
                .arg(static_cast<double>(step.target), 0, 'f', 4);
        case mc::SequenceStepKind::Start:
            return QStringLiteral("从 Ready 进入运行");
        case mc::SequenceStepKind::Stop:
            return QStringLiteral("停止输出（任何时候都接受）");
        case mc::SequenceStepKind::Wait:
            return QStringLiteral("等待 %1 ms").arg(static_cast<qulonglong>(step.waitMs));
        case mc::SequenceStepKind::WaitReady:
            // 说清这是**超时上限**而不是要等满的时间：等着等着就到和固定等一会儿
            // 是两件事，界面上必须分得开。
            return QStringLiteral("等设备回到 Ready，最多 %1 ms")
                .arg(static_cast<qulonglong>(step.waitMs));
        case mc::SequenceStepKind::Home:
            // 两个字段的含义在这一种步里是**换过的**（target = 速度、waitMs = 时限），
            // 所以这里必须把名字写出来，不能照别处那样只报数值。
            return QStringLiteral("回零：以 %1 deg/s 朝零点走，最多 %2 s（到了就把那里定义成 0）")
                .arg(static_cast<double>(step.target), 0, 'f', 1)
                .arg(static_cast<double>(step.waitMs) / 1000.0, 0, 'f', 1);
    }
    return QStringLiteral("—");
}

}  // namespace mchost
