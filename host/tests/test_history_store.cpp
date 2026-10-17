#include <gtest/gtest.h>

#include <cstdint>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>

#include "mc/alarm.hpp"
#include "mc/audit.hpp"
#include "mc_host/history_store.hpp"

// 历史落库的真实验证：真的开一个 SQLite 文件、真的写、真的读回来。
//
// 刻意**不用内存库**（":memory:"）：内存库测不出 WAL、也测不出"重开文件还在不在"——
// 而"程序重启后还能查到上周的数据"恰恰是这个模块唯一的存在理由。
namespace {

mchost::TelemetrySample sample(std::uint64_t tMs, float velocity) {
    mchost::TelemetrySample s;
    s.tMs = tMs;
    s.velocityDegPerSec = velocity;
    s.positionDeg = velocity * 2.0F;
    s.target = 200.0F;
    s.state = 3U;   // Running
    s.faultCode = 0U;
    s.pwmOutput = 42U;
    return s;
}

}  // namespace

TEST(HistoryStore, PersistsSessionsSamplesEventsAndCommandsAcrossReopen) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("history.db"));

    {
        mchost::HistoryStore store;
        ASSERT_TRUE(store.open(path)) << store.error().toStdString();
        EXPECT_TRUE(store.isOpen());
        EXPECT_TRUE(store.error().isEmpty());

        ASSERT_TRUE(store.beginSession(QStringLiteral("进程内模拟器"), true, 1'700'000'000'000LL));
        for (int i = 0; i < 100; ++i) {
            store.addSample(sample(static_cast<std::uint64_t>(i * 10), static_cast<float>(i)));
        }
        store.addEvent(150U, 1'700'000'000'150LL, QStringLiteral("链路失效：会话已作废"));
        store.upsertCommand(0U, 2U, QStringLiteral("SET_MODE"), 2, 0);
        store.upsertCommand(100U, 4U, QStringLiteral("START"), 2, 0);
        // 同一条命令的状态更新必须落在**同一行**上，而不是多出一条"还在等应答"。
        store.upsertCommand(100U, 4U, QStringLiteral("START"), 1, 0);

        QString error;
        const std::vector<mchost::HistoryStore::SessionRow> sessions = store.sessions(error);
        ASSERT_TRUE(error.isEmpty()) << error.toStdString();
        ASSERT_EQ(sessions.size(), std::size_t{1});
        EXPECT_EQ(sessions[0].sampleCount, 100);
        EXPECT_EQ(sessions[0].deviceKind, QStringLiteral("进程内模拟器"));
        EXPECT_TRUE(sessions[0].isSimulated);
        EXPECT_EQ(sessions[0].endedWallMs, -1);   // 还没结束

        EXPECT_EQ(store.countSamples(sessions[0].id, 0, 1000, error), 100);
        EXPECT_EQ(store.countSamples(sessions[0].id, 0, 499, error), 50);
        EXPECT_EQ(store.totalSamples(error), 100);
        store.endSession(1'700'000'002'000LL);
        EXPECT_FALSE(store.hasOpenSession());
    }

    // 重开一个进程（新的对象、新的连接）：数据必须还在。
    {
        mchost::HistoryStore store;
        ASSERT_TRUE(store.open(path)) << store.error().toStdString();

        QString error;
        const std::vector<mchost::HistoryStore::SessionRow> sessions = store.sessions(error);
        ASSERT_EQ(sessions.size(), std::size_t{1});
        EXPECT_EQ(sessions[0].endedWallMs, 1'700'000'002'000LL);

        const std::vector<mchost::HistoryStore::SampleRow> rows =
            store.samples(sessions[0].id, 0, 1000, 10, error);
        ASSERT_EQ(rows.size(), std::size_t{10});
        EXPECT_EQ(rows[0].tMs, 0);
        EXPECT_FLOAT_EQ(static_cast<float>(rows[3].velocityDegPerSec), 3.0F);
        EXPECT_EQ(rows[0].state, 3);   // 状态名要能还原回协议里的取值

        const std::vector<mchost::HistoryStore::EventRow> events = store.events(sessions[0].id, error);
        ASSERT_EQ(events.size(), std::size_t{1});
        EXPECT_TRUE(events[0].text.contains(QStringLiteral("链路失效")));

        const std::vector<mchost::HistoryStore::CommandRow> commands =
            store.commands(sessions[0].id, error);
        ASSERT_EQ(commands.size(), std::size_t{2});
        // 倒序：最近下的命令在前。START 被更新成"等待应答"（而不是留下两条）。
        EXPECT_EQ(commands[0].name, QStringLiteral("START"));
        EXPECT_EQ(commands[0].status, 1);
        EXPECT_EQ(commands[1].name, QStringLiteral("SET_MODE"));
        EXPECT_EQ(commands[1].status, 2);
    }
}

// 批量落盘的**证据**：样本数 / 事务数要接近批大小，而不是 1。
// 一行一个事务 = 每 10 ms 让 SQLite 刷一次盘，那正是这个模块必须避免的事。
TEST(HistoryStore, WritesSamplesInBatchesNotOneTransactionPerSample) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    mchost::HistoryStore store;
    ASSERT_TRUE(store.open(dir.filePath(QStringLiteral("batch.db")))) << store.error().toStdString();
    ASSERT_TRUE(store.beginSession(QStringLiteral("进程内模拟器"), true, 0));

    constexpr std::size_t kSamples = mchost::HistoryStore::kBatchSize * 3U;
    for (std::size_t i = 0; i < kSamples; ++i) {
        store.addSample(sample(i * 10U, static_cast<float>(i)));
    }
    store.flush();

    EXPECT_EQ(store.samplesWritten(), static_cast<std::uint64_t>(kSamples));
    EXPECT_EQ(store.pendingSamples(), std::size_t{0});
    // 恰好三批（每 kBatchSize 个样本触发一次），不是 kSamples 个事务。
    EXPECT_EQ(store.transactions(), std::uint64_t{3});
    EXPECT_LT(store.transactions(), kSamples);
}

// 导出 CSV：带 BOM、CRLF、表头是中文 —— 这份文件是给人双击打开的。
TEST(HistoryStore, ExportsCsvWithBomAndReadableHeaders) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    mchost::HistoryStore store;
    ASSERT_TRUE(store.open(dir.filePath(QStringLiteral("export.db")))) << store.error().toStdString();
    ASSERT_TRUE(store.beginSession(QStringLiteral("进程内模拟器"), true, 0));
    for (int i = 0; i < 5; ++i) {
        store.addSample(sample(static_cast<std::uint64_t>(i * 10), static_cast<float>(i) * 1.5F));
    }

    const QString csv = dir.filePath(QStringLiteral("out.csv"));
    QString error;
    ASSERT_TRUE(store.exportSamplesCsv(csv, store.sessionId(), 0, 1000, error))
        << error.toStdString();
    EXPECT_EQ(store.exportedRows(), std::uint64_t{5});

    QFile file(csv);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const QByteArray bytes = file.readAll();
    file.close();

    // BOM：Excel 没有它会按本地编码解，中文表头直接乱码。
    ASSERT_GE(bytes.size(), 3);
    EXPECT_EQ(static_cast<unsigned char>(bytes.at(0)), 0xEFU);
    EXPECT_EQ(static_cast<unsigned char>(bytes.at(1)), 0xBBU);
    EXPECT_EQ(static_cast<unsigned char>(bytes.at(2)), 0xBFU);

    const QString text = QString::fromUtf8(bytes);
    EXPECT_TRUE(text.contains(QStringLiteral("时刻(ms),速度(deg/s)")));
    EXPECT_TRUE(text.contains(QStringLiteral("\r\n"))) << "Excel 对 LF-only 的兼容不好";
    EXPECT_TRUE(text.contains(QStringLiteral("Running")))
        << "状态列用的是协议里的状态名，不是另起的中文别名";
    // 5 条样本 + 1 行表头 = 6 行。
    EXPECT_EQ(text.count(QStringLiteral("\r\n")), 6);
}

// 报警落库：代码、等级、确认状态都要留下来，而且**重开还在**。
// 只把一句人话存进事件流水是不够的 —— "这一段里有多少条 Error 以上没确认"
// 那种问题，只有在保存了代码和等级的前提下才答得出来。
TEST(HistoryStore, PersistsAlarmsWithTheirCodeLevelAndAcknowledgement) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("alarms.db"));

    {
        mchost::HistoryStore store;
        ASSERT_TRUE(store.open(path)) << store.error().toStdString();
        ASSERT_TRUE(store.beginSession(QStringLiteral("进程内模拟器"), true, 1'000'000LL));

        mc::AlarmEntry entry;
        entry.code = mc::kAlarmCodeCommLoss;
        entry.level = mc::AlarmLevel::Fatal;
        entry.detail = "运行中太久没收到合法帧";
        entry.firstTMs = 150U;
        entry.lastTMs = 350U;
        entry.firstWallMs = 1'000'150LL;
        entry.wallMs = 1'000'350LL;
        entry.occurrences = 3U;
        entry.active = false;
        entry.acknowledged = false;
        store.upsertAlarm(entry);

        // 同一起报警的后续更新（次数推进、被确认）必须落在**同一行**上，
        // 而不是每条状态各写一行 —— 那会让"一共出过几起"变得没法数。
        entry.lastTMs = 500U;
        entry.wallMs = 1'000'500LL;
        entry.occurrences = 5U;
        entry.acknowledged = true;
        entry.ackWallMs = 1'000'900LL;
        store.upsertAlarm(entry);

        QString error;
        const std::vector<mchost::HistoryStore::AlarmRow> rows =
            store.alarms(store.sessionId(), error);
        ASSERT_TRUE(error.isEmpty()) << error.toStdString();
        ASSERT_EQ(rows.size(), std::size_t{1});
        EXPECT_EQ(rows[0].code, static_cast<int>(mc::kAlarmCodeCommLoss));
        EXPECT_EQ(rows[0].level, static_cast<int>(mc::AlarmLevel::Fatal));
        // "第一次"不会被后来的更新推后 —— 这是事件开始的时间，不是结束时间。
        EXPECT_EQ(rows[0].firstWallMs, 1'000'150LL);
        EXPECT_EQ(rows[0].lastWallMs, 1'000'500LL);
        EXPECT_EQ(rows[0].occurrences, 5);
        EXPECT_TRUE(rows[0].acknowledged);
        EXPECT_EQ(rows[0].ackWallMs, 1'000'900LL);
        EXPECT_FALSE(rows[0].active);
        EXPECT_EQ(rows[0].detail, QStringLiteral("运行中太久没收到合法帧"));
    }

    // 重开一个对象（新连接）：报警必须还在。
    {
        mchost::HistoryStore store;
        ASSERT_TRUE(store.open(path)) << store.error().toStdString();
        QString error;
        const std::vector<mchost::HistoryStore::SessionRow> sessions = store.sessions(error);
        ASSERT_EQ(sessions.size(), std::size_t{1});
        const std::vector<mchost::HistoryStore::AlarmRow> rows =
            store.alarms(sessions[0].id, error);
        ASSERT_EQ(rows.size(), std::size_t{1});
        EXPECT_TRUE(rows[0].acknowledged);
        EXPECT_EQ(rows[0].code, static_cast<int>(mc::kAlarmCodeCommLoss));
    }
}

// 清空清单之后同一个码再报一次，是**另一起**事件：库里要两条。
// 只用 (会话, 代码) 做键的话，新事件会把上一行的次数覆盖掉 ——
// 那等于"点一下清屏就把库里的历史抹了"。
TEST(HistoryStore, TheSameCodeAfterAClearBecomesASecondRowInsteadOfOverwritingTheFirst) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    mchost::HistoryStore store;
    ASSERT_TRUE(store.open(dir.filePath(QStringLiteral("twice.db")))) << store.error().toStdString();
    ASSERT_TRUE(store.beginSession(QStringLiteral("进程内模拟器"), true, 0));

    const auto raised = [](std::uint64_t firstWallMs, std::uint32_t occurrences) {
        mc::AlarmEntry entry;
        entry.code = mc::kAlarmCodeLinkLost;
        entry.level = mc::AlarmLevel::Error;
        entry.firstTMs = 10U;
        entry.lastTMs = 20U;
        entry.firstWallMs = firstWallMs;
        entry.wallMs = firstWallMs + 20U;
        entry.occurrences = occurrences;
        return entry;
    };

    store.upsertAlarm(raised(1'000LL, 3U));
    store.upsertAlarm(raised(9'000LL, 1U));

    QString error;
    const std::vector<mchost::HistoryStore::AlarmRow> rows = store.alarms(store.sessionId(), error);
    ASSERT_TRUE(error.isEmpty()) << error.toStdString();
    ASSERT_EQ(rows.size(), std::size_t{2});
    // 按首次时间排序：先出的事在前。
    EXPECT_EQ(rows[0].firstWallMs, 1'000LL);
    EXPECT_EQ(rows[0].occurrences, 3);
    EXPECT_EQ(rows[1].firstWallMs, 9'000LL);
    EXPECT_EQ(rows[1].occurrences, 1);
}

// 打不开的库必须**说得出原因**，而且所有访问器都要能安全地被调用 ——
// 界面就是这么用的（打不开时它要显示"没有在落库"而不是崩掉）。
TEST(HistoryStore, AnUnopenableDatabaseIsReportedAndStaysSafeToQuery) {
    mchost::HistoryStore store;
    // 路径指向一个目录：SQLite 打不开它。
    const QString bad = QDir::tempPath();
    EXPECT_FALSE(store.open(bad));
    EXPECT_FALSE(store.isOpen());
    EXPECT_FALSE(store.error().isEmpty());

    EXPECT_FALSE(store.beginSession(QStringLiteral("x"), true, 0));
    store.addSample(sample(0U, 0.0F));   // 不该崩、也不该假装写成功
    // 审计在这里**照样要写**（没打开就是写不进去，但接口不能崩）。
    store.addAudit(0U, 1LL, 2, 1, QStringLiteral("启动"), false, QStringLiteral("没库"));
    store.flush();

    QString error;
    EXPECT_TRUE(store.sessions(error).empty());
    EXPECT_TRUE(store.audits(0, error).empty());
    EXPECT_EQ(store.totalSamples(error), 0);
    EXPECT_FALSE(store.exportSamplesCsv(QDir::tempPath() + QStringLiteral("/x.csv"), 1, 0, 1, error));
}

// 审计流水与上面几张表的关键差别：**没有会话也要写，而且同一条操作做两次就是两行**。
// 前者是因为改数据源、切角色、被拦下的命令恰恰发生在没有会话的时候；
// 后者是因为审计记的是"发生过几次"，去重就把次数抹掉了。
TEST(HistoryStore, AuditRowsAreAppendedWithoutASessionAndWithoutDeduplication) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("audit.db"));

    {
        mchost::HistoryStore store;
        ASSERT_TRUE(store.open(path)) << store.error().toStdString();
        // 刻意**不** beginSession：这是"还没连上设备时就想改数据源"的那一段。
        EXPECT_FALSE(store.hasOpenSession());
        store.addAudit(100U, 1'700'000'000'100LL, static_cast<int>(mc::OperatorRole::Engineer),
                       static_cast<int>(mc::OperatorAction::SetDataSource),
                       QStringLiteral("数据源 = 进程内模拟设备"), true, QString());
        store.addAudit(200U, 1'700'000'000'200LL, static_cast<int>(mc::OperatorRole::Viewer),
                       static_cast<int>(mc::OperatorAction::Start), QStringLiteral("启动"), false,
                       QStringLiteral("当前角色是「观察者」，使能并启动 需要「运动控制」权限"));
        // 同一个动作、同一个角色、同一个对象再来一次：必须是第二行。
        store.addAudit(300U, 1'700'000'000'300LL, static_cast<int>(mc::OperatorRole::Viewer),
                       static_cast<int>(mc::OperatorAction::Start), QStringLiteral("启动"), false,
                       QStringLiteral("当前角色是「观察者」，使能并启动 需要「运动控制」权限"));

        QString error;
        const std::vector<mchost::HistoryStore::AuditRow> rows = store.audits(0, error);
        ASSERT_TRUE(error.isEmpty()) << error.toStdString();
        // 失败时把库里实际有的列出来：只报"少了一条"会让人再跑一轮去查到底有什么。
        const auto describe = [&rows]() {
            QString text;
            for (const mchost::HistoryStore::AuditRow& r : rows) {
                text += QStringLiteral("[%1/%2/%3/%4] ")
                            .arg(r.wallMs)
                            .arg(r.role)
                            .arg(r.action)
                            .arg(r.dispatched ? QStringLiteral("已执行") : QStringLiteral("未执行"));
            }
            return text.toStdString();
        };
        ASSERT_EQ(rows.size(), std::size_t{3}) << describe() << " 库的错误：" << store.error().toStdString();
        // 倒序：最近的在上（与命令行、命令日志同一个习惯）。
        EXPECT_EQ(rows[0].wallMs, 1'700'000'000'300LL);
        EXPECT_EQ(rows[0].role, static_cast<int>(mc::OperatorRole::Viewer));
        EXPECT_EQ(rows[0].action, static_cast<int>(mc::OperatorAction::Start));
        EXPECT_FALSE(rows[0].dispatched);
        EXPECT_FALSE(rows[0].reason.isEmpty());
        EXPECT_EQ(rows[2].wallMs, 1'700'000'000'100LL);
        EXPECT_TRUE(rows[2].dispatched);
        EXPECT_TRUE(rows[2].reason.isEmpty());
        EXPECT_EQ(rows[2].subject, QStringLiteral("数据源 = 进程内模拟设备"));

        // limit 真的截断，而且是"最近 N 条"。
        const std::vector<mchost::HistoryStore::AuditRow> two = store.audits(2, error);
        ASSERT_EQ(two.size(), std::size_t{2});
        EXPECT_EQ(two[0].wallMs, 1'700'000'000'300LL);
        EXPECT_EQ(two[1].wallMs, 1'700'000'000'200LL);
    }

    // 重开：流水必须还在（它是唯一一份"长期"的记录，内存里那本重启就没了）。
    {
        mchost::HistoryStore store;
        ASSERT_TRUE(store.open(path)) << store.error().toStdString();
        QString error;
        const std::vector<mchost::HistoryStore::AuditRow> rows = store.audits(0, error);
        ASSERT_EQ(rows.size(), std::size_t{3}) << "审计没落住：重开就少了几条";
        EXPECT_EQ(rows[0].action, static_cast<int>(mc::OperatorAction::Start));
        EXPECT_EQ(rows[1].role, static_cast<int>(mc::OperatorRole::Viewer));
    }
}
