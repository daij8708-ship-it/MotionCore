#include "mc_host/recipe_store.hpp"

#include <QDateTime>
#include <QDir>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QVariant>

namespace mchost {
namespace {

// 与 HistoryStore 同一个理由：连接名是按全局表注册的，两个库同时开着必须各有其名。
QString nextConnectionName() {
    static int counter = 0;
    return QStringLiteral("mc_host_recipes_%1").arg(++counter);
}

}  // namespace

RecipeStore::RecipeStore()
    : db_(QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), nextConnectionName())) {}

RecipeStore::~RecipeStore() {
    close();
}

QString RecipeStore::defaultPath() {
    // 与历史库取同一个目录：用户要备份/清理时，两个文件应当挨在一起。
    // 这里刻意不调用 DeviceManager::defaultHistoryPath() —— 库不该反过来依赖
    // 应用服务层（依赖方向是单向的），五行重复比一条反向依赖便宜。
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dir.isEmpty()) {
        dir = QDir::homePath() + QStringLiteral("/.mc_host");
    }
    QDir().mkpath(dir);
    return dir + QStringLiteral("/recipes.db");
}

bool RecipeStore::open(const QString& dbPath) {
    if (open_ && path_ == dbPath) {
        return true;
    }
    close();

    path_ = dbPath;
    error_.clear();
    db_.setDatabaseName(dbPath);

    if (!db_.open()) {
        error_ = QStringLiteral("打不开配方库 %1：%2").arg(dbPath, db_.lastError().text());
        return false;
    }
    if (!ensureSchema(error_)) {
        db_.close();
        return false;
    }
    open_ = true;
    return true;
}

void RecipeStore::close() {
    if (!open_) {
        return;
    }
    db_.close();
    open_ = false;
}

bool RecipeStore::exec(const QString& sql, QString& error) const {
    QSqlQuery query(db_);
    if (!query.exec(sql)) {
        error = QStringLiteral("%1（%2）").arg(query.lastError().text(), sql.left(80));
        return false;
    }
    return true;
}

bool RecipeStore::ensureSchema(QString& error) {
    if (!exec(QStringLiteral("PRAGMA journal_mode=WAL"), error)) {
        return false;
    }
    if (!exec(QStringLiteral("PRAGMA synchronous=NORMAL"), error)) {
        return false;
    }

    // name 上的 COLLATE NOCASE 不是随手加的：同一个人不会认为 WAFER-A1 与
    // wafer-a1 是两份不同的配方，而唯一约束若区分大小写，两份就会同时存在 ——
    // 然后操作员在列表里看到两条看起来一样的配方，却只有一条是对的。
    return exec(QStringLiteral("CREATE TABLE IF NOT EXISTS recipes ("
                               "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                               "name TEXT NOT NULL COLLATE NOCASE,"
                               "version INTEGER NOT NULL,"
                               "mode INTEGER NOT NULL,"
                               "target REAL NOT NULL,"
                               "kp REAL NOT NULL,"
                               "ki REAL NOT NULL,"
                               "kd REAL NOT NULL,"
                               "checksum INTEGER NOT NULL,"
                               "updated_wall_ms INTEGER NOT NULL,"
                               "UNIQUE(name, version))"),
                error);
}

bool RecipeStore::versionTaken(const QString& name, int version, qint64 excludeId, bool& taken,
                               QString& error) {
    taken = false;
    QSqlQuery query(db_);
    query.prepare(QStringLiteral("SELECT id FROM recipes WHERE name = ? AND version = ?"));
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

std::vector<RecipeStore::Row> RecipeStore::list(QString& error) {
    std::vector<Row> out;
    if (!open_) {
        error = QStringLiteral("配方库未打开");
        return out;
    }

    QSqlQuery query(db_);
    if (!query.exec(QStringLiteral("SELECT id, name, version, mode, target, kp, ki, kd, checksum, "
                                   "updated_wall_ms FROM recipes ORDER BY name, version"))) {
        error = query.lastError().text();
        return out;
    }

    while (query.next()) {
        Row row;
        row.id = query.value(0).toLongLong();
        row.recipe.name = query.value(1).toString().toStdString();
        row.recipe.version = static_cast<std::uint16_t>(query.value(2).toInt());
        row.recipe.mode = static_cast<std::uint8_t>(query.value(3).toInt());
        row.recipe.target = query.value(4).toFloat();
        row.recipe.kp = query.value(5).toFloat();
        row.recipe.ki = query.value(6).toFloat();
        row.recipe.kd = query.value(7).toFloat();
        row.storedChecksum = static_cast<std::uint16_t>(query.value(8).toInt());
        row.updatedWallMs = query.value(9).toLongLong();

        // float → double → float 的往返是精确的（REAL 存的就是 double），
        // 所以这里比的是"同一份配方算两次"，任何不一致都来自库里那一行被动过。
        row.corrupted = (mc::recipeChecksum(row.recipe) != row.storedChecksum);
        out.push_back(row);
    }
    return out;
}

bool RecipeStore::insert(const mc::Recipe& recipe, qint64* newId, QString& error) {
    if (!open_) {
        error = QStringLiteral("配方库未打开");
        return false;
    }
    // 语义校验放在这里再跑一遍：库是边界（写进来的不止界面一条路），
    // 而"配方库里躺着一条非法配方"这种事没有任何合理的处置方式。
    const mc::RecipeError verdict = mc::validateRecipe(recipe);
    if (verdict != mc::RecipeError::None) {
        error = QStringLiteral("配方本身不合法：%1").arg(QString::fromUtf8(mc::recipeErrorName(verdict)));
        return false;
    }

    bool taken = false;
    if (!versionTaken(QString::fromStdString(recipe.name), recipe.version, -1, taken, error)) {
        return false;
    }
    if (taken) {
        error = QStringLiteral("已存在同名同版本的配方（%1 v%2）：要改请改版本号")
                    .arg(QString::fromStdString(recipe.name))
                    .arg(recipe.version);
        return false;
    }

    QSqlQuery query(db_);
    query.prepare(QStringLiteral("INSERT INTO recipes (name, version, mode, target, kp, ki, kd, "
                                 "checksum, updated_wall_ms) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    query.addBindValue(QString::fromStdString(recipe.name));
    query.addBindValue(static_cast<int>(recipe.version));
    query.addBindValue(static_cast<int>(recipe.mode));
    query.addBindValue(static_cast<double>(recipe.target));
    query.addBindValue(static_cast<double>(recipe.kp));
    query.addBindValue(static_cast<double>(recipe.ki));
    query.addBindValue(static_cast<double>(recipe.kd));
    query.addBindValue(static_cast<int>(mc::recipeChecksum(recipe)));
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

bool RecipeStore::update(qint64 id, const mc::Recipe& recipe, QString& error) {
    if (!open_) {
        error = QStringLiteral("配方库未打开");
        return false;
    }
    const mc::RecipeError verdict = mc::validateRecipe(recipe);
    if (verdict != mc::RecipeError::None) {
        error = QStringLiteral("配方本身不合法：%1").arg(QString::fromUtf8(mc::recipeErrorName(verdict)));
        return false;
    }

    bool taken = false;
    if (!versionTaken(QString::fromStdString(recipe.name), recipe.version, id, taken, error)) {
        return false;
    }
    if (taken) {
        error = QStringLiteral("另一条配方已占用 %1 v%2")
                    .arg(QString::fromStdString(recipe.name))
                    .arg(recipe.version);
        return false;
    }

    QSqlQuery query(db_);
    query.prepare(QStringLiteral("UPDATE recipes SET name = ?, version = ?, mode = ?, target = ?, "
                                 "kp = ?, ki = ?, kd = ?, checksum = ?, updated_wall_ms = ? "
                                 "WHERE id = ?"));
    query.addBindValue(QString::fromStdString(recipe.name));
    query.addBindValue(static_cast<int>(recipe.version));
    query.addBindValue(static_cast<int>(recipe.mode));
    query.addBindValue(static_cast<double>(recipe.target));
    query.addBindValue(static_cast<double>(recipe.kp));
    query.addBindValue(static_cast<double>(recipe.ki));
    query.addBindValue(static_cast<double>(recipe.kd));
    query.addBindValue(static_cast<int>(mc::recipeChecksum(recipe)));
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    query.addBindValue(id);
    if (!query.exec()) {
        error = query.lastError().text();
        return false;
    }
    if (query.numRowsAffected() <= 0) {
        error = QStringLiteral("没有 id = %1 这条配方").arg(id);
        return false;
    }
    return true;
}

bool RecipeStore::remove(qint64 id, QString& error) {
    if (!open_) {
        error = QStringLiteral("配方库未打开");
        return false;
    }
    QSqlQuery query(db_);
    query.prepare(QStringLiteral("DELETE FROM recipes WHERE id = ?"));
    query.addBindValue(id);
    if (!query.exec()) {
        error = query.lastError().text();
        return false;
    }
    return true;
}

}  // namespace mchost
