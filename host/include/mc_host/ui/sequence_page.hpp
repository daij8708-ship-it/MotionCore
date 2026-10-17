#pragma once

#include <cstdint>

#include <QWidget>

#include "mc/sequence.hpp"

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QShowEvent;
class QSpinBox;
class QTableWidget;

namespace mchost {

class DeviceManager;

// 层 1：任务调度页（自动流程）。
//
// 这一页把"一次完整的动作"变成一件**可复核、可存档、能自己跑完**的事：
// 一串有序的步（设模式 / 设 PID / 设目标 / 启动 / 等待 / 停止）存进程序库，
// 点一次按钮按顺序跑完。
//
// ⚠️ 三条必须写在界面上的边界（它们不是界面细节，是这套东西能不能用在设备上的前提）：
//   ① **任何一步没拿到"已确认"，整部程序当场停在那一-步**。停下来的原因写在
//      运行详情里 —— 不是"程序失败了"四个字，而是"第 3 步 SET_TARGET 被拒：状态非法"。
//   ② **运动指令不自动重发**。超时之后流程不会自己再来一遍；要继续得有人看明白
//      之后重新启动。界面上也**不提供**"重试"这个按钮。
//   ③ **"中止"不是"撤销"**。已经下发并被确认的命令发生在过去，收不回来，
//      设备就停在那时的状态。中止按钮旁边必须写着这句话。
//
// 还有一条与安全页相同的红线：这一页不提供、也**不代替**任何物理安全回路。
// 程序跑得再顺，急停/断电仍然是物理回路的事。
class SequencePage : public QWidget {
    Q_OBJECT

public:
    explicit SequencePage(DeviceManager& manager, QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* event) override;

private slots:
    void refreshList();
    void onSelectionChanged();
    void onNewProgram();
    void saveProgram();
    void deleteProgram();
    void onKindChanged();
    void addStep();
    void moveStepUp();
    void moveStepDown();
    void removeStep();
    void runProgram();
    void abortProgram();
    void refreshDraft();
    void refreshRuntime();

private:
    // 把界面上的编辑器读成一部程序（**不校验**，校验是 refreshDraft 的事）。
    mc::SequenceProgram draftProgram() const;
    void rebuildStepTable();
    void updateButtons();
    int selectedStepRow() const;
    // 校验用的模式：设备在用什么环就按什么判；没有设备时按 0（不做量纲上界检查）。
    std::uint8_t effectiveModeForValidation() const;

    DeviceManager& manager_;

    QLabel* storeValue_ = nullptr;
    QLabel* storeErrorLabel_ = nullptr;

    QTableWidget* programTable_ = nullptr;
    QLineEdit* nameEdit_ = nullptr;
    QSpinBox* versionSpin_ = nullptr;
    QPushButton* newButton_ = nullptr;
    QPushButton* saveButton_ = nullptr;
    QPushButton* deleteButton_ = nullptr;
    QLabel* programHint_ = nullptr;
    // 动作的结果（存了/删了/没存上）与校验提示**分开两个标签**：
    // 校验提示是按编辑状态随时重算的，落在一个标签里的话，"已保存"会在
    // 刷新列表的那一瞬间被覆盖掉 —— 操作员根本没机会看见它。
    QLabel* resultLabel_ = nullptr;

    QTableWidget* stepTable_ = nullptr;
    QComboBox* kindCombo_ = nullptr;
    QComboBox* modeCombo_ = nullptr;
    // 两个字段名在回零步里是**换过的**（target = 速度、waitMs = 时限），
    // 所以标签本身要跟着类型变 —— 挂着"目标："让人填一个回零速度，
    // 填进去的数值看着完全正常，含义却完全不同。
    QLabel* targetLabel_ = nullptr;
    QLabel* waitLabel_ = nullptr;
    QDoubleSpinBox* targetSpin_ = nullptr;
    QDoubleSpinBox* kpSpin_ = nullptr;
    QDoubleSpinBox* kiSpin_ = nullptr;
    QDoubleSpinBox* kdSpin_ = nullptr;
    QSpinBox* waitSpin_ = nullptr;
    QLineEdit* noteEdit_ = nullptr;
    QPushButton* addStepButton_ = nullptr;
    QPushButton* stepUpButton_ = nullptr;
    QPushButton* stepDownButton_ = nullptr;
    QPushButton* stepRemoveButton_ = nullptr;

    QPushButton* runButton_ = nullptr;
    QPushButton* abortButton_ = nullptr;
    QLabel* runStatusLabel_ = nullptr;
    QLabel* runDetailLabel_ = nullptr;
    QTableWidget* runTable_ = nullptr;

    // 编辑器里的那部程序（含未保存的改动）。它就是"当前编辑的那部"，
    // 而不是"列表里选中的那一行"——两者在改了没存的时候是不一样的。
    mc::SequenceProgram draft_;
    // 步骤说明是按行存进 draft_ 的，所以表格里不再另存一份；这里只记"哪一行被选中"。
    qint64 selectedId_ = 0;
    // 删除要点两次：第一次只是"预备"。理由与配方页相同 —— 弹出的确认框在无头
    // 环境里没人点，而删除是不可逆的，不如做一个看得见的二次确认。
    bool deleteArmed_ = false;
};

}  // namespace mchost
