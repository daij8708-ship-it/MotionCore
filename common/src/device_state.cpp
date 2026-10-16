#include "mc/device_state.hpp"

namespace mc {

const char* deviceStateName(DeviceState state) noexcept {
    switch (state) {
        case DeviceState::Disconnected: return "Disconnected";
        case DeviceState::Initializing: return "Initializing";
        case DeviceState::Ready:        return "Ready";
        case DeviceState::Running:      return "Running";
        case DeviceState::Stopping:     return "Stopping";
        case DeviceState::Fault:        return "Fault";
        case DeviceState::Recovery:     return "Recovery";
    }
    return "?";
}

const char* deviceEventName(DeviceEvent event) noexcept {
    switch (event) {
        case DeviceEvent::LinkOpened:        return "LinkOpened";
        case DeviceEvent::LinkLost:          return "LinkLost";
        case DeviceEvent::HandshakeOk:       return "HandshakeOk";
        case DeviceEvent::HandshakeTimeout:  return "HandshakeTimeout";
        case DeviceEvent::StartAccepted:     return "StartAccepted";
        case DeviceEvent::StopReceived:      return "StopReceived";
        case DeviceEvent::StopCompleted:     return "StopCompleted";
        case DeviceEvent::FaultRaised:       return "FaultRaised";
        case DeviceEvent::RecoveryConfirmed: return "RecoveryConfirmed";
        case DeviceEvent::RecoveryOk:        return "RecoveryOk";
    }
    return "?";
}

const char* faultCodeName(FaultCode code) noexcept {
    switch (code) {
        case FaultCode::None:              return "None";
        case FaultCode::CommunicationLoss: return "CommunicationLoss";
        case FaultCode::OverSpeed:         return "OverSpeed";
        case FaultCode::OverLimit:         return "OverLimit";
        case FaultCode::CrcBurst:          return "CrcBurst";
        case FaultCode::Forced:            return "Forced";
    }
    return "?";
}

CommandVerdict commandVerdict(DeviceState state, MessageType type) noexcept {
    switch (type) {
        // 会话起点 / 探活 / 幂等安全动作：处处放行。
        // 堵掉 HELLO 会让"断线重连"根本无法实现；堵掉 STOP 更荒唐。
        case MessageType::Hello:
        case MessageType::Heartbeat:
        case MessageType::Stop:
            return CommandVerdict::Allow;

        // 在线整定是本项目的核心用法，
        // 禁掉 Running 就等于"跑起来之后不让调参"。而改 PID 系数
        // 不改变目标值本身，所以不构成新的运动风险。
        case MessageType::SetPid:
            return (state == DeviceState::Ready || state == DeviceState::Running ||
                    state == DeviceState::Stopping)
                       ? CommandVerdict::Allow
                       : CommandVerdict::RejectIllegalState;

        // Ready 也放行，见 `docs/PROTOCOL.md` §5.3 的不变式 1。
        // 理由不是"方便"，是安全：若只允许 Running 下设定目标，操作员就必须
        // 先 START 再 SET_TARGET —— 而 START 打开输出时用的是**上一次的旧目标**，
        // 电机会先朝那个值动一下。"先设目标、再启动"没有这个瞬动。
        case MessageType::SetTarget:
            return (state == DeviceState::Ready || state == DeviceState::Running)
                       ? CommandVerdict::Allow
                       : CommandVerdict::RejectIllegalState;

        // SET_MODE 会改变 target 的**单位语义**（速度环是 deg/s，位置环是 deg）。
        // 运动中切模式，等于把同一个数值换成了另一种含义 —— 必须停下来再改。
        case MessageType::SetMode:
        case MessageType::Start:
            return (state == DeviceState::Ready) ? CommandVerdict::Allow
                                                 : CommandVerdict::RejectIllegalState;

        default:
            // ACK 类帧、以及将来新增的类型：不该由门禁判"状态非法"。
            // 它们由设备按 UnknownType 拒 —— 否则会把"线接反了/端点是旧的"
            // 这类问题误报成"状态不对"，排查方向直接被带偏。
            return CommandVerdict::Allow;
    }
}

DeviceStateMachine::DeviceStateMachine(DeviceState initial) noexcept : state_(initial) {}

void DeviceStateMachine::enterFault() noexcept {
    state_ = DeviceState::Fault;
    faultLatched_ = true;
}

TransitionResult DeviceStateMachine::apply(DeviceEvent event) noexcept {
    TransitionResult result;
    result.from = state_;

    switch (event) {
        case DeviceEvent::LinkOpened:
            if (state_ == DeviceState::Disconnected) {
                state_ = DeviceState::Initializing;
                result.accepted = true;
            }
            break;

        case DeviceEvent::LinkLost:
            // 从任何状态都合法 —— 链路真没了就是没了。已在 Disconnected 时算幂等，不算错。
            // ⚠️ 刻意**不动** faultLatched_：故障不能被"拔线重连"顺手洗掉。
            state_ = DeviceState::Disconnected;
            result.accepted = true;
            break;

        case DeviceEvent::HandshakeOk:
            if (state_ == DeviceState::Initializing) {
                // 带着未确认的故障重连上来 → 直接回 Fault，不借握手把故障洗掉
                state_ = faultLatched_ ? DeviceState::Fault : DeviceState::Ready;
                result.accepted = true;
            } else if (state_ != DeviceState::Disconnected) {
                // HELLO 是会话级消息：合法，但**不打断运动、也不复位故障**（幂等）。
                result.accepted = true;
            }
            break;

        case DeviceEvent::HandshakeTimeout:
            if (state_ == DeviceState::Initializing) {
                enterFault();
                result.accepted = true;
            }
            break;

        case DeviceEvent::StartAccepted:
            if (state_ == DeviceState::Ready) {
                state_ = DeviceState::Running;
                result.accepted = true;
            }
            break;

        case DeviceEvent::StopReceived:
            // STOP 处处合法（ADR-004：幂等安全动作）。只有 Running 会改变状态。
            result.accepted = true;
            if (state_ == DeviceState::Running) {
                state_ = DeviceState::Stopping;
            }
            break;

        case DeviceEvent::StopCompleted:
            if (state_ == DeviceState::Stopping) {
                state_ = DeviceState::Ready;
                result.accepted = true;
            }
            break;

        case DeviceEvent::FaultRaised:
            // Disconnected 下不产生故障：没有链路就没有可执行的保护动作，
            // 而且此刻任何"故障"都已被 LinkLost 表达过了。
            if (state_ != DeviceState::Disconnected) {
                enterFault();
                result.accepted = true;
            }
            break;

        case DeviceEvent::RecoveryConfirmed:
            if (state_ == DeviceState::Fault) {
                state_ = DeviceState::Recovery;
                result.accepted = true;
            }
            break;

        case DeviceEvent::RecoveryOk:
            if (state_ == DeviceState::Recovery) {
                state_ = DeviceState::Ready;
                faultLatched_ = false;   // 全模块唯一能清除故障锁存的地方
                result.accepted = true;
            }
            break;
    }

    result.to = state_;
    result.stateChanged = (result.from != result.to);
    result.faultLatchedAfter = faultLatched_;
    if (!result.accepted) {
        ++rejectedTransitions_;
    }
    return result;
}

TransitionResult DeviceStateMachine::simulate(DeviceState from, bool faultLatched,
                                              DeviceEvent event) const noexcept {
    DeviceStateMachine probe(from);
    probe.faultLatched_ = faultLatched;
    return probe.apply(event);
}

}  // namespace mc
