#pragma once

#include <cstdint>
#include <vector>

#include <QSqlDatabase>
#include <QString>

#include "mc/sequence.hpp"

namespace mchost {

// ---------------------------------------------------------------------------
// 程序库（SQLite）—— 与配方库/历史库**各占一个文件**，理由同配方库：
// 程序是配置（要一直留着），历史是流水（可以清）。放一起，清历史就会连程序
// 一起删掉 —— 那是一次数据丢失，不是一次清理。
//
// 一个刻意的取舍：**整部程序存成一列 BLOB + 一列校验和**，而不是把每一步摊成
// 一张子表。理由是这里根本没有"按步查询"的需求（界面永远是整部取出来看），
// 而摊成子表要维护"子表与主表一致"这件事 —— 多一处可能不一致的地方，
// 换不来任何东西。规范字节由核心库的 encodeSequence 给出（确定、可校验和）。
//
// ⚠️ 校验和挡的是**位翻转与手工改库**，不是防篡改。它回答的是
//    "这一行还是当初写进去的那一行吗"，而不是"这一行可信吗"。
//    坏掉的行**照样列出来**并标成损坏：藏起来等于"库里少了一部程序"，
//    而真相是"有一部坏了"。
// ---------------------------------------------------------------------------
class SequenceStore {
public:
    SequenceStore();
    ~SequenceStore();

    SequenceStore(const SequenceStore&) = delete;
    SequenceStore& operator=(const SequenceStore&) = delete;

    static QString defaultPath();

    bool open(const QString& dbPath);
    void close();
    bool isOpen() const noexcept { return open_; }
    const QString& path() const noexcept { return path_; }
    const QString& error() const noexcept { return error_; }

    struct Row {
        qint64 id = 0;
        mc::SequenceProgram program;
        std::uint16_t storedChecksum = 0U;
        qint64 updatedWallMs = 0;
        // 校验和对不上，或者字节解不开。两种都叫"这一行不可用"，但原因分开写：
        // 前者是被人动过，后者是格式对不上，排查方向完全不同。
        bool corrupted = false;
        QString problem;   // corrupted 为真时的一句话（为空表示这一行是好的）
    };

    // 全部程序，按名称与版本排序。库打不开时返回空并写 error —— 调用方必须
    // 区分"库是空的"和"库打不开"，两者的处置完全不同。
    std::vector<Row> list(QString& error);

    // 新增。同名同版本已存在 → 拒绝：要改就改版本号。
    // 被覆盖掉的那一版是"上次到底按什么顺序跑的"的唯一证据。
    bool insert(const mc::SequenceProgram& program, qint64* newId, QString& error);
    bool update(qint64 id, const mc::SequenceProgram& program, QString& error);
    bool remove(qint64 id, QString& error);

private:
    bool exec(const QString& sql, QString& error) const;
    bool ensureSchema(QString& error);
    bool versionTaken(const QString& name, int version, qint64 excludeId, bool& taken, QString& error);

    QSqlDatabase db_;
    QString path_;
    QString error_;
    bool open_ = false;
};

}  // namespace mchost
