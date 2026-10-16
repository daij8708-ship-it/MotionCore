#include <gtest/gtest.h>

#include <QCoreApplication>

// 非界面用例的入口：建 QCoreApplication 而不建 QApplication。
//
// 这一支**不需要图形平台插件** —— 里面测的是曲线模型与设备管理器（QTimer、信号槽），
// 它们跟窗口系统没有关系。合成一个 QApplication 入口的话，"没有显示器"的环境里
// 连这些用例都跑不起来，而它们本来是能跑的。
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
