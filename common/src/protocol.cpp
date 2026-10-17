#include "mc/protocol.hpp"

namespace mc {
namespace {

struct TypeInfo {
    MessageType type;
    const char* name;
};

// 顺序无关紧要；查到即返回。
constexpr TypeInfo kTypeTable[] = {
    {MessageType::Hello, "HELLO"},
    {MessageType::Heartbeat, "HEARTBEAT"},
    {MessageType::SetMode, "SET_MODE"},
    {MessageType::Start, "START"},
    {MessageType::Stop, "STOP"},
    {MessageType::SetTarget, "SET_TARGET"},
    {MessageType::SetPid, "SET_PID"},
    {MessageType::ClearFault, "CLEAR_FAULT"},
    {MessageType::ReadParams, "READ_PARAMS"},
    {MessageType::Home, "HOME"},
    {MessageType::SetLimits, "SET_LIMITS"},
    {MessageType::Telemetry, "TELEMETRY"},
    {MessageType::FaultEvent, "FAULT_EVENT"},
    {MessageType::Nack, "NAK"},
    {MessageType::HelloAck, "HELLO_ACK"},
    {MessageType::HeartbeatAck, "HEARTBEAT_ACK"},
    {MessageType::SetModeAck, "SET_MODE_ACK"},
    {MessageType::StartAck, "START_ACK"},
    {MessageType::SetTargetAck, "SET_TARGET_ACK"},
    {MessageType::SetPidAck, "SET_PID_ACK"},
    {MessageType::ClearFaultAck, "CLEAR_FAULT_ACK"},
    {MessageType::ReadParamsAck, "READ_PARAMS_ACK"},
    {MessageType::HomeAck, "HOME_ACK"},
    {MessageType::SetLimitsAck, "SET_LIMITS_ACK"},
};

const TypeInfo* lookup(MessageType type) noexcept {
    for (const TypeInfo& info : kTypeTable) {
        if (info.type == type) {
            return &info;
        }
    }
    return nullptr;
}

}  // namespace

const char* toString(MessageType type) noexcept {
    const TypeInfo* info = lookup(type);
    return (info != nullptr) ? info->name : "UNKNOWN";
}

bool isKnown(MessageType type) noexcept {
    return lookup(type) != nullptr;
}

bool isAck(MessageType type) noexcept {
    if (!isKnown(type)) {
        return false;
    }
    return (static_cast<std::uint8_t>(type) & kAckFlag) != 0U;
}

bool requiresAck(MessageType type) noexcept {
    switch (type) {
        case MessageType::Hello:
        case MessageType::Heartbeat:
        case MessageType::SetMode:
        case MessageType::Start:
        case MessageType::SetTarget:
        case MessageType::SetPid:
        case MessageType::ClearFault:
        case MessageType::ReadParams:
        // HOME 必须应答：它是**一条运动命令**（电机真的会转）。
        // 没收到"已接受"就不知道它有没有开始，而"以为在回零、其实没动"
        // 比明确失败更难查。
        case MessageType::Home:
        // SET_LIMITS 必须应答：它是设备侧的一道安全约束，
        // "改没改成功"不能靠猜。
        case MessageType::SetLimits:
            return true;
        default:
            // Stop（单向停止，ADR-004）、Telemetry（周期上报，ADR-005）、全部 ACK
            // 都不需要应答。
            //
            // FaultEvent 也不需要：它是**通知**而不是请求。故障期间设备会周期性重发，
            // 丢一帧下一拍就有新的 —— 这比引入一个 FaultEventAck 再维护一条等待队列
            // 更简单，也不会在"设备已经出事"的时候还要求它响应。
            return false;
    }
}

bool isRetryable(MessageType type) noexcept {
    switch (type) {
        case MessageType::Hello:
        case MessageType::SetMode:
        case MessageType::SetPid:
        case MessageType::ClearFault:
        case MessageType::ReadParams:
        case MessageType::SetLimits:
            // 幂等命令：重发不改变最终状态。
            //
            // CLEAR_FAULT 属于这一族：它不产生任何运动，重复清除一个已清除的故障
            // 也是无操作。而且重发**正好被设备侧的去重窗口接住**（同 seq 重放应答，
            // 不重复执行副作用），所以"应答丢了再发一次"是安全的。
            return true;
        case MessageType::Start:
        case MessageType::SetTarget:
        case MessageType::Home:
            // 运动指令（回零也在内：电机真的会转）：超时后进入 Fault
            // 等待人工确认，绝不自动重发（ADR-003）。
            return false;
        default:
            // 含 Heartbeat：虽期待应答，但心跳本身是周期性的，
            // 丢一帧等下一个周期即可，重传没有意义。
            return false;
    }
}

bool isPeriodicProbe(MessageType type) noexcept {
    // 只有心跳。它的应答确实需要（用来测 RTT、证明双向连通），
    // 但每一次超时都只是"漏了一拍"，由链路监控累计判断，不单独构成故障。
    return type == MessageType::Heartbeat;
}

bool tryAckOf(MessageType request, MessageType& out) noexcept {
    switch (request) {
        case MessageType::Hello:
            out = MessageType::HelloAck;
            return true;
        case MessageType::Heartbeat:
            out = MessageType::HeartbeatAck;
            return true;
        case MessageType::SetMode:
            out = MessageType::SetModeAck;
            return true;
        case MessageType::Start:
            out = MessageType::StartAck;
            return true;
        case MessageType::SetTarget:
            out = MessageType::SetTargetAck;
            return true;
        case MessageType::SetPid:
            out = MessageType::SetPidAck;
            return true;
        case MessageType::ClearFault:
            out = MessageType::ClearFaultAck;
            return true;
        case MessageType::ReadParams:
            out = MessageType::ReadParamsAck;
            return true;
        case MessageType::Home:
            out = MessageType::HomeAck;
            return true;
        case MessageType::SetLimits:
            out = MessageType::SetLimitsAck;
            return true;
        default:
            // Stop / Telemetry / FaultEvent 没有"请求-应答"配对。
            return false;
    }
}

}  // namespace mc
