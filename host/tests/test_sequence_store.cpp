#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include <QDir>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QVariant>

#include "mc_host/sequence_store.hpp"

// 程序库的真实验证：真的开一个 SQLite 文件、真的写、真的读回来。
//
// 刻意**不用内存库**：内存库测不出"重开文件还在不在"，而"上次那部程序还能不能
// 翻出来"恰恰是这个模块唯一的存在理由。
namespace {

mc::SequenceStep makeStep(mc::SequenceStepKind kind) {
    mc::SequenceStep step;
    step.kind = kind;
    return step;
}

mc::SequenceProgram programNamed(const std::string& name, std::uint16_t version) {
    mc::SequenceProgram program;
    program.name = name;
    program.version = version;

    mc::SequenceStep mode;
    mode.kind = mc::SequenceStepKind::SetMode;
    mode.mode = 1U;
    mode.note = "进速度环";

    mc::SequenceStep target;
    target.kind = mc::SequenceStepKind::SetTarget;
    target.target = 200.0F;

    mc::SequenceStep wait;
    wait.kind = mc::SequenceStepKind::Wait;
    wait.waitMs = 500U;

    program.steps = {mode, target, wait};
    return program;
}

// 绕过库自己直接改一行：模拟"位翻转 / 手工改库"。
// 用一个**另开的连接**（库对象自己那条连接被它独占着用），这更接近真实情形。
void tamperBlob(const QString& path, qint64 id, const QByteArray& replacement) {
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                   QStringLiteral("mc_tamper"));
        db.setDatabaseName(path);
        ASSERT_TRUE(db.open());
        QSqlQuery query(db);
        query.prepare(QStringLiteral("UPDATE sequences SET program = ? WHERE id = ?"));
        query.addBindValue(replacement);
        query.addBindValue(id);
        ASSERT_TRUE(query.exec()) << query.lastError().text().toStdString();
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("mc_tamper"));
}

}  // namespace

TEST(SequenceStore, RoundTripsEveryFieldAndSurvivesAReopen) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("sequences.db"));

    qint64 id = 0;
    {
        mchost::SequenceStore store;
        ASSERT_TRUE(store.open(path)) << store.error().toStdString();

        QString error;
        ASSERT_TRUE(store.insert(programNamed("流程一号", 2U), &id, error)) << error.toStdString();

        const std::vector<mchost::SequenceStore::Row> rows = store.list(error);
        ASSERT_TRUE(error.isEmpty()) << error.toStdString();
        ASSERT_EQ(rows.size(), std::size_t{1});
        EXPECT_FALSE(rows[0].corrupted);
        EXPECT_TRUE(rows[0].problem.isEmpty());
        EXPECT_EQ(rows[0].program.name, "流程一号");
        EXPECT_EQ(rows[0].program.version, 2U);
        ASSERT_EQ(rows[0].program.steps.size(), std::size_t{3});
        EXPECT_EQ(rows[0].program.steps[0].kind, mc::SequenceStepKind::SetMode);
        EXPECT_EQ(rows[0].program.steps[0].mode, 1U);
        EXPECT_EQ(rows[0].program.steps[0].note, "进速度环");
        EXPECT_FLOAT_EQ(rows[0].program.steps[1].target, 200.0F);
        EXPECT_EQ(rows[0].program.steps[2].waitMs, 500U);
        // 存进去的那一份的校验和，与当场重算的一致 —— 这就是"没被动过"的判据。
        EXPECT_EQ(mc::sequenceChecksum(rows[0].program), rows[0].storedChecksum);
        store.close();
    }

    // 重开一个对象（新连接）：程序必须还在，而且要能原样解码。
    {
        mchost::SequenceStore store;
        ASSERT_TRUE(store.open(path)) << store.error().toStdString();
        QString error;
        const std::vector<mchost::SequenceStore::Row> rows = store.list(error);
        ASSERT_EQ(rows.size(), std::size_t{1});
        EXPECT_EQ(rows[0].id, id);
        EXPECT_FALSE(rows[0].corrupted);
        EXPECT_EQ(rows[0].program.steps.size(), std::size_t{3});
        EXPECT_EQ(rows[0].program.steps[2].kind, mc::SequenceStepKind::Wait);
    }
}

TEST(SequenceStore, TheSameNameAndVersionCannotBeStoredTwice) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    mchost::SequenceStore store;
    ASSERT_TRUE(store.open(dir.filePath(QStringLiteral("dup.db")))) << store.error().toStdString();

    QString error;
    qint64 first = 0;
    ASSERT_TRUE(store.insert(programNamed("LINE-A", 1U), &first, error)) << error.toStdString();

    // 同名同版本再存一次：拒绝，而且要说清出路是"改版本号"。
    qint64 second = 0;
    EXPECT_FALSE(store.insert(programNamed("LINE-A", 1U), &second, error));
    EXPECT_TRUE(error.contains(QStringLiteral("同名同版本"))) << error.toStdString();
    EXPECT_TRUE(error.contains(QStringLiteral("改版本号"))) << error.toStdString();

    // 大小写不同算同一部：同一个人不会认为两串只差大小写的名字是两部程序，
    // 而列表里出现两条看起来一样的程序、只有一条是对的，是最坏的一种。
    EXPECT_FALSE(store.insert(programNamed("line-a", 1U), &second, error));

    // 换个版本号就能存。
    ASSERT_TRUE(store.insert(programNamed("LINE-A", 2U), &second, error)) << error.toStdString();
    EXPECT_NE(second, first);
}

TEST(SequenceStore, AnInvalidProgramIsRefusedAtTheStoreBoundary) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    mchost::SequenceStore store;
    ASSERT_TRUE(store.open(dir.filePath(QStringLiteral("invalid.db"))))
        << store.error().toStdString();

    mc::SequenceProgram program = programNamed("流程一号", 1U);
    program.steps.clear();   // 一部没有步的程序

    QString error;
    qint64 id = 0;
    EXPECT_FALSE(store.insert(program, &id, error));
    EXPECT_TRUE(error.contains(QStringLiteral("不合法"))) << error.toStdString();

    // 程序**自己设了模式**时，量纲上界判得了：SetMode(1) 之后 20000 deg/s 越界。
    // 这一条不依赖设备，程序里自带判据。
    program = programNamed("流程二号", 1U);
    program.steps[1].target = 20000.0F;
    EXPECT_FALSE(store.insert(program, &id, error));
    EXPECT_TRUE(error.contains(QStringLiteral("TargetOutOfRange"))) << error.toStdString();

    // 程序没设过模式时，库这一层**不替设备猜**：同一个 20000 在这里放行，
    // 因为"它该按 deg/s 还是 deg 判"只有设备那一侧知道。那条检查由启动时
    // 按设备的真实模式做（见 DeviceManager::startSequence）。
    mc::SequenceProgram noMode;
    noMode.name = "流程三号";
    noMode.version = 1U;
    mc::SequenceStep target = makeStep(mc::SequenceStepKind::SetTarget);
    target.target = 20000.0F;
    noMode.steps = {target};
    EXPECT_TRUE(store.insert(noMode, &id, error)) << error.toStdString();
}

TEST(SequenceStore, AProgramEditedFromOutsideShowsUpAsCorruptedInsteadOfLoadingSilently) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("tamper.db"));

    mchost::SequenceStore store;
    ASSERT_TRUE(store.open(path)) << store.error().toStdString();

    QString error;
    qint64 id = 0;
    ASSERT_TRUE(store.insert(programNamed("流程一号", 1U), &id, error)) << error.toStdString();

    store.close();
    // 把等待时长从 500 改成一个听起来更合理、但**不是当初写进去的**值。
    mc::SequenceProgram tampered = programNamed("流程一号", 1U);
    tampered.steps[2].waitMs = 250U;
    const std::vector<std::uint8_t> bytes = mc::encodeSequence(tampered);
    tamperBlob(path, id, QByteArray(reinterpret_cast<const char*>(bytes.data()),
                                    static_cast<int>(bytes.size())));
    ASSERT_TRUE(store.open(path)) << store.error().toStdString();

    const std::vector<mchost::SequenceStore::Row> rows = store.list(error);
    ASSERT_TRUE(error.isEmpty()) << error.toStdString();
    ASSERT_EQ(rows.size(), std::size_t{1});
    // 坏掉的行**照样列出来**并标成损坏：藏起来等于"库里少了一部程序"，
    // 而真相是"有一部坏了"。
    EXPECT_TRUE(rows[0].corrupted);
    EXPECT_FALSE(rows[0].problem.isEmpty());
    EXPECT_EQ(rows[0].program.name, "流程一号");   // 名字还在，能认出是哪一份
}

TEST(SequenceStore, BytesThatCannotBeDecodedAreAlsoReportedAsCorrupted) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("garbage.db"));

    mchost::SequenceStore store;
    ASSERT_TRUE(store.open(path)) << store.error().toStdString();

    QString error;
    qint64 id = 0;
    ASSERT_TRUE(store.insert(programNamed("流程一号", 1U), &id, error)) << error.toStdString();
    store.close();

    // 一串根本不是程序的东西。它**不许**被当成一部空程序悄悄载进来。
    tamperBlob(path, id, QByteArray("\x01\x02\x03", 3));
    ASSERT_TRUE(store.open(path)) << store.error().toStdString();

    const std::vector<mchost::SequenceStore::Row> rows = store.list(error);
    ASSERT_EQ(rows.size(), std::size_t{1});
    EXPECT_TRUE(rows[0].corrupted);
    EXPECT_TRUE(rows[0].problem.contains(QStringLiteral("解不开"))) << rows[0].problem.toStdString();
}

TEST(SequenceStore, UpdateAndRemoveOnlyTouchTheSelectedRow) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    mchost::SequenceStore store;
    ASSERT_TRUE(store.open(dir.filePath(QStringLiteral("edit.db")))) << store.error().toStdString();

    QString error;
    qint64 first = 0;
    qint64 second = 0;
    ASSERT_TRUE(store.insert(programNamed("流程一号", 1U), &first, error)) << error.toStdString();
    ASSERT_TRUE(store.insert(programNamed("流程二号", 1U), &second, error)) << error.toStdString();

    // 更新时要挡住"偷走别人版本号"这件事：被覆盖的那一版是唯一的历史证据。
    mc::SequenceProgram stolen = programNamed("流程二号", 1U);
    stolen.name = "流程一号";
    EXPECT_FALSE(store.update(second, stolen, error));
    EXPECT_TRUE(error.contains(QStringLiteral("另一部程序")));

    mc::SequenceProgram edited = programNamed("流程一号", 1U);
    edited.steps.pop_back();
    ASSERT_TRUE(store.update(first, edited, error)) << error.toStdString();

    const std::vector<mchost::SequenceStore::Row> rows = store.list(error);
    ASSERT_EQ(rows.size(), std::size_t{2});
    EXPECT_EQ(rows[0].program.steps.size(), std::size_t{2}) << "被改的那一行要少一步";
    EXPECT_EQ(rows[1].program.steps.size(), std::size_t{3}) << "另一行不许受影响";

    ASSERT_TRUE(store.remove(first, error)) << error.toStdString();
    EXPECT_EQ(store.list(error).size(), std::size_t{1});

    // 删一条不存在的：报告"没有这一行"，而不是静默成功。
    EXPECT_FALSE(store.update(first, edited, error));
    EXPECT_TRUE(error.contains(QStringLiteral("没有 id")));
}

TEST(SequenceStore, AnUnopenablePathFailsWithAReason) {
    mchost::SequenceStore store;
    const QString bad = QDir::tempPath();   // 目录：SQLite 打不开它
    EXPECT_FALSE(store.open(bad));
    EXPECT_FALSE(store.isOpen());
    EXPECT_FALSE(store.error().isEmpty());

    QString error;
    EXPECT_TRUE(store.list(error).empty());
    EXPECT_FALSE(error.isEmpty());
    EXPECT_FALSE(store.update(1, programNamed("x", 1U), error));
}
