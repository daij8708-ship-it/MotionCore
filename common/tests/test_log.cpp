#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "mc/log.hpp"

// 运行日志的分级、环形与滚动（P1-8）。
// 这里刻意**只测纯 C++ 的那一层**：分级判定、丢最旧、轮转与"打不开要明确失败"。
// 至于"哪个事件该记成哪一级"住在宿主侧，由那边自己的用例守。
namespace {

// 每条用例一个独立目录，跑完删掉 —— 免得互相看见对方的文件，
// 也免得在一台机器上留下垃圾。
class TempDir {
public:
    explicit TempDir(const std::string& name) {
        path_ = std::filesystem::temp_directory_path() / ("mc_log_test_" + name);
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    std::string file(const std::string& name) const { return (path_ / name).string(); }
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

std::string readAll(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::size_t countLines(const std::string& text) {
    std::size_t lines = 0U;
    for (const char c : text) {
        if (c == '\n') {
            ++lines;
        }
    }
    return lines;
}

// 固定时基：每写一条往前挪 1 ms。日志的时间戳于是可预测，断言不用跟机器时钟赛跑。
std::uint64_t fixedTick = 0U;
std::uint64_t tickClock() {
    fixedTick += 1U;
    return 1'700'000'000'000ULL + fixedTick;
}

}  // namespace

// ---------------------------------------------------------------------------
// 级别 token
// ---------------------------------------------------------------------------

TEST(LogLevel, TokensRoundTripAndAreCaseInsensitive) {
    for (std::size_t i = 0U; i < mc::kLogLevelCount; ++i) {
        const auto level = static_cast<mc::LogLevel>(i);
        EXPECT_TRUE(mc::isLogLevelToken(mc::logLevelName(level)));
        EXPECT_EQ(mc::logLevelFromToken(mc::logLevelName(level)), level);
    }
    // 配置文件里写小写也该认 —— 它不是接口，是给人敲的。
    EXPECT_EQ(mc::logLevelFromToken("warn"), mc::LogLevel::Warn);
    EXPECT_EQ(mc::logLevelFromToken("Error"), mc::LogLevel::Error);
}

// 认不出的级别名 → Info，**不是"全都不记"**：写错一个名字就静默丢掉全部日志，
// 那种故障看起来就像"日志功能坏了"。
TEST(LogLevel, AnUnknownTokenFallsBackToInfoRatherThanSilence) {
    EXPECT_FALSE(mc::isLogLevelToken("verbose"));
    EXPECT_FALSE(mc::isLogLevelToken(""));
    EXPECT_EQ(mc::logLevelFromToken("verbose"), mc::LogLevel::Info);
}

// 级别名与中文措辞都必须非空且互不相同（两级长得一样，人就看不出差别了）。
TEST(LogLevel, EveryLevelHasItsOwnNameAndText) {
    for (std::size_t i = 0U; i < mc::kLogLevelCount; ++i) {
        const auto a = static_cast<mc::LogLevel>(i);
        EXPECT_STRNE(mc::logLevelName(a), "");
        EXPECT_STRNE(mc::logLevelText(a), "");
        for (std::size_t j = i + 1U; j < mc::kLogLevelCount; ++j) {
            const auto b = static_cast<mc::LogLevel>(j);
            EXPECT_STRNE(mc::logLevelName(a), mc::logLevelName(b));
            EXPECT_STRNE(mc::logLevelText(a), mc::logLevelText(b));
        }
    }
}

TEST(LogLine, CarriesTheLevelTagAndText) {
    mc::LogRecord record;
    record.timestampMs = 1'700'000'000'000ULL;
    record.level = mc::LogLevel::Warn;
    record.tag = "cmd";
    record.text = "SET_TARGET 被拒：当前设备状态不允许该命令";
    const std::string line = mc::formatLogLine(record);
    EXPECT_NE(line.find("WARN"), std::string::npos);
    EXPECT_NE(line.find("[cmd]"), std::string::npos);
    EXPECT_NE(line.find("SET_TARGET"), std::string::npos);
}

// ---------------------------------------------------------------------------
// 有界环形
// ---------------------------------------------------------------------------

// 满了丢**最旧**的，并把丢掉的**计数**留下 ——
// 界面上只显示"最后 N 条"而不说丢了多少，会让人以为那就是全部。
TEST(LogRing, DropsTheOldestAndCountsWhatItDropped) {
    mc::LogRing ring(3U);
    for (int i = 0; i < 5; ++i) {
        mc::LogRecord record;
        record.level = mc::LogLevel::Info;
        record.text = "第 " + std::to_string(i) + " 条";
        ring.push(record);
    }
    ASSERT_EQ(ring.records().size(), 3U);
    EXPECT_EQ(ring.dropped(), 2U);
    EXPECT_EQ(ring.records().front().text, "第 2 条") << "留下的应是最新的三条";
    EXPECT_EQ(ring.records().back().text, "第 4 条");
}

// 容量 0 不该变成"一条都存不下还每写一条崩一次"——夹到 1，保持可用。
TEST(LogRing, AZeroCapacityIsClampedInsteadOfDiscardingEverything) {
    mc::LogRing ring(0U);
    mc::LogRecord record;
    record.text = "唯一一条";
    ring.push(record);
    EXPECT_EQ(ring.records().size(), 1U);
}

// 清屏只清"现在还剩几条"，**不清"一共丢过多少"**：
// 后者是证据，点一下按钮就把它抹掉正是要避免的。
TEST(LogRing, ClearingKeepsTheDroppedAccount) {
    mc::LogRing ring(2U);
    for (int i = 0; i < 4; ++i) {
        mc::LogRecord record;
        record.text = "x";
        ring.push(record);
    }
    ASSERT_EQ(ring.dropped(), 2U);
    ring.clear();
    EXPECT_TRUE(ring.records().empty());
    EXPECT_EQ(ring.dropped(), 2U) << "清屏不该把丢弃计数一起抹掉";
}

// ---------------------------------------------------------------------------
// 滚动文件
// ---------------------------------------------------------------------------

TEST(RotatingFile, WritesLinesThenRotatesAndKeepsTheConfiguredCount) {
    const TempDir dir("rotate");
    const std::string path = dir.file("app.log");
    mc::RotatingFileWriter writer;
    ASSERT_TRUE(writer.open(path, 200U, 3U)) << writer.lastError();

    // 每行约 30 字节，写 30 行 ≈ 900 字节 ⇒ 至少轮转 3 次
    for (int i = 0; i < 30; ++i) {
        ASSERT_TRUE(writer.writeLine("一行有点长度的日志内容 序号 " + std::to_string(i)))
            << writer.lastError();
    }
    EXPECT_GT(writer.rotations(), 0U) << "超过上限却没有轮转";
    EXPECT_LT(writer.currentBytes(), 200U) << "轮转之后当前文件必须重新变小";
    EXPECT_TRUE(std::filesystem::exists(path));
    EXPECT_TRUE(std::filesystem::exists(writer.rotatedPath(1U))) << "第一份轮转文件必须存在";
    // 保留 3 份 ⇒ 只有 .1 和 .2，不该出现 .3
    EXPECT_STRNE(writer.rotatedPath(2U).c_str(), "");
    EXPECT_STREQ(writer.rotatedPath(3U).c_str(), "") << "keepFiles=3 时不该有第 3 份";

    // 真正的契约：**最老的被丢掉、最新的还在**。
    // 断"当前文件非空"是错的 —— 最后一次写入刚好触发轮转时，当前文件本来就是空的
    // （刚填满的那一份已经改名成 .1 了）。
    std::string all = readAll(path);
    for (std::size_t n = 1U; n < 3U; ++n) {
        all += readAll(writer.rotatedPath(n));
    }
    EXPECT_GT(countLines(all), 0U);
    EXPECT_EQ(all.find("序号 0"), std::string::npos) << "最老的那些行该被轮转丢掉";
    EXPECT_NE(all.find("序号 29"), std::string::npos) << "最新写入的那一行必须还在某一份文件里";

    writer.close();
}

// 保留 1 份（= 只留当前文件）：**不轮转、不报错**，超了就原地重开。
TEST(RotatingFile, KeepingASingleFileTruncatesInsteadOfRotating) {
    const TempDir dir("single");
    const std::string path = dir.file("app.log");
    mc::RotatingFileWriter writer;
    ASSERT_TRUE(writer.open(path, 120U, 1U)) << writer.lastError();
    for (int i = 0; i < 20; ++i) {
        ASSERT_TRUE(writer.writeLine("内容 序号 " + std::to_string(i)));
    }
    EXPECT_GT(writer.rotations(), 0U);
    EXPECT_LT(readAll(path).size(), 400U) << "单份模式下文件不该无限增长";
    EXPECT_STREQ(writer.rotatedPath(1U).c_str(), "");
    writer.close();
}

// 日志文件**追加**而不是截断：重启之后上一次出事时的最后几行还在 ——
// 日志的价值恰恰集中在"崩溃前那一刻"。若改成截断，这条用例会变红。
TEST(RotatingFile, ReopeningAppendsInsteadOfLosingThePreviousRun) {
    const TempDir dir("append");
    const std::string path = dir.file("app.log");
    {
        mc::RotatingFileWriter writer;
        ASSERT_TRUE(writer.open(path, 1'000'000U, 2U)) << writer.lastError();
        ASSERT_TRUE(writer.writeLine("上一次运行留下的最后一行"));
        writer.close();
    }
    const std::uint64_t before = std::filesystem::file_size(path);
    mc::RotatingFileWriter again;
    ASSERT_TRUE(again.open(path, 1'000'000U, 2U)) << again.lastError();
    EXPECT_EQ(again.currentBytes(), before) << "重开之后必须接着已有大小算，而不是从头算";
    ASSERT_TRUE(again.writeLine("这一次运行的第一行"));
    const std::string all = readAll(path);
    EXPECT_NE(all.find("上一次运行留下的最后一行"), std::string::npos);
    EXPECT_NE(all.find("这一次运行的第一行"), std::string::npos);
    again.close();
}

// 打不开必须**明确失败**且此后仍然安全可调：磁盘满 / 目录没权限时，
// 采集不该停摆（与历史库那条姿态一致）。
TEST(RotatingFile, AFailureToOpenIsReportedAndStaysSafeToUse) {
    mc::RotatingFileWriter writer;
    const std::string bogus =
        (std::filesystem::temp_directory_path() / "mc_log_no_such_dir").string() + "/app.log";
    EXPECT_FALSE(writer.open(bogus, 1024U, 3U));
    EXPECT_FALSE(writer.isOpen());
    EXPECT_STRNE(writer.lastError().c_str(), "");
    EXPECT_FALSE(writer.writeLine("这一行写不出去"));
    EXPECT_EQ(writer.rotations(), 0U);
    writer.close();   // 幂等，不该崩
}

// ---------------------------------------------------------------------------
// Logger：级别过滤 + 双路输出
// ---------------------------------------------------------------------------

// 过滤发生在**最前面**：挡掉的记录不进环形、也不写文件。
// 两个计数器分别回答"记了多少"与"挡了多少"。
TEST(Logger, FiltersBelowTheMinimumLevelAndCountsBothSides) {
    mc::Logger logger(64U, &tickClock);
    logger.setMinLevel(mc::LogLevel::Warn);

    logger.trace("t", "不该出现");
    logger.debug("t", "不该出现");
    logger.info("t", "不该出现");
    EXPECT_TRUE(logger.ring().records().empty());
    EXPECT_EQ(logger.totalFiltered(), 3U);
    EXPECT_EQ(logger.totalWritten(), 0U);

    logger.warn("t", "这一条要");
    logger.error("t", "这一条也要");
    EXPECT_EQ(logger.ring().records().size(), 2U);
    EXPECT_EQ(logger.totalWritten(), 2U);
    EXPECT_EQ(logger.totalFiltered(), 3U) << "挡掉的计数不该被写出去的条数冲掉";
    EXPECT_EQ(logger.ring().records().front().level, mc::LogLevel::Warn);
}

// 抬高级别之后，"会不会记"这件事必须当场就能问出来 ——
// 免得调用方先拼好一大串字符串再被丢掉。
TEST(Logger, WouldLogAnswersBeforeTheStringIsBuilt) {
    mc::Logger logger(16U, &tickClock);
    logger.setMinLevel(mc::LogLevel::Info);
    EXPECT_FALSE(logger.wouldLog(mc::LogLevel::Debug));
    EXPECT_TRUE(logger.wouldLog(mc::LogLevel::Info));
    EXPECT_TRUE(logger.wouldLog(mc::LogLevel::Error));
    logger.setMinLevel(mc::LogLevel::Trace);
    EXPECT_TRUE(logger.wouldLog(mc::LogLevel::Trace));
}

// 文件这一路只拿到**过了级别**的那些 —— 与环形一致。
// 若实现成"先写文件再过滤"，Debug/Trace 的洪水会照样灌进磁盘。
TEST(Logger, TheFileOnlySeesWhatSurvivedTheLevelFilter) {
    const TempDir dir("logger");
    const std::string path = dir.file("run.log");
    mc::RotatingFileWriter writer;
    ASSERT_TRUE(writer.open(path, 1'000'000U, 2U)) << writer.lastError();

    mc::Logger logger(64U, &tickClock);
    logger.setMinLevel(mc::LogLevel::Info);
    logger.attachFile(&writer);
    logger.debug("cmd", "调试细节：不该进文件");
    logger.info("link", "已连接");
    logger.error("alarm", "故障锁存");

    const std::string all = readAll(path);
    EXPECT_EQ(all.find("不该进文件"), std::string::npos) << "被级别挡掉的记录不该写进文件";
    EXPECT_NE(all.find("已连接"), std::string::npos);
    EXPECT_NE(all.find("故障锁存"), std::string::npos);
    EXPECT_EQ(logger.fileWriteFailures(), 0U);
    EXPECT_EQ(countLines(all), 2U) << "文件里应恰好是过了级别的那两条";
    writer.close();
}

// 文件写不进去要**被计数**，而不是静默 —— "以为记下来了、其实没有"
// 是最坏的一种日志故障。
TEST(Logger, FileWriteFailuresAreCountedNotSwallowed) {
    mc::Logger logger(16U, &tickClock);
    logger.setMinLevel(mc::LogLevel::Info);
    mc::RotatingFileWriter writer;   // 从没打开过
    logger.attachFile(&writer);
    logger.info("t", "这一条写不进去");
    EXPECT_EQ(logger.fileWriteFailures(), 1U);
    EXPECT_EQ(logger.ring().records().size(), 1U) << "写文件失败不该连带丢掉环形里的那条";
}

// 没挂文件时也要能正常工作：环形这一路是底线能力。
TEST(Logger, WorksWithoutAFileAttached) {
    mc::Logger logger(8U, &tickClock);
    logger.info("t", "只有环形");
    EXPECT_EQ(logger.ring().records().size(), 1U);
    EXPECT_EQ(logger.fileWriteFailures(), 0U);
    EXPECT_EQ(logger.file(), nullptr);
}
