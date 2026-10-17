#include "mc_host/history_store.hpp"

#include <QDateTime>
#include <QFile>
#include <QSqlError>
#include <QSqlQuery>
#include <QTextStream>
#include <QVariant>

#include "mc/device_state.hpp"

namespace mchost {
namespace {

// 连接名必须唯一：Qt 的 QSqlDatabase 是按名字注册的全局表，
// 两个 HistoryStore（例如测试里同时开了两个）用同一个名字会互相抢连接。
QString nextConnectionName() {
    static int counter = 0;
    return QStringLiteral("mc_host_history_%1").arg(++counter);
}

}  // namespace

HistoryStore::HistoryStore() : db_(QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                             nextConnectionName())),
                               lastFlushWallMs_(QDateTime::currentMSecsSinceEpoch()) {}

HistoryStore::~HistoryStore() {
    close();
}

bool HistoryStore::open(const QString& dbPath) {
    if (open_ && path_ == dbPath) {
        return true;
    }
    close();

    path_ = dbPath;
    error_.clear();
    db_.setDatabaseName(dbPath);

    if (!db_.open()) {
        error_ = QStringLiteral("打不开数据库文件 %1：%2").arg(dbPath, db_.lastError().text());
        return false;
    }

    if (!ensureSchema()) {
        db_.close();
        return false;
    }

    open_ = true;
    lastFlushWallMs_ = QDateTime::currentMSecsSinceEpoch();
    return true;
}

void HistoryStore::close() {
    if (!open_) {
        return;
    }
    flush();
    db_.close();
    open_ = false;
    sessionId_ = 0;
}

bool HistoryStore::exec(const QString& sql, QString& error) const {
    QSqlQuery query(db_);
    if (!query.exec(sql)) {
        error = QStringLiteral("%1（%2）").arg(query.lastError().text(), sql.left(80));
        return false;
    }
    return true;
}

bool HistoryStore::ensureSchema() {
    // WAL：写的时候读不会被挡。历史页翻旧数据与后台落新数据同时发生是常态。
    QString error;
    if (!exec(QStringLiteral("PRAGMA journal_mode=WAL"), error)) {
        this->error_ = error;
        return false;
    }
    // NORMAL：WAL 下这个档位在"掉电最多丢最后一个事务"与写入开销之间取平衡，
    // 而我们的批量事务本来就意味着"最多丢最后 2.5 s 的样本"。
    if (!exec(QStringLiteral("PRAGMA synchronous=NORMAL"), error)) {
        this->error_ = error;
        return false;
    }

    const QStringList schema = {
        QStringLiteral("CREATE TABLE IF NOT EXISTS session ("
                       "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                       "started_wall_ms INTEGER NOT NULL,"
                       "ended_wall_ms INTEGER,"
                       "device_kind TEXT NOT NULL,"
                       "is_simulated INTEGER NOT NULL,"
                       "sample_count INTEGER NOT NULL DEFAULT 0)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS sample ("
                       "session_id INTEGER NOT NULL,"
                       "t_ms INTEGER NOT NULL,"
                       "velocity REAL NOT NULL,"
                       "position REAL NOT NULL,"
                       "target REAL NOT NULL,"
                       "state INTEGER NOT NULL,"
                       "fault_code INTEGER NOT NULL,"
                       "pwm INTEGER NOT NULL)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_sample_session_t "
                       "ON sample(session_id, t_ms)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS event ("
                       "session_id INTEGER NOT NULL,"
                       "t_ms INTEGER NOT NULL,"
                       "wall_ms INTEGER NOT NULL,"
                       "text TEXT NOT NULL)"),
        // 报警单独一张表，而不是把代码/等级塞进上面那张 event 的文本里：
        // 一旦只留下"人看得懂的一句话"，"查一下这一段里有多少条 Error 以上没确认"
        // 就再也查不出来了 —— 而那正是报警分级存在的全部意义。
        // 唯一键里的 first_wall_ms 见 upsertAlarm 的说明（清屏之后同码再报是另一起）。
        QStringLiteral("CREATE TABLE IF NOT EXISTS alarm ("
                       "session_id INTEGER NOT NULL,"
                       "code INTEGER NOT NULL,"
                       "level INTEGER NOT NULL,"
                       "first_t_ms INTEGER NOT NULL,"
                       "last_t_ms INTEGER NOT NULL,"
                       "first_wall_ms INTEGER NOT NULL,"
                       "last_wall_ms INTEGER NOT NULL,"
                       "occurrences INTEGER NOT NULL,"
                       "acknowledged INTEGER NOT NULL,"
                       "ack_wall_ms INTEGER NOT NULL,"
                       "active INTEGER NOT NULL,"
                       "detail TEXT NOT NULL,"
                       "UNIQUE(session_id, code, first_wall_ms))"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS command ("
                       "session_id INTEGER NOT NULL,"
                       "seq INTEGER NOT NULL,"
                       "name TEXT NOT NULL,"
                       "sent_ms INTEGER NOT NULL,"
                       "status INTEGER NOT NULL,"
                       "nack_reason INTEGER NOT NULL,"
                       "UNIQUE(session_id, seq))"),
        // 审计与上面几张表的姿态不同：它是**追加**的流水，不是会变的当前状态。
        // 所以既不做 UPSERT，也不按 (session_id, seq) 去唯一化 —— 那种唯一键会
        // 让"同一个动作做过两次"里的第二次把第一次替换掉，而审计要记的恰恰是"几次"。
        // 会话号只是其中一列（0 = 当时没连接），不是键。
        QStringLiteral("CREATE TABLE IF NOT EXISTS audit ("
                       "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                       "wall_ms INTEGER NOT NULL,"
                       "t_ms INTEGER NOT NULL,"
                       "session_id INTEGER NOT NULL,"
                       "role INTEGER NOT NULL,"
                       "action INTEGER NOT NULL,"
                       "subject TEXT NOT NULL,"
                       "dispatched INTEGER NOT NULL,"
                       "reason TEXT NOT NULL)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_audit_wall ON audit(wall_ms)"),
    };
    for (const QString& sql : schema) {
        if (!exec(sql, error)) {
            this->error_ = error;
            return false;
        }
    }
    return true;
}

bool HistoryStore::beginSession(const QString& deviceKind, bool isSimulated,
                                qint64 startedWallMs) {
    if (!open_) {
        return false;
    }
    flush();

    QSqlQuery query(db_);
    query.prepare(QStringLiteral(
        "INSERT INTO session (started_wall_ms, ended_wall_ms, device_kind, is_simulated, "
        "sample_count) VALUES (?, NULL, ?, ?, 0)"));
    query.addBindValue(startedWallMs);
    query.addBindValue(deviceKind);
    query.addBindValue(isSimulated ? 1 : 0);
    if (!query.exec()) {
        error_ = QStringLiteral("开会话失败：%1").arg(query.lastError().text());
        return false;
    }

    sessionId_ = query.lastInsertId().toLongLong();
    return true;
}

void HistoryStore::endSession(qint64 endedWallMs) {
    if (!open_ || sessionId_ <= 0) {
        return;
    }
    flush();

    QSqlQuery query(db_);
    query.prepare(QStringLiteral("UPDATE session SET ended_wall_ms = ? WHERE id = ?"));
    query.addBindValue(endedWallMs);
    query.addBindValue(sessionId_);
    (void)query.exec();
    sessionId_ = 0;
}

void HistoryStore::addSample(const TelemetrySample& sample) {
    if (!open_ || sessionId_ <= 0) {
        return;
    }
    pending_.push_back(sample);
    if (pending_.size() >= kBatchSize) {
        flush();
    }
}

void HistoryStore::addEvent(std::uint64_t tMs, qint64 wallMs, const QString& text) {
    if (!open_ || sessionId_ <= 0) {
        return;
    }
    // 事件没有"批量"的说法（一次报警一条），但**必须先落盘**：报警是事后追查的起点，
    // 让它跟样本一起悬在内存里，掉电时恰恰丢掉的就是最要紧的那条。
    flush();

    QSqlQuery query(db_);
    query.prepare(QStringLiteral(
        "INSERT INTO event (session_id, t_ms, wall_ms, text) VALUES (?, ?, ?, ?)"));
    query.addBindValue(sessionId_);
    query.addBindValue(static_cast<qint64>(tMs));
    query.addBindValue(wallMs);
    query.addBindValue(text);
    (void)query.exec();
}

void HistoryStore::upsertAlarm(const mc::AlarmEntry& entry) {
    if (!open_ || sessionId_ <= 0) {
        return;
    }
    // 与 addEvent 同理：报警必须先落盘。掉电时最要紧的那条恰恰是刚发生的那条。
    flush();

    QSqlQuery query(db_);
    query.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO alarm (session_id, code, level, first_t_ms, last_t_ms, "
        "first_wall_ms, last_wall_ms, occurrences, acknowledged, ack_wall_ms, active, detail) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    query.addBindValue(sessionId_);
    query.addBindValue(static_cast<int>(entry.code));
    query.addBindValue(static_cast<int>(entry.level));
    query.addBindValue(static_cast<qint64>(entry.firstTMs));
    query.addBindValue(static_cast<qint64>(entry.lastTMs));
    query.addBindValue(static_cast<qint64>(entry.firstWallMs));
    query.addBindValue(static_cast<qint64>(entry.wallMs));
    query.addBindValue(static_cast<int>(entry.occurrences));
    query.addBindValue(entry.acknowledged ? 1 : 0);
    query.addBindValue(static_cast<qint64>(entry.ackWallMs));
    query.addBindValue(entry.active ? 1 : 0);
    query.addBindValue(QString::fromStdString(entry.detail));
    (void)query.exec();
}

void HistoryStore::upsertCommand(std::uint64_t sentMs, std::uint16_t seq, const QString& name,
                                 int status, int nackReason) {
    if (!open_ || sessionId_ <= 0) {
        return;
    }
    // 这里**不**强制落盘（与 addEvent 不同）：命令记录的变化是高频的
    // （一条命令会从"等待应答"变成"已确认"），每次都开一个事务就把批量落盘废掉了。
    // 代价是最多丢掉最后半个刷写周期内的命令记录 —— 有界、且写进文档。

    QSqlQuery query(db_);
    query.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO command (session_id, seq, name, sent_ms, status, nack_reason) "
        "VALUES (?, ?, ?, ?, ?, ?)"));
    query.addBindValue(sessionId_);
    query.addBindValue(static_cast<int>(seq));
    query.addBindValue(name);
    query.addBindValue(static_cast<qint64>(sentMs));
    query.addBindValue(status);
    query.addBindValue(nackReason);
    (void)query.exec();
}

void HistoryStore::addAudit(std::uint64_t elapsedMs, qint64 wallMs, int role, int action,
                            const QString& subject, bool dispatched, const QString& reason) {
    // ⚠ 这里**不判 sessionId_ > 0**：改数据源、切角色、会话没就绪就被拦下的命令，
    // 都发生在"没有会话"的时候 —— 用 sessionId_ <= 0 当作"不记"的条件，
    // 恰好会把最该留痕的那几件事丢掉。session_id 记 0 只是如实说明当时没有会话。
    if (!open_) {
        return;
    }
    // 与 addEvent 同理：审计必须先落盘。掉电时最要紧的恰恰是刚发生的那一条。
    flush();

    QSqlQuery query(db_);
    query.prepare(QStringLiteral(
        "INSERT INTO audit (wall_ms, t_ms, session_id, role, action, subject, dispatched, reason) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?)"));
    query.addBindValue(wallMs);
    query.addBindValue(static_cast<qint64>(elapsedMs));
    query.addBindValue(sessionId_);
    query.addBindValue(role);
    query.addBindValue(action);
    query.addBindValue(subject);
    query.addBindValue(dispatched ? 1 : 0);
    // ⚠ QString()（null）会被 Qt 绑成 SQL **NULL**，而这两列是 NOT NULL
    // （"总是有一份值，只是可能为空"）。空文本必须写成 ''，不能写成 NULL ——
    // 否则整条流水会被约束挡掉，而它恰恰是"这件事发生过"的唯一凭证。
    query.addBindValue(reason.isNull() ? QString::fromLatin1("") : reason);
    // 与 upsertAlarm / addEvent 不同，这里**留下失败原因**：审计写不进去这件事
    // 必须看得见。默默丢掉一条流水，事后查起来就是"这件事从没发生过"，
    // 而那正是审计最不能出的错。
    if (!query.exec()) {
        error_ = QStringLiteral("写审计失败：%1").arg(query.lastError().text());
    }
}

void HistoryStore::flush() {
    QString ignored;
    (void)flushLocked(ignored);
}

void HistoryStore::tick(qint64 wallMs) {
    if (!open_ || pending_.empty()) {
        return;
    }
    if (wallMs - lastFlushWallMs_ < static_cast<qint64>(kFlushPeriodMs)) {
        return;
    }
    flush();
}

bool HistoryStore::flushLocked(QString& error) {
    lastFlushWallMs_ = QDateTime::currentMSecsSinceEpoch();
    if (!open_ || pending_.empty()) {
        return true;
    }

    // 一个事务写完这一批。任一步失败就整体回滚，并把原因记下来 ——
    // 部分写入的历史比没有历史更糟：它看起来是完整的一段。
    if (!db_.transaction()) {
        error_ = QStringLiteral("开事务失败：%1").arg(db_.lastError().text());
        error = error_;
        pending_.clear();
        return false;
    }

    QSqlQuery query(db_);
    query.prepare(QStringLiteral(
        "INSERT INTO sample (session_id, t_ms, velocity, position, target, state, fault_code, pwm) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?)"));

    for (const TelemetrySample& sample : pending_) {
        query.addBindValue(sessionId_);
        query.addBindValue(static_cast<qint64>(sample.tMs));
        query.addBindValue(static_cast<double>(sample.velocityDegPerSec));
        query.addBindValue(static_cast<double>(sample.positionDeg));
        query.addBindValue(static_cast<double>(sample.target));
        query.addBindValue(static_cast<int>(sample.state));
        query.addBindValue(static_cast<int>(sample.faultCode));
        query.addBindValue(static_cast<int>(sample.pwmOutput));
        if (!query.exec()) {
            error_ = QStringLiteral("写样本失败：%1").arg(query.lastError().text());
            error = error_;
            db_.rollback();
            pending_.clear();
            return false;
        }
    }

    QSqlQuery update(db_);
    update.prepare(QStringLiteral("UPDATE session SET sample_count = sample_count + ? "
                                  "WHERE id = ?"));
    update.addBindValue(static_cast<qint64>(pending_.size()));
    update.addBindValue(sessionId_);
    if (!update.exec()) {
        error_ = QStringLiteral("更新会话计数失败：%1").arg(update.lastError().text());
        error = error_;
        db_.rollback();
        pending_.clear();
        return false;
    }

    if (!db_.commit()) {
        error_ = QStringLiteral("提交失败：%1").arg(db_.lastError().text());
        error = error_;
        db_.rollback();
        pending_.clear();
        return false;
    }

    samplesWritten_ += pending_.size();
    ++transactions_;
    pending_.clear();
    return true;
}

// --- 查询 -------------------------------------------------------------------

std::vector<HistoryStore::SessionRow> HistoryStore::sessions(QString& error) {
    std::vector<SessionRow> out;
    if (!open_) {
        error = error_;
        return out;
    }
    flush();

    QSqlQuery query(db_);
    if (!query.exec(QStringLiteral("SELECT id, started_wall_ms, ended_wall_ms, device_kind, "
                                   "is_simulated, sample_count FROM session ORDER BY id DESC"))) {
        error = query.lastError().text();
        return out;
    }
    while (query.next()) {
        SessionRow row;
        row.id = query.value(0).toLongLong();
        row.startedWallMs = query.value(1).toLongLong();
        row.endedWallMs = query.value(2).isNull() ? -1 : query.value(2).toLongLong();
        row.deviceKind = query.value(3).toString();
        row.isSimulated = query.value(4).toInt() != 0;
        row.sampleCount = query.value(5).toLongLong();
        out.push_back(row);
    }
    return out;
}

std::vector<HistoryStore::SampleRow> HistoryStore::samples(qint64 sessionId, qint64 fromTMs,
                                                           qint64 toTMs, int limit,
                                                           QString& error) {
    std::vector<SampleRow> out;
    if (!open_) {
        error = error_;
        return out;
    }
    flush();

    QSqlQuery query(db_);
    query.prepare(QStringLiteral(
        "SELECT t_ms, velocity, position, target, state, fault_code, pwm FROM sample "
        "WHERE session_id = ? AND t_ms >= ? AND t_ms <= ? ORDER BY t_ms ASC LIMIT ?"));
    query.addBindValue(sessionId);
    query.addBindValue(fromTMs);
    query.addBindValue(toTMs);
    query.addBindValue(limit);
    if (!query.exec()) {
        error = query.lastError().text();
        return out;
    }
    while (query.next()) {
        SampleRow row;
        row.tMs = query.value(0).toLongLong();
        row.velocityDegPerSec = query.value(1).toDouble();
        row.positionDeg = query.value(2).toDouble();
        row.target = query.value(3).toDouble();
        row.state = query.value(4).toInt();
        row.faultCode = query.value(5).toInt();
        row.pwmOutput = query.value(6).toInt();
        out.push_back(row);
    }
    return out;
}

std::vector<HistoryStore::EventRow> HistoryStore::events(qint64 sessionId, QString& error) {
    std::vector<EventRow> out;
    if (!open_) {
        error = error_;
        return out;
    }

    QSqlQuery query(db_);
    query.prepare(QStringLiteral("SELECT t_ms, wall_ms, text FROM event WHERE session_id = ? "
                                 "ORDER BY t_ms ASC"));
    query.addBindValue(sessionId);
    if (!query.exec()) {
        error = query.lastError().text();
        return out;
    }
    while (query.next()) {
        EventRow row;
        row.tMs = query.value(0).toLongLong();
        row.wallMs = query.value(1).toLongLong();
        row.text = query.value(2).toString();
        out.push_back(row);
    }
    return out;
}

std::vector<HistoryStore::AlarmRow> HistoryStore::alarms(qint64 sessionId, QString& error) {
    std::vector<AlarmRow> out;
    if (!open_) {
        error = error_;
        return out;
    }
    flush();

    QSqlQuery query(db_);
    query.prepare(QStringLiteral(
        "SELECT code, level, first_t_ms, last_t_ms, first_wall_ms, last_wall_ms, occurrences, "
        "acknowledged, ack_wall_ms, active, detail FROM alarm WHERE session_id = ? "
        "ORDER BY first_wall_ms ASC, code ASC"));
    query.addBindValue(sessionId);
    if (!query.exec()) {
        error = query.lastError().text();
        return out;
    }
    while (query.next()) {
        AlarmRow row;
        row.code = query.value(0).toInt();
        row.level = query.value(1).toInt();
        row.firstTMs = query.value(2).toLongLong();
        row.lastTMs = query.value(3).toLongLong();
        row.firstWallMs = query.value(4).toLongLong();
        row.lastWallMs = query.value(5).toLongLong();
        row.occurrences = query.value(6).toInt();
        row.acknowledged = query.value(7).toInt() != 0;
        row.ackWallMs = query.value(8).toLongLong();
        row.active = query.value(9).toInt() != 0;
        row.detail = query.value(10).toString();
        out.push_back(row);
    }
    return out;
}

std::vector<HistoryStore::CommandRow> HistoryStore::commands(qint64 sessionId,
                                                            QString& error) {
    std::vector<CommandRow> out;
    if (!open_) {
        error = error_;
        return out;
    }
    flush();

    QSqlQuery query(db_);
    // 倒序：最近下的命令在最上面（与界面上的命令行同一个习惯）。
    query.prepare(QStringLiteral("SELECT sent_ms, seq, name, status, nack_reason FROM command "
                                 "WHERE session_id = ? ORDER BY sent_ms DESC, seq DESC"));
    query.addBindValue(sessionId);
    if (!query.exec()) {
        error = query.lastError().text();
        return out;
    }
    while (query.next()) {
        CommandRow row;
        row.sentMs = query.value(0).toLongLong();
        row.seq = query.value(1).toInt();
        row.name = query.value(2).toString();
        row.status = query.value(3).toInt();
        row.nackReason = query.value(4).toInt();
        out.push_back(row);
    }
    return out;
}

std::vector<HistoryStore::AuditRow> HistoryStore::audits(int limit, QString& error) {
    std::vector<AuditRow> out;
    if (!open_) {
        error = error_;
        return out;
    }
    flush();

    // 倒序（最近的在上）：与界面上的命令行、命令日志同一个习惯。
    // LIMIT 0 在 SQLite 里是"不返回任何行"，所以 0 表示不设限时要**不写 LIMIT**，
    // 而不是写一个 LIMIT 0 上去。
    QString sql = QStringLiteral(
        "SELECT wall_ms, role, action, subject, dispatched, reason FROM audit "
        "ORDER BY wall_ms DESC, id DESC");
    if (limit > 0) {
        sql += QStringLiteral(" LIMIT %1").arg(limit);
    }
    QSqlQuery query(db_);
    if (!query.exec(sql)) {
        error = query.lastError().text();
        return out;
    }
    while (query.next()) {
        AuditRow row;
        row.wallMs = query.value(0).toLongLong();
        row.role = query.value(1).toInt();
        row.action = query.value(2).toInt();
        row.subject = query.value(3).toString();
        row.dispatched = query.value(4).toInt() != 0;
        row.reason = query.value(5).toString();
        out.push_back(row);
    }
    return out;
}

qint64 HistoryStore::countSamples(qint64 sessionId, qint64 fromTMs, qint64 toTMs,
                                  QString& error) {    if (!open_) {
        error = error_;
        return 0;
    }
    flush();
    QSqlQuery query(db_);
    query.prepare(QStringLiteral("SELECT COUNT(*) FROM sample WHERE session_id = ? "
                                 "AND t_ms >= ? AND t_ms <= ?"));
    query.addBindValue(sessionId);
    query.addBindValue(fromTMs);
    query.addBindValue(toTMs);
    if (!query.exec() || !query.next()) {
        error = query.lastError().text();
        return 0;
    }
    return query.value(0).toLongLong();
}

qint64 HistoryStore::totalSamples(QString& error) {
    if (!open_) {
        error = error_;
        return 0;
    }
    flush();
    QSqlQuery query(db_);
    if (!query.exec(QStringLiteral("SELECT COUNT(*) FROM sample")) || !query.next()) {
        error = query.lastError().text();
        return 0;
    }
    return query.value(0).toLongLong();
}

bool HistoryStore::exportSamplesCsv(const QString& filePath, qint64 sessionId, qint64 fromTMs,
                                    qint64 toTMs, QString& error) {
    error.clear();
    if (!open_) {
        error = error_;
        return false;
    }

    const std::vector<SampleRow> rows = samples(sessionId, fromTMs, toTMs, -1, error);
    if (!error.isEmpty()) {
        return false;
    }

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        error = QStringLiteral("写不了 %1：%2").arg(filePath, file.errorString());
        return false;
    }

    // Qt6 的 QTextStream 默认就是 UTF-8，这里不再显式设编码（少一处会过期的 API 用法）。
    QTextStream stream(&file);
    // BOM：Excel 双击打开 UTF-8 CSV 时，没有 BOM 的中文表头会变成乱码。
    // 这份文件是给人看、给人贴进报告的，所以这一条属于"能不能用"而不是洁癖。
    stream << QChar(0xFEFF);
    stream << QStringLiteral("时刻(ms),速度(deg/s),位置(deg),目标,状态,故障码,PWM\r\n");
    for (const SampleRow& row : rows) {
        stream << row.tMs << ',' << QString::number(row.velocityDegPerSec, 'f', 4) << ','
               << QString::number(row.positionDeg, 'f', 4) << ','
               << QString::number(row.target, 'f', 4) << ','
               << QString::fromUtf8(mc::deviceStateName(static_cast<mc::DeviceState>(row.state)))
               << ',' << row.faultCode << ',' << row.pwmOutput << QStringLiteral("\r\n");
    }
    stream.flush();
    file.close();

    exportedRows_ += rows.size();
    return true;
}

}  // namespace mchost
