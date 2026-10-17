#include <gtest/gtest.h>

#include <QApplication>
#include <QByteArray>
#include <QStandardPaths>
#include <QtGlobal>

// 界面用例的入口：必须建 QApplication（控件构造时就要求它存在）。
int main(int argc, char** argv) {
    // 没显式指定平台就退到 offscreen：界面用例不该"因为没人坐在电脑前"而变红。
    // 显式设了（例如 xcb / WSLg）就尊重它 —— 覆盖用户的选择才是更坏的行为。
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    }
    // 同 core_test_main：用例不往用户数据目录里写历史库（理由见那边）。
    QStandardPaths::setTestModeEnabled(true);

    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
