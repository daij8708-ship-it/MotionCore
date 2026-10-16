// ---------------------------------------------------------------------------
// mc_host —— Qt 上位机
//
// 它**不含任何协议实现**：所有帧、状态机、超时、重连都在 libmc_core 里，
// 本程序只做三件事：驱动时钟、把事件显示出来、把用户操作翻译成命令。
//
// 运行方式：
//   ./mc_host                      正常打开窗口（WSLg 下会直接显示在你的桌面上）
//   ./mc_host --selftest[=秒数]     无头自检：连上进程内模拟器跑一段，打印实测摘要并截图
//
// --selftest 不是"演示一下"，它是一个**有判据的自检**：任何一条不满足就以非 0 退出。
// 这样它可以直接挂进 ctest，让 Qt 这一层也有可复现的验收。
// ---------------------------------------------------------------------------
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include <QApplication>
#include <QElapsedTimer>
#include <QString>
#include <QStringList>
#include <QTabWidget>
#include <QTimer>

#include "mc/device_state.hpp"
#include "mc_host/device.hpp"
#include "mc_host/device_manager.hpp"
#include "mc_host/ui/curve_plot.hpp"
#include "mc_host/ui/main_window.hpp"

namespace {

struct Options {
    bool selftest = false;
    int seconds = 4;
    QString shotPrefix;   // 非空 → 每页存一张 <prefix>-<页名>.png
};

Options parseOptions(const QStringList& args) {
    Options options;
    for (const QString& arg : args) {
        if (arg == QStringLiteral("--selftest")) {
            options.selftest = true;
        } else if (arg.startsWith(QStringLiteral("--selftest="))) {
            options.selftest = true;
            options.seconds = arg.section('=', 1).toInt();
        } else if (arg.startsWith(QStringLiteral("--shot-prefix="))) {
            options.shotPrefix = arg.section('=', 1);
        }
    }
    return options;
}

int reportFailures = 0;

void require(bool condition, const char* what) {
    if (condition) {
        std::printf("  [ok]   %s\n", what);
    } else {
        std::printf("  [FAIL] %s\n", what);
        ++reportFailures;
    }
}

void printReport(mchost::DeviceManager& manager, mchost::MainWindow& window,
                 std::uint64_t refreshEmits) {
    const mchost::IDevice* device = manager.device();
    const mchost::TelemetryRing& ring = manager.telemetry();
    const mc::AckStats& ack = device->ackStats();
    const mc::StreamDecoder::Stats& decode = device->decoderStats();
    const std::uint64_t ticks = manager.ticks();

    std::printf("\n=== mc_host 自检报告 ===\n");
    std::printf("设备类型        : %s\n", device->kindName());
    std::printf("会话            : %s（%llu 轮握手）\n",
                device->sessionReady() ? "就绪" : "未就绪",
                static_cast<unsigned long long>(device->handshakeRounds()));
    std::printf("tick / 时长     : %llu 拍 / %.1f s\n",
                static_cast<unsigned long long>(manager.ticks()), manager.elapsedMs() / 1000.0);
    std::printf("发送帧          : %llu（编码失败 %llu / 发送失败 %llu）\n",
                static_cast<unsigned long long>(device->framesSent()),
                static_cast<unsigned long long>(device->hostEncodeFailures()),
                static_cast<unsigned long long>(device->hostSendFailures()));
    std::printf("应答 / NAK      : 对上 %llu / NAK %llu\n",
                static_cast<unsigned long long>(ack.completed),
                static_cast<unsigned long long>(ack.rejectedByPeer));
    std::printf("遥测            : 接收 %llu / 拒绝 %llu | 缓冲 %zu 条（丢弃 %llu）\n",
                static_cast<unsigned long long>(device->telemetryAcceptedTotal()),
                static_cast<unsigned long long>(device->telemetryRejectedTotal()), ring.size(),
                static_cast<unsigned long long>(ring.discarded()));
    std::printf("解码拒绝        : CRC %llu / 长度 %llu / 版本 %llu / 重同步字节 %llu\n",
                static_cast<unsigned long long>(decode.framesCrcError),
                static_cast<unsigned long long>(decode.framesBadLength),
                static_cast<unsigned long long>(decode.framesBadVersion),
                static_cast<unsigned long long>(decode.bytesDiscarded));
    std::printf("超时            : Fault %llu / 重发 %llu / 心跳漏拍 %llu\n",
                static_cast<unsigned long long>(ack.timedOutFaulted),
                static_cast<unsigned long long>(ack.timedOutRetried),
                static_cast<unsigned long long>(ack.timedOutDropped));
    std::printf("设备            : %s | 使能 %s | 速度 %.1f deg/s | 位置 %.1f deg | 目标 %.1f\n",
                mc::deviceStateName(device->deviceState()), device->enabled() ? "是" : "否",
                static_cast<double>(device->velocityDegPerSec()),
                static_cast<double>(device->positionDeg()),
                static_cast<double>(device->deviceTarget()));

    const std::deque<mchost::CommandRecord>& log = device->commandLog();
    std::printf("命令行          : 共 %zu 条（新→旧）\n", log.size());
    std::size_t shown = 0U;
    for (const mchost::CommandRecord& record : log) {
        if (shown >= 8U) {
            break;
        }
        std::printf("    %-12s #%-5u %s%s\n", record.name.c_str(), record.seq,
                    mchost::commandStatusName(record.status),
                    record.status == mchost::CommandStatus::Nacked
                        ? (QStringLiteral("（%1）")
                               .arg(QString::fromUtf8(mchost::nackReasonName(record.nackReason))))
                              .toUtf8()
                              .constData()
                        : "");
        ++shown;
    }

    mchost::CurvePlot* plot = window.findChild<mchost::CurvePlot*>(QStringLiteral("curvePlot"));
    std::printf("曲线控件        : 重绘 %llu 次（自检里显式渲染过）\n",
                static_cast<unsigned long long>(plot != nullptr ? plot->repaintCount() : 0U));
    std::printf("界面刷新信号    : %llu 次 / tick %llu 拍（分频 %d）\n",
                static_cast<unsigned long long>(refreshEmits),
                static_cast<unsigned long long>(ticks), mchost::DeviceManager::kUiRefreshDivider);

    std::printf("\n--- 判据 ---\n");
    require(device->sessionReady(), "会话自己握上手（没有手工发 HELLO）");
    require(ack.completed >= 5U, "至少 5 条命令被对端确认");
    require(ack.rejectedByPeer == 0U, "本场景没有被拒的命令");
    require(device->telemetryAcceptedTotal() > 100U, "遥测持续到达（>100 条）");
    require(device->telemetryRejectedTotal() == 0U, "没有被拒的遥测");
    require(device->deviceState() == mc::DeviceState::Running, "设备处于 Running");
    require(device->enabled(), "设备输出已使能");
    require(device->velocityDegPerSec() > 50.0F, "电机确实转起来了（速度 > 50 deg/s）");
    require(ack.timedOutFaulted == 0U, "健康链路上没有超时判 Fault");
    require(device->hostEncodeFailures() == 0U && device->hostSendFailures() == 0U,
            "上位机内部不变式成立（编码/发送失败为 0）");
    require(plot != nullptr && plot->repaintCount() > 0U,
            "曲线控件确实被绘制过（无头环境里由自检显式渲染一次）");
    // 节流的证据取"刷新信号次数"，不取"重绘次数"：无头环境下没人投递绘制事件，
    // 重绘次数只反映我们主动渲染了几次 —— 拿它比 tick 数等于自己造结论。
    require(refreshEmits > 0U, "界面刷新信号按分频发出（>0 次）");
    require(refreshEmits <= ticks / static_cast<std::uint64_t>(mchost::DeviceManager::kUiRefreshDivider) + 1U,
            "刷新次数不超过 tick 数的五分之一 —— 定频刷新确实在节流");
    std::printf("（判据失败 %d 条）\n", reportFailures);
}

// 绘制事件只投递给**当前可见页**，而且由事件循环异步投递 —— 自检里不能假设
// "曲线页已经画过了"（那几秒它并不是当前页）。要拿到"曲线控件真的画过"的证据，
// 只能切到那一页并显式渲染一次。这是自检的取样手段，不是产品运行时的路径。
//
// 页序会随界面一起长（现在 6 页，随后还有历史 / 设置）。这里**按页标题找下标**，
// 不写死数字：写死的话每加一页都要改一次，而且改错不会报错、只会静默地截错页面。
int tabIndexByTitle(const mchost::MainWindow& window, const QString& needle) {
    QTabWidget* tabs = window.tabs();
    if (tabs == nullptr) {
        return -1;
    }
    for (int i = 0; i < tabs->count(); ++i) {
        if (tabs->tabText(i).contains(needle)) {
            return i;
        }
    }
    return -1;
}

void forceOneRender(mchost::MainWindow& window) {
    const int curve = tabIndexByTitle(window, QStringLiteral("曲线"));
    if (curve < 0) {
        std::printf("切页失败：找不到标题含“曲线”的页\n");
        return;
    }
    (void)window.showTab(curve);
    QApplication::processEvents();
    (void)window.grab();
}

void saveTabShots(mchost::MainWindow& window, const QString& prefix) {
    struct Shot {
        const char* needle;
        const char* file;
    };
    const Shot shots[] = {{"总览", "overview"}, {"运动", "motion"}, {"参数", "param"},
                          {"曲线", "curve"},    {"报警", "alarm"},  {"调试", "debug"}};
    for (const Shot& shot : shots) {
        const int index = tabIndexByTitle(window, QString::fromUtf8(shot.needle));
        if (index < 0 || !window.showTab(index)) {
            std::printf("切页失败：%s\n", shot.file);
            continue;
        }
        // 让布局与绘制跑完再抓图，否则抓到的是上一页的残留。
        QApplication::processEvents();
        QApplication::processEvents();
        const QString path = QStringLiteral("%1-%2.png").arg(prefix, QString::fromUtf8(shot.file));
        const bool ok = window.grab().save(path);
        std::printf("截图 %-9s : %s（%s）\n", shot.file, ok ? "已保存" : "保存失败",
                    path.toUtf8().constData());
    }
    (void)window.showTab(0);
}

int runSelfTest(const Options& options, mchost::DeviceManager& manager, mchost::MainWindow& window) {
    const int seconds = (options.seconds > 0) ? options.seconds : 4;

    // 界面节流的证据必须来自"真的收到了多少次刷新信号"，而不是数控件重绘了几次 ——
    // 后者在无头环境里只反映我们主动渲染了几次（见 forceOneRender 的说明）。
    std::uint64_t refreshEmits = 0U;
    QObject::connect(&manager, &mchost::DeviceManager::statsRefreshed, &manager,
                     [&refreshEmits]() { ++refreshEmits; });

    // 脚本化的操作序列：每一步都对应一次真实的库调用（不是直接改状态）。
    QTimer::singleShot(200, &manager, &mchost::DeviceManager::connectDevice);
    QTimer::singleShot(500, &manager, [&manager]() { manager.sendSetMode(1); });
    QTimer::singleShot(700, &manager, [&manager]() { manager.sendStart(); });
    QTimer::singleShot(900, &manager, [&manager]() { manager.sendSetTarget(200.0); });
    QTimer::singleShot(1200, &manager, [&manager]() { manager.sendSetPid(1.6, 0.4, 0.02); });
    QTimer::singleShot(2000, &manager, [&manager]() { manager.sendSetTarget(300.0); });

    QTimer::singleShot(seconds * 1000, &manager, [&options, &manager, &window, &refreshEmits]() {
        forceOneRender(window);
        printReport(manager, window, refreshEmits);
        if (!options.shotPrefix.isEmpty()) {
            saveTabShots(window, options.shotPrefix);
        }
        QApplication::quit();
    });
    return QApplication::exec();
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    const Options options = parseOptions(QApplication::arguments());

    mchost::DeviceManager manager;
    mchost::MainWindow window(manager);
    window.show();

    if (options.selftest) {
        const int code = runSelfTest(options, manager, window);
        return (reportFailures == 0) ? code : 1;
    }
    return QApplication::exec();
}
