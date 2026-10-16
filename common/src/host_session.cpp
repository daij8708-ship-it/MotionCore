#include "mc/host_session.hpp"

#include <algorithm>

namespace mc {

HostSession::HostSession(ITransport& transport) : transport_(transport) {}

HostSession::HostSession(ITransport& transport, AckTracker::Config trackerConfig)
    : transport_(transport), tracker_(trackerConfig) {}

void HostSession::enableLinkMonitor(LinkMonitor::Config config, SeqSource seqSource) {
    if (!seqSource) {
        // 没有序列号来源就发不出心跳。不装作启用成功 —— 保持未启用，且可被观察到。
        return;
    }
    linkMonitor_ = std::make_unique<LinkMonitor>(config);
    seqSource_ = std::move(seqSource);
}

void HostSession::enableHandshake(SeqSource seqSource, HandshakeConfig config) {
    if (!seqSource) {
        // 同上：发不出 HELLO 的"握手层"没有任何意义。
        return;
    }
    seqSource_ = std::move(seqSource);
    handshakeConfig_ = config;
    roundBackoffMs_ = config.roundBackoffInitialMs;
    handshakeEnabled_ = true;
    // 从零开始：调用本函数即表示"现在要建立会话"，而不是"假装已经建立"。
    sessionState_ = SessionState::NotReady;
    handshakeRounds_ = 0U;
    helloSeq_ = 0U;
    nextRoundAttemptMs_ = 0U;
    lastLinkDown_ = false;
}

bool HostSession::sendFrame(MessageType type, std::uint16_t seq,
                            const std::vector<std::uint8_t>& payload) {
    Frame frame;
    frame.version = kProtocolVersion;
    frame.type = type;
    frame.seq = seq;
    frame.payload = payload;

    const EncodeResult encoded = encodeFrame(frame);
    if (!encoded) {
        ++encodeFailures_;
        return false;
    }
    if (!transport_.send(encoded.bytes.data(), encoded.bytes.size())) {
        ++sendFailures_;
        return false;
    }
    ++framesSent_;
    return true;
}

bool HostSession::sendCommand(MessageType type, std::uint16_t seq,
                              const std::vector<std::uint8_t>& payload, std::uint64_t nowMs) {
    const bool awaitAck = requiresAck(type);

    if (awaitAck && !tracker_.canRegister(type, seq)) {
        // 登记不上就先别发 —— 发出去也没人能认领它的应答。
        return false;
    }

    if (!sendFrame(type, seq, payload)) {
        return false;
    }

    if (awaitAck && !tracker_.registerPending(type, seq, nowMs)) {
        // 上面刚查过 canRegister，走到这里说明本类内部状态不一致。
        ++encodeFailures_;
        return false;
    }

    if (awaitAck && isRetryable(type)) {
        // 只有幂等命令才留底。留底的意义是：超时被判 Retry 时能**原样**重发，
        // 而"原样"必须包含同一 seq —— 设备侧的去重窗口靠它把"已执行过的那次"
        // 变成重放应答而不是重复执行。
        InFlightRequest item;
        item.type = type;
        item.seq = seq;
        item.payload = payload;
        inFlight_.push_back(std::move(item));
    }
    return true;
}

bool HostSession::sendHello(std::uint16_t seq, std::uint64_t nowMs) {
    return sendCommand(MessageType::Hello, seq, {}, nowMs);
}

bool HostSession::sendHeartbeat(std::uint16_t seq, std::uint64_t nowMs) {
    return sendCommand(MessageType::Heartbeat, seq, {}, nowMs);
}

bool HostSession::sendSetMode(std::uint8_t mode, std::uint16_t seq, std::uint64_t nowMs) {
    return sendCommand(MessageType::SetMode, seq, encodeSetMode(SetMode{mode}), nowMs);
}

bool HostSession::sendStart(std::uint16_t seq, std::uint64_t nowMs) {
    return sendCommand(MessageType::Start, seq, {}, nowMs);
}

bool HostSession::sendSetTarget(float target, std::uint16_t seq, std::uint64_t nowMs) {
    return sendCommand(MessageType::SetTarget, seq, encodeSetTarget(SetTarget{target}), nowMs);
}

bool HostSession::sendSetPid(const SetPid& pid, std::uint16_t seq, std::uint64_t nowMs) {
    return sendCommand(MessageType::SetPid, seq, encodeSetPid(pid), nowMs);
}

bool HostSession::sendClearFault(std::uint8_t faultCode, std::uint16_t seq, std::uint64_t nowMs) {
    ClearFault payload;
    payload.faultCode = faultCode;
    return sendCommand(MessageType::ClearFault, seq, encodeClearFault(payload), nowMs);
}

bool HostSession::sendStop(std::uint16_t seq) {
    // 单向：不登记、不等待、失败也不重试（ADR-004）。
    return sendFrame(MessageType::Stop, seq, {});
}

HostSession::PumpResult HostSession::pump(std::uint64_t nowMs, std::size_t maxBytesPerPump) {
    PumpResult result;

    std::uint8_t buffer[256];
    while (result.bytesRead < maxBytesPerPump) {
        const std::size_t want = std::min(sizeof(buffer), maxBytesPerPump - result.bytesRead);
        const std::size_t got = transport_.receive(buffer, want);
        if (got == 0U) {
            break;
        }
        result.bytesRead += got;

        const std::vector<Frame> frames = decoder_.feed(buffer, got);
        result.framesDecoded += frames.size();
        for (const Frame& frame : frames) {
            dispatch(frame, result, nowMs);
        }
    }

    result.timeouts = tracker_.pollTimeouts(nowMs);
    for (const TimeoutEvent& event : result.timeouts) {
        switch (event.outcome) {
            case TxOutcome::Dropped:
                // 心跳漏拍：不是故障，也不重发 —— 下一拍自然会有新的心跳。
                ++result.droppedProbes;
                break;

            case TxOutcome::Retry:
                // 上面判了"可以重发"，这里必须**真的**重发。
                // 否则"可重传"就只是一个没人执行的决定：请求会一直挂在 tracker 里，
                // 而且因为 canRegister 已为 false，调用方连自己重发都做不到。
                resendRetryable(event);
                break;

            case TxOutcome::Fault:
                forgetInFlight(event.seq);
                if (handshakeEnabled_ && event.requestType == MessageType::Hello &&
                    sessionState_ == SessionState::Handshaking && event.seq == helloSeq_) {
                    // 一轮握手用尽重试：退避后重开一轮。**不设总轮数上限** ——
                    // 要不要放弃是上位机（人）的决定，本层只如实报 handshakeRounds()。
                    sessionState_ = SessionState::NotReady;
                    nextRoundAttemptMs_ = nowMs + roundBackoffMs_;
                    roundBackoffMs_ =
                        std::min(roundBackoffMs_ * 2U, handshakeConfig_.roundBackoffMaxMs);
                    result.handshakeRoundFailed = true;
                }
                break;
        }
    }

    if (linkMonitor_ != nullptr) {
        const LinkMonitor::Tick beat = linkMonitor_->tick(nowMs);
        if (beat.sendHeartbeat) {
            const std::uint16_t seq = seqSource_();
            if (sendHeartbeat(seq, nowMs)) {
                // 只有真的发出去了才登记发送时刻 —— 否则算出的 RTT 会是假的 0。
                linkMonitor_->onHeartbeatSent(seq, nowMs);
                result.heartbeatSent = true;
            }
            // 发失败就当这一拍丢了，由"连续丢 N 拍"累计，不在这里特判。
        }
        if (beat.recommendationChanged) {
            // 只影响之后登记的请求；在途请求保留各自原有的截止时刻。
            tracker_.setTimeoutMs(beat.recommendedAckTimeoutMs);
        }
        result.linkDown = beat.linkDown;
        result.ackTimeoutMs = beat.recommendedAckTimeoutMs;

        // 只在跳变沿上作废会话：链路一直断着时不该反复作废、反复重置退避。
        if (beat.linkDown && !lastLinkDown_ && handshakeEnabled_ &&
            sessionState_ == SessionState::Ready) {
            sessionState_ = SessionState::NotReady;
            nextRoundAttemptMs_ = nowMs;   // 立刻重开一轮 —— 断线后越早重新探到越好
            roundBackoffMs_ = handshakeConfig_.roundBackoffInitialMs;
            result.sessionLost = true;
            // ⚠ 刻意**不**清空 tracker 的在途请求：它们必须自己走完超时。
            // 静默丢弃会让一条运动指令"凭空消失"，既不报 Fault 也不报超时；
            // 而 ADR-003 要求它超时就是一个**安全事件**，必须被上报。
        }
        lastLinkDown_ = beat.linkDown;
    } else {
        result.ackTimeoutMs = tracker_.timeoutMs();
    }

    tickHandshake(nowMs);

    result.transportOpen = transport_.isOpen();
    return result;
}

void HostSession::resendRetryable(const TimeoutEvent& event) {
    const auto it = std::find_if(inFlight_.begin(), inFlight_.end(),
                                 [&event](const InFlightRequest& item) {
                                     return item.seq == event.seq;
                                 });
    if (it == inFlight_.end()) {
        // 不可重传的类型（尤其运动指令）根本没有留底 —— 走到这里就是"不重发"本身。
        return;
    }
    // 同一 seq、同一载荷原样重发。对端去重窗口会把"已执行过的那次"变成重放应答。
    (void)sendFrame(it->type, it->seq, it->payload);
}

void HostSession::forgetInFlight(std::uint16_t seq) {
    const auto it = std::find_if(inFlight_.begin(), inFlight_.end(),
                                 [seq](const InFlightRequest& item) { return item.seq == seq; });
    if (it != inFlight_.end()) {
        inFlight_.erase(it);
    }
}

void HostSession::tickHandshake(std::uint64_t nowMs) {
    if (!handshakeEnabled_ || sessionState_ != SessionState::NotReady) {
        return;
    }
    if (nowMs < nextRoundAttemptMs_) {
        return;
    }

    const std::uint16_t seq = seqSource_();
    if (!sendHello(seq, nowMs)) {
        // 没发出去（编码失败 / 传输拒收）—— 不装作在握手。下一拍再试，
        // 失败本身已经计入 encodeFailures_ / sendFailures_，不会被吞掉。
        return;
    }
    helloSeq_ = seq;
    sessionState_ = SessionState::Handshaking;
    ++handshakeRounds_;
}

void HostSession::dispatch(const Frame& frame, PumpResult& result, std::uint64_t nowMs) {
    if (isAck(frame.type)) {
        const bool matched = tracker_.onAck(frame.type, frame.seq);
        if (matched) {
            ++result.acksMatched;
            forgetInFlight(frame.seq);

            // 会话就绪**只**由"确实配对上的 HELLO_ACK"触发。
            // 一个陈旧的/没人认领的 HelloAck 绝不能把会话判成就绪 ——
            // 那等于让线上随便一帧把上发指令的闸门打开。
            if (handshakeEnabled_ && frame.type == MessageType::HelloAck &&
                sessionState_ == SessionState::Handshaking && frame.seq == helloSeq_) {
                sessionState_ = SessionState::Ready;
                roundBackoffMs_ = handshakeConfig_.roundBackoffInitialMs;   // 会话建立，退避归零
                result.handshakeCompleted = true;
            }
        } else {
            // 陈旧、重复、或应答类型与在等请求对不上 —— 都不是成功。
            ++result.unexpectedFrames;
        }

        if (linkMonitor_ != nullptr && frame.type == MessageType::HeartbeatAck) {
            // 心跳应答**无条件**交给监控器，不依赖 tracker 是否还认领它。
            // 否则会自举死锁：初始窗口若小于真实 RTT，心跳先被判超时出队 →
            // 监控器永远拿不到 RTT 样本 → 推荐窗口永不收敛 → 窗口一直太小。
            // 迟到的心跳应答同样携带**真实 RTT**，而它正是把窗口撑开所必需的。
            linkMonitor_->onHeartbeatAck(frame.seq, nowMs);
        }
        return;
    }

    switch (frame.type) {
        case MessageType::Telemetry: {
            Telemetry telemetry;
            const PayloadError error =
                decodeTelemetry(frame.payload.data(), frame.payload.size(), telemetry);
            if (error != PayloadError::None || !isTelemetryValid(telemetry)) {
                // 关键：非法遥测**绝不进入数据通路**。
                // NaN 的 4 个字节是完全合法的 float32，长度校验拦不住它 ——
                // 一旦让它混进曲线，整条显示就毁了。
                ++result.telemetryRejected;
                ++telemetryRejected_;
                return;
            }
            lastTelemetry_ = telemetry;
            hasTelemetry_ = true;
            ++telemetryAccepted_;
            ++result.telemetryAccepted;
            return;
        }

        case MessageType::Nack: {
            Nack nack;
            if (decodeNack(frame.payload.data(), frame.payload.size(), nack) != PayloadError::None) {
                ++result.unexpectedFrames;
                return;
            }
            receivedNacks_.push_back(nack);
            ++result.naksReceived;

            // 对端明确拒绝了 —— 必须停止等待它的应答，
            // 否则会白等到超时，甚至在超时后把它判成"运动指令失控"。
            if (tracker_.cancelPending(nack.rejectedSeq)) {
                forgetInFlight(nack.rejectedSeq);
            } else {
                ++result.unexpectedFrames;   // 拒了一个我们没在等的请求
            }
            return;
        }

        default:
            ++result.unexpectedFrames;
            return;
    }
}

}  // namespace mc
