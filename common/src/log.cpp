#include "mc/log.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>

namespace mc {
namespace {

constexpr const char* kLevelTokens[kLogLevelCount] = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR"};
constexpr const char* kLevelNames[kLogLevelCount] = {"跟踪", "调试", "信息", "警告", "错误"};

char asciiUpper(char c) noexcept {
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - ('a' - 'A')) : c;
}

bool sameToken(const char* a, const char* b) noexcept {
    if (a == nullptr || b == nullptr) {
        return false;
    }
    std::size_t i = 0U;
    for (; a[i] != '\0' && b[i] != '\0'; ++i) {
        if (asciiUpper(a[i]) != asciiUpper(b[i])) {
            return false;
        }
    }
    return a[i] == '\0' && b[i] == '\0';
}

std::size_t levelIndex(LogLevel level) noexcept {
    const auto raw = static_cast<std::size_t>(level);
    return (raw < kLogLevelCount) ? raw : static_cast<std::size_t>(LogLevel::Info);
}

std::uint64_t defaultClock() noexcept {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

}  // namespace

const char* logLevelName(LogLevel level) noexcept { return kLevelTokens[levelIndex(level)]; }

const char* logLevelText(LogLevel level) noexcept { return kLevelNames[levelIndex(level)]; }

bool isLogLevelToken(const char* token) noexcept {
    if (token == nullptr) {
        return false;
    }
    for (std::size_t i = 0U; i < kLogLevelCount; ++i) {
        if (sameToken(token, kLevelTokens[i])) {
            return true;
        }
    }
    return false;
}

LogLevel logLevelFromToken(const char* token) noexcept {
    for (std::size_t i = 0U; i < kLogLevelCount; ++i) {
        if (sameToken(token, kLevelTokens[i])) {
            return static_cast<LogLevel>(i);
        }
    }
    // 认不出就给 Info：配置文件里写错一个级别名不该让程序起不来，
    // 但也不能悄悄变成"全都不记"（那看起来像日志坏了）。
    return LogLevel::Info;
}

std::string formatLogLine(const LogRecord& record) {
    const std::uint64_t ms = record.timestampMs;
    const std::time_t secs = static_cast<std::time_t>(ms / 1000U);
    const std::uint64_t millis = ms % 1000U;
    std::tm tm{};
#if defined(_WIN32)
    (void)localtime_s(&tm, &secs);
#else
    (void)localtime_r(&secs, &tm);
#endif
    char stamp[32] = {};
    std::snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d %02d:%02d:%02d.%03llu",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<unsigned long long>(millis));

    std::string line;
    line.reserve(record.text.size() + record.tag.size() + 48U);
    line += '[';
    line += stamp;
    line += "] [";
    line += logLevelName(record.level);
    line += "] [";
    line += record.tag;
    line += "] ";
    line += record.text;
    return line;
}

// ---------------------------------------------------------------------------
// LogRing
// ---------------------------------------------------------------------------

LogRing::LogRing(std::size_t capacity)
    : capacity_((capacity == 0U) ? 1U : capacity) {}

void LogRing::push(const LogRecord& record) {
    if (records_.size() >= capacity_) {
        records_.pop_front();
        ++dropped_;
    }
    records_.push_back(record);
}

void LogRing::clear() {
    records_.clear();
    // **不清 dropped_**：那是"一共丢过多少"的累计账，
    // 与"现在还剩几条"是两件事；点一下清屏就把证据抹掉正是要避免的。
}

// ---------------------------------------------------------------------------
// RotatingFileWriter
// ---------------------------------------------------------------------------

RotatingFileWriter::~RotatingFileWriter() { close(); }

std::string RotatingFileWriter::rotatedPath(std::size_t n) const {
    if (n == 0U || n >= keepFiles_) {
        return std::string();
    }
    return path_ + "." + std::to_string(n);
}

bool RotatingFileWriter::open(const std::string& path, std::uint64_t maxBytes,
                              std::size_t keepFiles) {
    close();
    path_ = path;
    maxBytes_ = maxBytes;
    keepFiles_ = (keepFiles == 0U) ? 1U : keepFiles;
    currentBytes_ = 0U;
    rotations_ = 0U;
    lastError_.clear();

    // 追加而不是截断：重启之后上一次出事时的最后几行还在 ——
    // 日志的价值恰恰集中在"崩溃前那一刻"。
    file_ = std::fopen(path_.c_str(), "ab");
    if (file_ == nullptr) {
        lastError_ = "打不开日志文件";
        return false;
    }
    if (std::fseek(file_, 0L, SEEK_END) == 0) {
        const long size = std::ftell(file_);
        if (size > 0L) {
            currentBytes_ = static_cast<std::uint64_t>(size);
        }
    }
    return true;
}

void RotatingFileWriter::close() {
    if (file_ != nullptr) {
        std::fclose(file_);
        file_ = nullptr;
    }
}

void RotatingFileWriter::rotate() {
    if (file_ != nullptr) {
        std::fclose(file_);
        file_ = nullptr;
    }
    if (keepFiles_ > 1U) {
        // 最老的那份直接删掉（保留 N 份 = 允许占 N 个文件）
        const std::string oldest = rotatedPath(keepFiles_ - 1U);
        if (!oldest.empty()) {
            std::remove(oldest.c_str());
        }
        // 从大到小依次后移：先挪大的，才不会被覆盖
        for (std::size_t n = keepFiles_ - 1U; n >= 2U; --n) {
            const std::string from = rotatedPath(n - 1U);
            const std::string to = rotatedPath(n);
            if (!from.empty() && !to.empty()) {
                std::rename(from.c_str(), to.c_str());
            }
        }
        const std::string first = rotatedPath(1U);
        if (!first.empty()) {
            std::rename(path_.c_str(), first.c_str());
        }
    }
    ++rotations_;
    file_ = std::fopen(path_.c_str(), "wb");
    currentBytes_ = 0U;
    if (file_ == nullptr) {
        lastError_ = "轮转后重开日志文件失败";
    }
}

bool RotatingFileWriter::writeLine(const std::string& line) {
    if (file_ == nullptr) {
        lastError_ = "日志文件没有打开";
        return false;
    }
    const std::string withNewline = line + "\n";
    const std::size_t written =
        std::fwrite(withNewline.data(), 1U, withNewline.size(), file_);
    if (written != withNewline.size()) {
        lastError_ = "写日志文件失败";
        return false;
    }
    // 每条都刷一下。慢，但日志要的正是"进程没了之后还能看到最后几行" ——
    // 靠缓冲区攒着的日志在崩溃那一刻恰好全丢。
    std::fflush(file_);
    currentBytes_ += static_cast<std::uint64_t>(withNewline.size());
    if (maxBytes_ > 0U && currentBytes_ >= maxBytes_) {
        rotate();
        if (file_ == nullptr) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Logger
// ---------------------------------------------------------------------------

Logger::Logger(std::size_t ringCapacity, Clock clock)
    : ring_(ringCapacity),
      clock_(clock ? std::move(clock) : Clock(&defaultClock)) {}

bool Logger::wouldLog(LogLevel level) const noexcept {
    return levelIndex(level) >= levelIndex(minLevel_);
}

void Logger::log(LogLevel level, const std::string& tag, const std::string& text) {
    if (!wouldLog(level)) {
        ++totalFiltered_;
        return;
    }
    LogRecord record;
    record.timestampMs = clock_();
    record.level = level;
    record.tag = tag;
    record.text = text;
    ring_.push(record);
    ++totalWritten_;
    // **只要有写入器就去写**，写不成一律计数 —— 包括"挂了写入器但从没打开成功"。
    // 那种情况是配置错了（路径没权限 / 目录不存在），必须看得见：
    // 悄悄跳过会让"我以为在记文件、其实一条都没有"一直持续到出事那天。
    if (file_ != nullptr) {
        if (!file_->writeLine(formatLogLine(record))) {
            ++fileWriteFailures_;
        }
    }
}

}  // namespace mc
