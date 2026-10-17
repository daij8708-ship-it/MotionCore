#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>

namespace mc {

// ---------------------------------------------------------------------------
// 运行日志（P1-8）：分级 + 有界环形 + 滚动文件
//
// 与**报警**、**审计**的分工 —— 三者都在记"发生了什么"，但不是一回事：
//   · 报警：设备出了**需要人处理**的事。有级别、要确认、会拦启动。
//   · 审计：人做了 / 试了什么。是追责与合规用的流水，**被拒的也要记**。
//   · 日志：程序自己走过哪些路。最多、最细、**可以随时丢**。
//
// 把三者合成一条流，后果很具体：每拍一条的排查信息会把"必须处理的那一条"
// 淹掉 —— 而人恰恰是在出事的时候才去看日志的。
//
// 这份实现刻意**不引入第三方库**（spdlog 之类），理由与自绘曲线一致：
// 需要的只有"分级 + 过滤 + 环形 + 轮转"，一共两百行以内；
// 引一个库换来的是构建依赖、版本漂移与一套用不到的特性。
// ---------------------------------------------------------------------------

enum class LogLevel : std::uint8_t {
    Trace = 0U,   // 每拍级别的细节（默认不记：它本身就是噪声）
    Debug = 1U,   // 状态迁移、命令收发
    Info = 2U,    // 连接、握手、配方 / 任务推进
    Warn = 3U,    // 被拒、超时、链路失效 —— "不正常，但程序还能继续"
    Error = 4U,   // 故障锁存、落盘失败 —— "需要人去看一眼"
};

inline constexpr std::size_t kLogLevelCount = 5U;

// 线上 / 文件里用的稳定 token（英文、全大写）。日志文件是要被别的工具读的，
// 中文级别名到了 grep/awk 那一层就不好使了 —— 给人看的中文另有一处（log_text）。
const char* logLevelName(LogLevel level) noexcept;
// 给人看的中文级别名（界面用）。**日志文件里一律用 token**，见上。
const char* logLevelText(LogLevel level) noexcept;
// 认不出 → Info（而不是抛 / 崩溃）：配置文件里写错一个级别名，
// 不该让整个程序起不来。
LogLevel logLevelFromToken(const char* token) noexcept;
bool isLogLevelToken(const char* token) noexcept;

struct LogRecord {
    std::uint64_t timestampMs = 0U;
    LogLevel level = LogLevel::Info;
    std::string tag;    // 分类（"link" / "cmd" / "alarm" …），便于按来源筛
    std::string text;   // 一句话
};

// 一行日志的文本形式（含级别与分类）。时间戳按本地时间格式化。
std::string formatLogLine(const LogRecord& record);

// ---------------------------------------------------------------------------
// 有界环形：**留着给界面看**的那一份。
//
// 满了丢**最旧**的 —— 日志不是账本，丢旧的正是不想要的那一头。
// 丢弃量单独计数并显示出来：界面上只看到"最后 512 条"而不说丢了多少，
// 会让人以为这就是全部。
// ---------------------------------------------------------------------------
class LogRing {
public:
    static constexpr std::size_t kDefaultCapacity = 512U;

    explicit LogRing(std::size_t capacity = kDefaultCapacity);

    void push(const LogRecord& record);

    const std::deque<LogRecord>& records() const noexcept { return records_; }
    std::size_t capacity() const noexcept { return capacity_; }
    std::size_t dropped() const noexcept { return dropped_; }
    void clear();

private:
    std::deque<LogRecord> records_;
    std::size_t capacity_;
    std::size_t dropped_ = 0U;
};

// ---------------------------------------------------------------------------
// 滚动文件：写到 path，超过 maxBytes 就轮转，只保留 keepFiles 份。
//
// 轮转规则（与常见做法一致，也好解释）：
//   path.(keepFiles-1) 被删掉 → path.(n) 改名成 path.(n+1)（从大到小）→ path 改名成 path.1
//   然后重新开一个空的 path。于是**最新的永远在 path**，越老后缀越大。
//
// 打不开时**明确失败**（返回 false，原因在 lastError()）并保持可继续调用：
// "磁盘满了 / 目录没权限"不该让采集停摆 —— 这与历史库那条姿态一致。
// 本类不做线程安全：整个上位机是单线程的（见 PERF.md 的裁定）。
// ---------------------------------------------------------------------------
class RotatingFileWriter {
public:
    RotatingFileWriter() = default;
    ~RotatingFileWriter();

    RotatingFileWriter(const RotatingFileWriter&) = delete;
    RotatingFileWriter& operator=(const RotatingFileWriter&) = delete;

    bool open(const std::string& path, std::uint64_t maxBytes, std::size_t keepFiles);
    bool isOpen() const noexcept { return file_ != nullptr; }
    void close();

    // 写一行（自动补换行）。返回 false = 这次没写进去。
    bool writeLine(const std::string& line);

    const std::string& path() const noexcept { return path_; }
    std::uint64_t maxBytes() const noexcept { return maxBytes_; }
    std::size_t keepFiles() const noexcept { return keepFiles_; }
    // 当前文件已经写了多少字节（界面要显示"离轮转还有多远"）。
    std::uint64_t currentBytes() const noexcept { return currentBytes_; }
    // 已经发生过几次轮转（累计，不是"现存几份"）。
    std::uint64_t rotations() const noexcept { return rotations_; }
    const std::string& lastError() const noexcept { return lastError_; }
    // 轮转出来的第 n 份文件的路径（n 从 1 开始）。越界返回空。
    std::string rotatedPath(std::size_t n) const;

private:
    void rotate();

    // 用 C 的 FILE* 而不是 std::ofstream：轮转要 rename/remove，
    // 本来就在 <cstdio> 的地盘上；混两套 I/O 只会多一处状态要同步。
    std::FILE* file_ = nullptr;
    std::string path_;
    std::uint64_t maxBytes_ = 0U;
    std::size_t keepFiles_ = 1U;
    std::uint64_t currentBytes_ = 0U;
    std::uint64_t rotations_ = 0U;
    std::string lastError_;
};

// ---------------------------------------------------------------------------
// Logger：级别过滤 → 双路输出（环形给界面 + 文件给人查）。
//
// **过滤发生在最前面**：被级别挡掉的记录不进环形、也不格式化 ——
// 否则"记了又不看"的那部分仍然要花每人一条的格式化和内存。
// 两个计数器（写出去几条 / 被级别挡掉几条）都留着：
// "日志怎么这么少"与"日志怎么这么多"都要能当场回答。
// ---------------------------------------------------------------------------
class Logger {
public:
    // 时钟可注入：测试要确定性，"写出去的每一条都带同一个时刻"比读机器时钟好断言。
    using Clock = std::function<std::uint64_t()>;

    explicit Logger(std::size_t ringCapacity = LogRing::kDefaultCapacity, Clock clock = Clock());

    void setMinLevel(LogLevel level) noexcept { minLevel_ = level; }
    LogLevel minLevel() const noexcept { return minLevel_; }
    bool wouldLog(LogLevel level) const noexcept;

    void log(LogLevel level, const std::string& tag, const std::string& text);
    // 便捷形式：级别固定、tag 固定。省掉每次调用都拼一遍字符串。
    void trace(const std::string& tag, const std::string& text) { log(LogLevel::Trace, tag, text); }
    void debug(const std::string& tag, const std::string& text) { log(LogLevel::Debug, tag, text); }
    void info(const std::string& tag, const std::string& text) { log(LogLevel::Info, tag, text); }
    void warn(const std::string& tag, const std::string& text) { log(LogLevel::Warn, tag, text); }
    void error(const std::string& tag, const std::string& text) { log(LogLevel::Error, tag, text); }

    // 挂一个文件写入器（**不持有**，生命周期由调用方管）。传 nullptr = 只留环形。
    void attachFile(RotatingFileWriter* writer) noexcept { file_ = writer; }
    RotatingFileWriter* file() const noexcept { return file_; }

    LogRing& ring() noexcept { return ring_; }
    const LogRing& ring() const noexcept { return ring_; }

    std::uint64_t totalWritten() const noexcept { return totalWritten_; }
    std::uint64_t totalFiltered() const noexcept { return totalFiltered_; }
    // 文件写入失败的次数。**不忽略**：它意味着"以为记下来了、其实没有"。
    std::uint64_t fileWriteFailures() const noexcept { return fileWriteFailures_; }

private:
    LogRing ring_;
    Clock clock_;
    RotatingFileWriter* file_ = nullptr;
    LogLevel minLevel_ = LogLevel::Info;
    std::uint64_t totalWritten_ = 0U;
    std::uint64_t totalFiltered_ = 0U;
    std::uint64_t fileWriteFailures_ = 0U;
};

}  // namespace mc
