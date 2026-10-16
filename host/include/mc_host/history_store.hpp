#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <QSqlDatabase>
#include <QString>

#include "mc_host/telemetry_ring.hpp"

namespace mchost {

// 历史数据的落库（SQLite）。
//
// 三件事决定它长这样：
//   ① **WAL**：写入期间读查询不会被写锁挡住。历史页一边翻旧数据、后台一边落新数据
//      是常态，用默认的 rollback journal 就会出现"查询卡住整条采集"。
//   ② **批量事务**：一帧一行、一行一个事务，等于每 10 ms 让 SQLite 刷一次盘。
//      攒够一批（或到刷写周期）在一个事务里写 —— 这是"每帧一次 fsync"与
//      "一段时间一次 fsync"的区别，不是风格问题。
//   ③ **不静默失败**：数据库打不开时，采集照常进行（界面上的实时数据不依赖它），
//      但历史页必须显示"没有在落库"和原因。默默丢数据是最坏的一种：
//      事后有人来查历史，看到的是"这段时间没有数据"，而真实情况是没人写过。
//
// 它**不判协议**、也不改任何采集流程：只负责"把已经发生的事实记下来"。
class HistoryStore {
public:
    // 攒够这么多样本就落一次盘。
    // 100 Hz 下这大约 2.5 s 一次事务；再大只是把"未落盘的窗口"拉长（掉电丢得更多）。
    static constexpr std::size_t kBatchSize = 256U;
    // 即使没攒够，超过这么久也落一次（低速采样时不能让数据一直悬在内存里）。
    static constexpr std::uint64_t kFlushPeriodMs = 500U;

    HistoryStore();
    ~HistoryStore();

    HistoryStore(const HistoryStore&) = delete;
    HistoryStore& operator=(const HistoryStore&) = delete;

    // 打开（或新建）数据库文件并建表。重复调用是幂等的。
    bool open(const QString& dbPath);
    void close();
    bool isOpen() const noexcept { return open_; }
    const QString& path() const noexcept { return path_; }
    // 打不开时给界面用的一句话（含 SQLite 的原文）。
    const QString& error() const noexcept { return error_; }

    // --- 写入 -------------------------------------------------------------------
    // 开一次会话（= 一次连接）。startedWallMs 是**墙上时钟**（给人看的），
    // 样本里的 t_ms 则是相对会话起点的毫秒数（与界面上的曲线同一口径）。
    bool beginSession(const QString& deviceKind, bool isSimulated, qint64 startedWallMs);
    void endSession(qint64 endedWallMs);
    bool hasOpenSession() const noexcept { return sessionId_ > 0; }
    qint64 sessionId() const noexcept { return sessionId_; }

    void addSample(const TelemetrySample& sample);
    void addEvent(std::uint64_t tMs, qint64 wallMs, const QString& text);
    // 命令记录是**可以变的**（等待应答 → 已确认/被拒/超时），所以按
    // (session_id, seq) 做 UPSERT：同一条命令的状态更新落在同一行上，
    // 不会出现"一条命令两条记录、一条还在等应答"。
    void upsertCommand(std::uint64_t sentMs, std::uint16_t seq, const QString& name,
                       int status, int nackReason);

    // 把攒着的样本落盘（不足一批也会写）。断开前应当调用一次。
    void flush();
    // 按时间决定要不要落盘：由应用服务层的刷新节拍调用。
    void tick(qint64 wallMs);

    // --- 观测 -------------------------------------------------------------------
    std::uint64_t samplesWritten() const noexcept { return samplesWritten_; }
    std::uint64_t pendingSamples() const noexcept { return pending_.size(); }
    // 已经开过多少个事务。**批量落盘的证据**：样本数 / 事务数应当接近 kBatchSize，
    // 而不是 1（每样本一个事务 = 没有批量）。
    std::uint64_t transactions() const noexcept { return transactions_; }

    struct SessionRow {
        qint64 id = 0;
        qint64 startedWallMs = 0;
        qint64 endedWallMs = -1;   // -1 = 没有正常结束（进程被杀也会留下这个值，这是有意的）
        QString deviceKind;
        bool isSimulated = true;
        qint64 sampleCount = 0;
    };

    struct SampleRow {
        qint64 tMs = 0;
        double velocityDegPerSec = 0.0;
        double positionDeg = 0.0;
        double target = 0.0;
        int state = 0;
        int faultCode = 0;
        int pwmOutput = 0;
    };

    struct EventRow {
        qint64 tMs = 0;
        qint64 wallMs = 0;
        QString text;
    };

    struct CommandRow {
        qint64 sentMs = 0;
        int seq = 0;
        QString name;
        int status = 0;
        int nackReason = 0;
    };

    // --- 查询（历史页用）--------------------------------------------------------
    // 这些方法都**先落盘再查**：攒着没写的样本（最多半个刷写周期）必须算进结果里，
    // 否则"查询显示没有数据、其实只是还没写"会让排查方向彻底跑偏。
    // 正因为要落盘，它们不是 const。
    std::vector<SessionRow> sessions(QString& error);
    std::vector<SampleRow> samples(qint64 sessionId, qint64 fromTMs, qint64 toTMs, int limit,
                                   QString& error);
    std::vector<EventRow> events(qint64 sessionId, QString& error);
    std::vector<CommandRow> commands(qint64 sessionId, QString& error);
    qint64 countSamples(qint64 sessionId, qint64 fromTMs, qint64 toTMs, QString& error);
    // 库里的总样本数（不挑会话），给界面显示"这个文件里到底有多少数据"。
    qint64 totalSamples(QString& error);

    // 导出为 CSV（UTF-8 带 BOM + CRLF）。带 BOM 是为了 Excel 直接双击打开时
    // 中文表头不乱码 —— 这不是洁癖，是"这份文件是给人看的"这条约束的一部分。
    bool exportSamplesCsv(const QString& filePath, qint64 sessionId, qint64 fromTMs, qint64 toTMs,
                          QString& error);
    std::uint64_t exportedRows() const noexcept { return exportedRows_; }

private:
    bool exec(const QString& sql, QString& error) const;
    bool flushLocked(QString& error);
    bool ensureSchema();

    QSqlDatabase db_;
    QString path_;
    QString error_;
    bool open_ = false;

    qint64 sessionId_ = 0;
    std::vector<TelemetrySample> pending_;
    std::uint64_t samplesWritten_ = 0;
    std::uint64_t transactions_ = 0;
    qint64 lastFlushWallMs_ = 0;
    std::uint64_t exportedRows_ = 0;
};

}  // namespace mchost
