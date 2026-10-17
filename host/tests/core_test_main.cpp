#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QStandardPaths>

// 非界面用例的入口：建 QCoreApplication 而不建 QApplication。
//
// 这一支**不需要图形平台插件** —— 里面测的是曲线模型与设备管理器（QTimer、信号槽），
// 它们跟窗口系统没有关系。合成一个 QApplication 入口的话，"没有显示器"的环境里
// 连这些用例都跑不起来，而它们本来是能跑的。
int main(int argc, char** argv) {
    // 用例不许在用户数据目录里留东西：`DeviceManager` 构造时会按默认路径建一个历史库，
    // 而这里的用例会真的连接设备、真的产生遥测 —— 不隔离的话，跑一次 ctest
    // 就在 $HOME 里多出一份"看起来像真实运行记录"的库，越跑越大。
    // 切到 QStandardPaths 的测试模式后，默认路径落到隔离目录（~/.qttest/…）。
    QStandardPaths::setTestModeEnabled(true);

    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
