#pragma once

#include <cstdint>
#include <vector>

#include <QSqlDatabase>
#include <QString>

#include "mc/recipe.hpp"

namespace mchost {

// ---------------------------------------------------------------------------
// 配方库（SQLite）
//
// 与历史库**分成两个文件**，理由不是洁癖：历史是流水（一次性、可以删），
// 配方是配置（要一直留着、还要能回溯"上次用的是哪一版"）。放进同一个库，
// 清理历史就会连配方一起删掉 —— 那是一次数据丢失，不是一次清理。
//
// 每一行都记着写入时的 CRC 校验和。装载时按库里的字段重算一遍并比对，
// 不一致就把这一行标成损坏。
// ⚠️ 它挡的是**位翻转和手工改表**，不是防篡改 —— 有意改库的人顺手把
//    校验和一起改了就行。它的价值在于"这一行还是当初写进去的那一行吗"
//    这个问题有一个自动答案，而不是看上去有。
//
// 损坏的行**照样列出来**：藏掉它等于"库里少了一份配方"，而真相是
// "有一份坏了"。界面必须能说出后者。
// ---------------------------------------------------------------------------
class RecipeStore {
public:
    RecipeStore();
    ~RecipeStore();

    RecipeStore(const RecipeStore&) = delete;
    RecipeStore& operator=(const RecipeStore&) = delete;

    // 用户数据目录下与历史库同级的 recipes.db。
    static QString defaultPath();

    bool open(const QString& dbPath);
    void close();
    bool isOpen() const noexcept { return open_; }
    const QString& path() const noexcept { return path_; }
    const QString& error() const noexcept { return error_; }

    struct Row {
        qint64 id = 0;
        mc::Recipe recipe;
        std::uint16_t storedChecksum = 0U;
        qint64 updatedWallMs = 0;
        bool corrupted = false;
    };

    // 全部配方。库打不开时返回空并写 error —— 调用方必须区分
    // "库是空的"和"库打不开"，因为两者的处置完全不同。
    std::vector<Row> list(QString& error);

    // 新增。同名同版本已存在 → 拒绝：要改就改版本号。
    // 覆盖掉的那一份是"上一次到底跑的是什么参数"的唯一证据，不能顺手毁掉。
    bool insert(const mc::Recipe& recipe, qint64* newId, QString& error);
    bool update(qint64 id, const mc::Recipe& recipe, QString& error);
    bool remove(qint64 id, QString& error);

private:
    bool exec(const QString& sql, QString& error) const;
    bool ensureSchema(QString& error);
    // 同名的另一行占了同样的版本号吗（excludeId 用于更新时排除自己）。
    bool versionTaken(const QString& name, int version, qint64 excludeId, bool& taken, QString& error);

    QSqlDatabase db_;
    QString path_;
    QString error_;
    bool open_ = false;
};

}  // namespace mchost
