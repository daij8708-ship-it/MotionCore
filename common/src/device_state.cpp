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
        case DeviceState::Home:         return "Home";
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
        case DeviceEvent::HomeAccepted:      return "HomeAccepted";
        case DeviceEvent::HomeCompleted:     return "HomeCompleted";
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
        case FaultCode::HomeFailed:        return "HomeFailed";
    }
    return "?";
}

CommandVerdict commandVerdict(DeviceState state, MessageType type) noexcept {
    switch (type) {
        // 会话起点 / 探活 / 幂等安全动作：处处放行。
        // 堵掉 HELLO 会让"断线重连"根本无法实现；堵掉 STOP 更荒唐。
        //
        // READ_PARAMS 属于同一族：**它什么都不改变**，因此不存在"这个状态下
        // 不允许读"这种情况 —— 把它也塞进门禁表只会多一格没人能说清理由的格子。
        case MessageType::Hello:
        case MessageType::Heartbeat:
        case MessageType::Stop:
        case MessageType::ReadParams:
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

        // 回零：**只在 Ready 放行**。它与 START 同族（会让电机动起来），
        // 但比 START 更严：回零的前提是"现在没有别的运动目标"，
        // 所以在 Running / Stopping 下必须先停下来。
        // Fault / Recovery 下更谈不上：零点还没建立，此刻该做的是先把故障处理掉。
        case MessageType::Home:
            return (state == DeviceState::Ready) ? CommandVerdict::Allow
                                                 : CommandVerdict::RejectIllegalState;

        // 软限位：与 SET_PID 同族 —— 改的是**保护参数**，不改变目标值本身，
        // 因此运动中也可以改。允许在 Running 下改是有意的：想把行程收紧一点
        // （或放宽）不该逼着人先停机，而真的收紧之后设备自己会按新限位执法。
        //
        // 唯一会立刻出事的那种（新限位把**此刻的位置**关在门外，等于设完就报超限故障）
        // 不由这张表管：它需要知道当前位置，而门禁表只看状态。那一层判在设备侧
        // （见 DeviceSimulator 的 SET_LIMITS 分支），以 BadParameter 明确拒掉。
        case MessageType::SetLimits:
            return (state == DeviceState::Ready || state == DeviceState::Running ||
                    state == DeviceState::Stopping)
                       ? CommandVerdict::Allow
                       : CommandVerdict::RejectIllegalState;

        // 故障确认：**只在 Fault 状态放行**。
        //
        // 放行的条件不是"故障锁存着"，而是"此刻正停在故障态" —— 这两者不等价：
        // 拔线重连后锁存位仍在，但状态是 Disconnected/Initializing，那时该做的是
        // 把链路接回来（重握手会直接回到 Fault），而不是在一条发不出命令的链路上
        // 点确认。Recovery 里也不放行：那说明人工确认已经做过了，正等自检结果，
        // 此时再确认一次没有意义。
        case MessageType::ClearFault:
            return (state == DeviceState::Fault) ? CommandVerdict::Allow
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
            // STOP 处处合法（ADR-004：幂等安全动作）。只有"电机动着"的状态会改变状态。
            // ⚠ 回零中也要能停：那是**电机正在转**的时刻，而 STOP 在这里拦不住
            //   就等于把一段运动变成了不可中断的。停下来之后零点**没有建立** ——
            //   这件事由上位机按"从 Home 直接进了 Stopping"如实报出来。
            result.accepted = true;
            if (state_ == DeviceState::Running || state_ == DeviceState::Home) {
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

        case DeviceEvent::HomeAccepted:
            if (state_ == DeviceState::Ready) {
                state_ = DeviceState::Home;
                result.accepted = true;
            }
            break;

        case DeviceEvent::HomeCompleted:
            // 只有"正在回零"才能报到位。Ready 里再收到一次是**被拒**的
            // （而不是幂等接受）：它意味着有人在没有回零的情况下宣称零点建立了。
            if (state_ == DeviceState::Home) {
                state_ = DeviceState::Ready;
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
