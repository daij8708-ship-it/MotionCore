#pragma once

#include <cstdint>

#include <QWidget>

#include "mc/recipe.hpp"

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;

namespace mchost {

class DeviceManager;

// 层 1：工艺配方页。
//
// 这一页把"设备按什么参数干活"变成一件**可存档、可版本化**的事：
// 一份配方 = 一组具名参数（模式 / 目标 / 三个 PID 增益），存进配方库，
// 需要的时候下装到设备。参数不再只活在界面控件里 —— 关掉窗口就没了。
//
// ⚠️ 一条必须写在界面上的边界：协议 v1 里**没有"读参数"的消息**，遥测帧里也只带
//    target。所以下装之后的证据是**两层**的：
//      ① 三条命令都被设备确认（应答）—— 覆盖模式 / PID / 目标三项；
//      ② 目标值另有**回读**核对（遥测里真的有它）—— 只覆盖目标这一项。
//    把这两件事说成一件事，就等于宣称"我猜它生效了"。
class RecipePage : public QWidget {
    Q_OBJECT

public:
    explicit RecipePage(DeviceManager& manager, QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* event) override;

private slots:
    void refreshList();
    void onSelectionChanged();
    void onNewRecipe();
    void saveRecipe();
    void deleteRecipe();
    void validateEditor();
    void downloadRecipe();
    void cancelDownload();
    void refreshRuntimeState();

private:
    // 把编辑器里的内容读成一份配方（**不校验**，校验是 validateEditor 的事）。
    mc::Recipe editorRecipe() const;
    void loadIntoEditor(const mc::Recipe& recipe);
    void applyModeToTargetRange();
    void updateDownloadButton();
    void updateDownloadTable();

    DeviceManager& manager_;

    QTableWidget* table_ = nullptr;
    QLabel* storeValue_ = nullptr;
    QLabel* storeErrorLabel_ = nullptr;

    QLineEdit* nameEdit_ = nullptr;
    QSpinBox* versionSpin_ = nullptr;
    QComboBox* modeCombo_ = nullptr;
    QDoubleSpinBox* targetSpin_ = nullptr;
    QDoubleSpinBox* kpSpin_ = nullptr;
    QDoubleSpinBox* kiSpin_ = nullptr;
    QDoubleSpinBox* kdSpin_ = nullptr;
    QLabel* validationLabel_ = nullptr;
    QPushButton* newButton_ = nullptr;
    QPushButton* saveButton_ = nullptr;
    QPushButton* deleteButton_ = nullptr;
    QLabel* resultLabel_ = nullptr;

    QPushButton* downloadButton_ = nullptr;
    QPushButton* cancelButton_ = nullptr;
    QLabel* downloadStatusLabel_ = nullptr;
    QTableWidget* downloadTable_ = nullptr;

    qint64 selectedId_ = 0;
    // 删除要点两次：第一次只是"预备"。弹出的确认框在无头环境里没人点，
    // 而这个动作是不可逆的 —— 与其做一个测不到的确认框，不如做一个看得见的二次确认。
    bool deleteArmed_ = false;
};

}  // namespace mchost
