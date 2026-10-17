#include "mc_host/sequence_store.hpp"

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QVariant>

namespace mchost {
namespace {

// 与 HistoryStore / RecipeStore 同一个理由：连接名是按全局表注册的，
// 三个库同时开着必须各有其名。
QString nextConnectionName() {
    static int counter = 0;
    return QStringLiteral("mc_host_sequences_%1").arg(++counter);
}

}  // namespace

SequenceStore::SequenceStore()
    : db_(QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), nextConnectionName())) {}

SequenceStore::~SequenceStore() {
    close();
}

QString SequenceStore::defaultPath() {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dir.isEmpty()) {
        dir = QDir::homePath() + QStringLiteral("/.mc_host");
    }
    QDir().mkpath(dir);
    return dir + QStringLiteral("/sequences.db");
}

bool SequenceStore::open(const QString& dbPath) {
    if (open_ && path_ == dbPath) {
        return true;
    }
    close();

    path_ = dbPath;
    error_.clear();
    db_.setDatabaseName(dbPath);
    if (!db_.open()) {
        error_ = QStringLiteral("打不开程序库 %1：%2").arg(dbPath, db_.lastError().text());
        return false;
    }
    if (!ensureSchema(error_)) {
        db_.close();
        return false;
    }
    open_ = true;
    return true;
}

void SequenceStore::close() {
    if (!open_) {
        return;
    }
    db_.close();
    open_ = false;
}

bool SequenceStore::exec(const QString& sql, QString& error) const {
    QSqlQuery query(db_);
    if (!query.exec(sql)) {
        error = QStringLiteral("%1（%2）").arg(query.lastError().text(), sql.left(80));
        return false;
    }
    return true;
}

bool SequenceStore::ensureSchema(QString& error) {
    if (!exec(QStringLiteral("PRAGMA journal_mode=WAL"), error)) {
        return false;
    }
    if (!exec(QStringLiteral("PRAGMA synchronous=NORMAL"), error)) {
        return false;
    }
    // name/version 用列存着（而不是只藏在 BLOB 里）：这样**坏掉的行也还能被认出来**
    // —— 列表上要显示"TASK-A v2 这一份坏了"，而不是一行"（无法识别）"。
    // COLLATE NOCASE 的理由与配方库同：同一个人不会认为两串只差大小写的名字是两部程序。
    return exec(QStringLiteral("CREATE TABLE IF NOT EXISTS sequences ("
                               "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                               "name TEXT NOT NULL COLLATE NOCASE,"
                               "version INTEGER NOT NULL,"
                               "program BLOB NOT NULL,"
                               "checksum INTEGER NOT NULL,"
                               "updated_wall_ms INTEGER NOT NULL,"
                               "UNIQUE(name, version))"),
                error);
}

bool SequenceStore::versionTaken(const QString& name, int version, qint64 excludeId, bool& taken,
                                 QString& error) {
    taken = false;
    QSqlQuery query(db_);
    query.prepare(QStringLiteral("SELECT id FROM sequences WHERE name = ? AND version = ?"));
    query.addBindValue(name);
    query.addBindValue(version);
    if (!query.exec()) {
        error = query.lastError().text();
        return false;
    }
    while (query.next()) {
        if (query.value(0).toLongLong() != excludeId) {
            taken = true;
            break;
        }
    }
    return true;
}

std::vector<SequenceStore::Row> SequenceStore::list(QString& error) {
    std::vector<Row> out;
    if (!open_) {
        error = QStringLiteral("程序库未打开");
        return out;
    }

    QSqlQuery query(db_);
    if (!query.exec(QStringLiteral("SELECT id, name, version, program, checksum, updated_wall_ms "
                                   "FROM sequences ORDER BY name, version"))) {
        error = query.lastError().text();
        return out;
    }

    while (query.next()) {
        Row row;
        row.id = query.value(0).toLongLong();
        row.updatedWallMs = query.value(5).toLongLong();
        row.storedChecksum = static_cast<std::uint16_t>(query.value(4).toInt());

        const QByteArray blob = query.value(3).toByteArray();
        const auto* data = reinterpret_cast<const std::uint8_t*>(blob.constData());
        const mc::SequenceError verdict =
            mc::decodeSequence(data, static_cast<std::size_t>(blob.size()), row.program);
        if (verdict != mc::SequenceError::None) {
            row.corrupted = true;
            row.problem = QStringLiteral("这一行的字节解不开（%1）")
                              .arg(QString::fromUtf8(mc::sequenceErrorName(verdict)));
            // 名字与版本仍从列里取：坏掉的行**照样要能被认出来**。
            row.program.name = query.value(1).toString().toStdString();
            row.program.version = static_cast<std::uint16_t>(query.value(2).toInt());
            out.push_back(row);
            continue;
        }

        // 校验和覆盖的就是这份字节，所以它挡得住"某几个字节被改了"。
        if (mc::sequenceChecksum(row.program) != row.storedChecksum) {
            row.corrupted = true;
            row.problem = QStringLiteral("这一行与写入时不一致（校验和对不上）");
        }
        out.push_back(row);
    }
    return out;
}

bool SequenceStore::insert(const mc::SequenceProgram& program, qint64* newId, QString& error) {
    if (!open_) {
        error = QStringLiteral("程序库未打开");
        return false;
    }
    // 语义校验在库这一层再跑一遍：写进来的不止界面一条路，
    // 而"库里的程序是跑不了的"这件事没有任何合理的处置方式。
    //
    // 初始模式给 0（停止控制）：库这一层**不知道设备当时是什么模式**，
    // 所以它不替设备猜"这个目标值该按 deg/s 还是 deg 判"。量纲上界那条检查
    // 由启动时按设备的**真实模式**做 —— 那时候才有资格判。
    const mc::SequenceError verdict = mc::validateSequence(program, 0U);
    if (verdict != mc::SequenceError::None) {
        error = QStringLiteral("程序本身不合法：%1")
                    .arg(QString::fromUtf8(mc::sequenceErrorName(verdict)));
        return false;
    }

    const QString name = QString::fromStdString(program.name);
    bool taken = false;
    if (!versionTaken(name, program.version, -1, taken, error)) {
        return false;
    }
    if (taken) {
        error = QStringLiteral("已存在同名同版本的程序（%1 v%2）：要改请改版本号")
                    .arg(name)
                    .arg(program.version);
        return false;
    }

    const std::vector<std::uint8_t> bytes = mc::encodeSequence(program);
    const QByteArray blob(reinterpret_cast<const char*>(bytes.data()),
                          static_cast<int>(bytes.size()));

    QSqlQuery query(db_);
    query.prepare(QStringLiteral("INSERT INTO sequences (name, version, program, checksum, "
                                 "updated_wall_ms) VALUES (?, ?, ?, ?, ?)"));
    query.addBindValue(name);
    query.addBindValue(static_cast<int>(program.version));
    query.addBindValue(blob);
    query.addBindValue(static_cast<int>(mc::sequenceChecksum(program)));
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    if (!query.exec()) {
        error = query.lastError().text();
        return false;
    }
    if (newId != nullptr) {
        *newId = query.lastInsertId().toLongLong();
    }
    return true;
}

bool SequenceStore::update(qint64 id, const mc::SequenceProgram& program, QString& error) {
    if (!open_) {
        error = QStringLiteral("程序库未打开");
        return false;
    }
    // 初始模式给 0 的理由与 insert 相同（见那里的说明）。
    const mc::SequenceError verdict = mc::validateSequence(program, 0U);
    if (verdict != mc::SequenceError::None) {
        error = QStringLiteral("程序本身不合法：%1")
                    .arg(QString::fromUtf8(mc::sequenceErrorName(verdict)));
        return false;
    }

    const QString name = QString::fromStdString(program.name);
    bool taken = false;
    if (!versionTaken(name, program.version, id, taken, error)) {
        return false;
    }
    if (taken) {
        error = QStringLiteral("另一部程序已占用 %1 v%2").arg(name).arg(program.version);
        return false;
    }

    const std::vector<std::uint8_t> bytes = mc::encodeSequence(program);
    const QByteArray blob(reinterpret_cast<const char*>(bytes.data()),
                          static_cast<int>(bytes.size()));

    QSqlQuery query(db_);
    query.prepare(QStringLiteral("UPDATE sequences SET name = ?, version = ?, program = ?, "
                                 "checksum = ?, updated_wall_ms = ? WHERE id = ?"));
    query.addBindValue(name);
    query.addBindValue(static_cast<int>(program.version));
    query.addBindValue(blob);
    query.addBindValue(static_cast<int>(mc::sequenceChecksum(program)));
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    query.addBindValue(id);
    if (!query.exec()) {
        error = query.lastError().text();
        return false;
    }
    if (query.numRowsAffected() <= 0) {
        error = QStringLiteral("没有 id = %1 这部程序").arg(id);
        return false;
    }
    return true;
}

bool SequenceStore::remove(qint64 id, QString& error) {
    if (!open_) {
        error = QStringLiteral("程序库未打开");
        return false;
    }
    QSqlQuery query(db_);
    query.prepare(QStringLiteral("DELETE FROM sequences WHERE id = ?"));
    query.addBindValue(id);
    if (!query.exec()) {
        error = query.lastError().text();
        return false;
    }
    return true;
}

}  // namespace mchost
