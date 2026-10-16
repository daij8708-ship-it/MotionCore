// ---------------------------------------------------------------------------
// mc_host_bench —— 上位机单线程负载实测
//
// 它不是演示，是一把尺子，只回答一个问题：
//   **在给定遥测率下，UI 线程每拍被占住多久、定时器还准不准。**
//
// 为什么必须先有它：任何"要不要开线程 / 该优化哪一段"的结论都得先有基线。
// 没有基线的优化前后对比，两栏数字都是空的。
//
// 两档负载（设备侧的产帧周期不同，其余完全一样）：
//   baseline  设备 100 Hz 产帧（与 kTickIntervalMs 一致，每拍一帧）
//   stress    设备 1 kHz 产帧（上位机每拍要收 10 帧、解 10 帧）
// 外加两项单独测量：
//   界面刷新（取窗 + 抽稀 + 更新控件数据）
//   整页绘制（grab()）
//
// 运行：
//   ./mc_host_bench --seconds=5              两档各跑 5 s
//   ./mc_host_bench --seconds=45             长跑，把环形缓冲填满（曲线最坏情况）
//   QT_QPA_PLATFORM=xcb ./mc_host_bench      想在真显示下测就自己设平台
//
// 两点必须说清、否则数字会被误读：
//   ① 绘制是事件循环投递的**独立事件**，不在 onTick 回调之内 ——
//      所以"每拍处理耗时"里不含绘制成本，绘制成本由 grab() 那一项单独给出。
//      （离屏平台**会**投递绘制事件，只是只投给当前可见页；本机 5 s 实测约 180 次。）
//   ② 上位机单拍最多从链路读 512 B，一帧遥测 30 B，即单拍上限约 17 帧。
//      遥测再快也不会让单拍读得更多，只会让读不完的部分留在设备侧队列里。
// ---------------------------------------------------------------------------
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QString>
#include <QStringList>
#include <QTimer>

#include "mc/device_simulator.hpp"
#include "mc/link_monitor.hpp"
#include "mc_host/device.hpp"
#include "mc_host/device_manager.hpp"
#include "mc_host/ui/curve_page.hpp"
#include "mc_host/ui/curve_plot.hpp"
#include "mc_host/ui/main_window.hpp"

namespace {

constexpr std::size_t kTimingSampleCapacity = 8192U;
constexpr int kRenderWarmups = 3;
constexpr int kRenderSamples = 20;
constexpr int kCurveRefreshSamples = 50;

struct Options {
    int seconds = 5;
};

Options parseOptions(const QStringList& args) {
    Options options;
    for (const QString& arg : args) {
        if (arg.startsWith(QStringLiteral("--seconds="))) {
            const int value = arg.section('=', 1).toInt();
            if (value > 0) {
                options.seconds = value;
            }
        }
    }
    return options;
}

struct Summary {
    std::size_t n = 0U;
    double mean = 0.0;
    long long p50 = 0;
    long long p95 = 0;
    long long p99 = 0;
    long long max = 0;
};

// 最近秩百分位。样本少时它自然退到最大值 —— 宁可报大，也不凭空造一个更小的数。
long long percentile(const std::vector<long long>& sorted, double p) {
    if (sorted.empty()) {
        return 0;
    }
    double rank = (p / 100.0) * static_cast<double>(sorted.size());
    rank = std::ceil(rank);
    const long long index = (rank <= 1.0) ? 0LL : static_cast<long long>(rank) - 1LL;
    const long long clamped = std::min<long long>(index, static_cast<long long>(sorted.size()) - 1LL);
    return sorted[static_cast<std::size_t>(clamped)];
}

Summary summarize(std::vector<long long> values) {
    Summary summary;
    if (values.empty()) {
        return summary;
    }
    std::sort(values.begin(), values.end());
    double sum = 0.0;
    for (const long long value : values) {
        sum += static_cast<double>(value);
    }
    summary.n = values.size();
    summary.mean = sum / static_cast<double>(values.size());
    summary.p50 = percentile(values, 50.0);
    summary.p95 = percentile(values, 95.0);
    summary.p99 = percentile(values, 99.0);
    summary.max = values.back();
    return summary;
}

void printSummary(const char* label, const Summary& summary, const char* unit) {
    std::printf("  %s：P50 %7lld %s | P95 %7lld %s | P99 %7lld %s | 最大 %8lld %s | 均值 %9.1f %s"
                "（%zu 个样本）\n",
                label, summary.p50, unit, summary.p95, unit, summary.p99, unit, summary.max, unit,
                summary.mean, unit, summary.n);
}

struct PhaseReport {
    QString label;
    std::uint32_t telemetryPeriodMs = 0U;
    double wallSeconds = 0.0;
    std::uint64_t ticks = 0U;
    std::uint64_t refreshEmits = 0U;
    std::uint64_t telemetryAccepted = 0U;
    std::uint64_t telemetryRejected = 0U;
    std::uint64_t framesDecoded = 0U;
    std::uint64_t crcErrors = 0U;
    std::uint64_t badLengths = 0U;
    std::uint64_t bytesDiscarded = 0U;
    std::uint64_t ringSize = 0U;
    std::uint64_t ringDiscarded = 0U;
    std::uint64_t deviceBacklogDropped = 0U;
    std::uint64_t plotRepaints = 0U;
    std::uint64_t ackTimeoutsFaulted = 0U;
    Summary tickBusy;
    Summary tickInterval;
    Summary curveRefresh;
    Summary render;
};

PhaseReport runPhase(const QString& label, std::uint32_t telemetryPeriodMs, int seconds) {
    PhaseReport report;
    report.label = label;
    report.telemetryPeriodMs = telemetryPeriodMs;

    mc::SimulatorConfig simConfig;
    simConfig.telemetryPeriodMs = telemetryPeriodMs;

    mchost::DeviceManager manager;
    auto sim = std::make_unique<mchost::SimulatedDevice>(simConfig, mc::LinkMonitor::Config{});
    mchost::SimulatedDevice* simRaw = sim.get();
    manager.setDevice(std::move(sim));
    manager.enableTimingDiagnostics(true, kTimingSampleCapacity);

    mchost::MainWindow window(manager);
    window.show();
    (void)window.showTab(2);
    QApplication::processEvents();

    // 时间窗选最宽的 30 s：抽稀要处理的样本最多，这就是最坏情况。
    if (auto* combo = window.findChild<QComboBox*>(QStringLiteral("windowCombo")); combo != nullptr) {
        combo->setCurrentIndex(3);
    }
    mchost::CurvePage* curvePage = window.findChild<mchost::CurvePage*>();
    mchost::CurvePlot* plot = window.findChild<mchost::CurvePlot*>(QStringLiteral("curvePlot"));

    std::uint64_t refreshEmits = 0U;
    QObject::connect(&manager, &mchost::DeviceManager::statsRefreshed, &manager,
                     [&refreshEmits]() { ++refreshEmits; });

    QElapsedTimer wall;
    QTimer::singleShot(200, &manager, [&manager, &wall]() {
        wall.start();
        manager.connectDevice();
    });
    QTimer::singleShot(500, &manager, [&manager]() { manager.sendSetMode(1); });
    QTimer::singleShot(700, &manager, [&manager]() { manager.sendStart(); });
    QTimer::singleShot(900, &manager, [&manager]() { manager.sendSetTarget(200.0); });
    QTimer::singleShot(seconds * 1000, &manager, []() { QApplication::quit(); });
    QApplication::exec();

    report.wallSeconds = static_cast<double>(wall.elapsed()) / 1000.0;
    report.ticks = manager.ticks();
    report.refreshEmits = refreshEmits;

    const mchost::IDevice* device = manager.device();
    report.telemetryAccepted = device->telemetryAcceptedTotal();
    report.telemetryRejected = device->telemetryRejectedTotal();
    const mc::StreamDecoder::Stats& decode = device->decoderStats();
    report.framesDecoded = decode.framesDecoded;
    report.crcErrors = decode.framesCrcError;
    report.badLengths = decode.framesBadLength;
    report.bytesDiscarded = decode.bytesDiscarded;
    report.ackTimeoutsFaulted = device->ackStats().timedOutFaulted;
    report.ringSize = manager.telemetry().size();
    report.ringDiscarded = manager.telemetry().discarded();
    report.deviceBacklogDropped = simRaw->telemetryBacklogDropped();
    report.plotRepaints = (plot != nullptr) ? plot->repaintCount() : 0U;

    // 样本按写入顺序环形存放；没有绕圈时前 valid 个就是全部样本，
    // 绕圈之后整个缓冲都是有效样本 —— 两种情况都取前 valid 个。
    std::vector<long long> busyUs;
    std::vector<long long> intervalUs;
    const std::size_t valid = manager.timingSampleCount();
    busyUs.reserve(valid);
    intervalUs.reserve(valid);
    for (std::size_t i = 0U; i < valid; ++i) {
        busyUs.push_back(manager.tickBusyUs()[static_cast<int>(i)]);
        intervalUs.push_back(manager.tickIntervalUs()[static_cast<int>(i)]);
    }
    report.tickBusy = summarize(std::move(busyUs));
    report.tickInterval = summarize(std::move(intervalUs));

    // 界面刷新成本：直接调页面的刷新槽。它做的正是 statsRefreshed 到达时做的那些事
    // （取窗、抽稀、写入控件数据），只是不含绘制 —— 绘制归 grab() 那一项。
    if (curvePage != nullptr) {
        std::vector<long long> refreshUs;
        refreshUs.reserve(kCurveRefreshSamples);
        for (int i = 0; i < kRenderWarmups; ++i) {
            curvePage->refresh();
        }
        for (int i = 0; i < kCurveRefreshSamples; ++i) {
            QElapsedTimer timer;
            timer.start();
            curvePage->refresh();
            refreshUs.push_back(timer.nsecsElapsed() / 1000);
        }
        report.curveRefresh = summarize(std::move(refreshUs));
    }

    // 整页绘制成本：grab() 会把当前页整体画一遍（离屏平台下画进后备存储）。
    {
        std::vector<long long> renderUs;
        renderUs.reserve(kRenderSamples);
        for (int i = 0; i < kRenderWarmups; ++i) {
            (void)window.grab();
        }
        for (int i = 0; i < kRenderSamples; ++i) {
            QElapsedTimer timer;
            timer.start();
            (void)window.grab();
            renderUs.push_back(timer.nsecsElapsed() / 1000);
        }
        report.render = summarize(std::move(renderUs));
    }

    return report;
}

void printReport(const PhaseReport& report) {
    const double nominalHz =
        (report.telemetryPeriodMs == 0U) ? 0.0 : 1000.0 / static_cast<double>(report.telemetryPeriodMs);
    const double actualHz = (report.wallSeconds > 0.0)
                                ? static_cast<double>(report.telemetryAccepted) / report.wallSeconds
                                : 0.0;
    const double tickHz =
        (report.wallSeconds > 0.0) ? static_cast<double>(report.ticks) / report.wallSeconds : 0.0;

    std::printf("\n--- %s（设备产帧周期 %u ms，名义 %.0f 帧/s）---\n", report.label.toUtf8().constData(),
                report.telemetryPeriodMs, nominalHz);
    std::printf("  实测时长        : %.2f s（tick %llu 拍，实测 %.1f 拍/s）\n", report.wallSeconds,
                static_cast<unsigned long long>(report.ticks), tickHz);
    printSummary("每拍处理耗时  ", report.tickBusy, "us");
    printSummary("拍间隔        ", report.tickInterval, "us");
    std::printf("  上位机收遥测    : %llu 帧（实测 %.1f 帧/s，名义 %.0f 帧/s）| 拒收 %llu\n",
                static_cast<unsigned long long>(report.telemetryAccepted), actualHz, nominalHz,
                static_cast<unsigned long long>(report.telemetryRejected));
    std::printf("  解码            : %llu 帧 | CRC 错 %llu | 长度错 %llu | 重同步丢字节 %llu\n",
                static_cast<unsigned long long>(report.framesDecoded),
                static_cast<unsigned long long>(report.crcErrors),
                static_cast<unsigned long long>(report.badLengths),
                static_cast<unsigned long long>(report.bytesDiscarded));
    std::printf("  环形缓冲        : %llu 条 / 容量 4096（因覆盖丢弃 %llu）\n",
                static_cast<unsigned long long>(report.ringSize),
                static_cast<unsigned long long>(report.ringDiscarded));
    std::printf("  设备侧欠发丢弃  : %llu 帧（单拍上限 %zu 帧）\n",
                static_cast<unsigned long long>(report.deviceBacklogDropped),
                mc::kMaxTelemetryFramesPerTick);
    std::printf("  命令超时判 Fault: %llu\n",
                static_cast<unsigned long long>(report.ackTimeoutsFaulted));
    std::printf("  界面刷新信号    : %llu 次（%.1f 次/s，分频 %d）| 控件重绘 %llu 次\n",
                static_cast<unsigned long long>(report.refreshEmits),
                (report.wallSeconds > 0.0)
                    ? static_cast<double>(report.refreshEmits) / report.wallSeconds
                    : 0.0,
                mchost::DeviceManager::kUiRefreshDivider,
                static_cast<unsigned long long>(report.plotRepaints));
    printSummary("一次界面刷新  ", report.curveRefresh, "us");
    printSummary("一次整页绘制  ", report.render, "us");
}

}  // namespace

int main(int argc, char** argv) {
    // 离屏平台让测量在任何机器上都能跑，且不依赖桌面环境。
    // 想在真显示下测，自己设 QT_QPA_PLATFORM=xcb 再跑即可。
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication app(argc, argv);
    const Options options = parseOptions(QApplication::arguments());

    std::printf("=== mc_host_bench 单线程负载实测 ===\n");
    std::printf("平台            : %s\n", qgetenv("QT_QPA_PLATFORM").constData());
    std::printf("每档时长        : %d s\n", options.seconds);
#ifndef NDEBUG
    std::printf("⚠ 本次是 **未优化构建**（NDEBUG 未定义，默认 Debug）。\n"
                "  下面这些数字只说明“这条路径在做什么”，不能当作发布版本的性能指标 ——\n"
                "  要发布口径请用 -DCMAKE_BUILD_TYPE=Release 重新构建后再跑。\n");
#endif
    std::printf("说明            : 绘制由事件循环以独立事件投递，不在 onTick 回调里，\n"
                "                  故每拍耗时不含绘制；绘制成本由“一次整页绘制”那一项单独给出。\n");

    const PhaseReport baseline = runPhase(QStringLiteral("baseline 100 Hz"), 10U, options.seconds);
    printReport(baseline);

    const PhaseReport stress = runPhase(QStringLiteral("stress 1 kHz"), 1U, options.seconds);
    printReport(stress);

    return 0;
}
