#include "mc_host/ui/sequence_page.hpp"

#include <utility>

#include <QBrush>
#include <QColor>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QShowEvent>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

#include "mc/device_state.hpp"
#include "mc_host/device.hpp"
#include "mc_host/device_manager.hpp"
#include "mc_host/sequence_text.hpp"

namespace mchost {
namespace {

constexpr int kStepColumns = 4;   // 序号 | 类型 | 参数 | 说明
constexpr int kRunColumns = 4;    // 第几步 | 类型 | 结局 | 说明

QColor phaseColor(mc::SequencePhase phase) {
    switch (phase) {
        case mc::SequencePhase::Idle:      return QColor(QStringLiteral("#555555"));
        case mc::SequencePhase::Running:   return QColor(QStringLiteral("#1b5e20"));
        case mc::SequencePhase::Succeeded: return QColor(QStringLiteral("#1b5e20"));
        case mc::SequencePhase::Failed:    return QColor(QStringLiteral("#b00020"));
        case mc::SequencePhase::Aborted:   return QColor(QStringLiteral("#8a5a00"));
    }
    return QColor();
}

QColor outcomeColor(mc::StepOutcome outcome) {
    return (outcome == mc::StepOutcome::Acked) ? QColor(QStringLiteral("#1b5e20"))
                                               : QColor(QStringLiteral("#b00020"));
}

}  // namespace

SequencePage::SequencePage(DeviceManager& manager, QWidget* parent)
    : QWidget(parent), manager_(manager) {
    auto* root = new QVBoxLayout(this);

    auto* banner = new QLabel(
        QStringLiteral("⚠ 自动流程在任何一步没拿到“已确认”时当场停下，**不会自动重发**"
                       "（运动指令的规矩）；“中止”只让后面的事不再发生，已经下发并被确认的"
                       "命令收不回来，设备保持在那时的状态。急停 / 断电是**物理**回路，"
                       "这一页不提供任何替代。"),
        this);
    banner->setObjectName(QStringLiteral("sequenceBanner"));
    banner->setWordWrap(true);
    banner->setStyleSheet(
        QStringLiteral("background:#7a1f1f; color:#fff0f0; padding:7px; border-radius:4px;"));
    root->addWidget(banner);

    // --- 程序库 ---------------------------------------------------------------
    auto* storeGroup = new QGroupBox(QStringLiteral("程序库（SQLite）"), this);
    auto* storeGrid = new QGridLayout(storeGroup);
    storeGrid->addWidget(new QLabel(QStringLiteral("位置："), this), 0, 0);
    storeValue_ = new QLabel(this);
    storeValue_->setObjectName(QStringLiteral("sequencePathValue"));
    storeValue_->setWordWrap(true);
    storeValue_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    storeGrid->addWidget(storeValue_, 0, 1);
    storeErrorLabel_ = new QLabel(this);
    storeErrorLabel_->setObjectName(QStringLiteral("sequenceStoreErrorLabel"));
    storeErrorLabel_->setWordWrap(true);
    storeErrorLabel_->setStyleSheet(QStringLiteral("color:#7a1f1f;"));
    storeGrid->addWidget(storeErrorLabel_, 1, 0, 1, 2);
    storeGrid->setColumnStretch(1, 1);
    root->addWidget(storeGroup);

    auto* lists = new QHBoxLayout();

    auto* programGroup = new QGroupBox(QStringLiteral("程序"), this);
    auto* programLayout = new QVBoxLayout(programGroup);
    programTable_ = new QTableWidget(0, 4, this);
    programTable_->setObjectName(QStringLiteral("sequenceProgramTable"));
    programTable_->setHorizontalHeaderLabels({QStringLiteral("名称"), QStringLiteral("版本"),
                                              QStringLiteral("步数"), QStringLiteral("状态")});
    programTable_->verticalHeader()->setVisible(false);
    programTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    programTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    programTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    programTable_->horizontalHeader()->setStretchLastSection(true);
    programLayout->addWidget(programTable_, 1);

    auto* editorRow = new QGridLayout();
    editorRow->addWidget(new QLabel(QStringLiteral("名称："), this), 0, 0);
    nameEdit_ = new QLineEdit(this);
    nameEdit_->setObjectName(QStringLiteral("programNameEdit"));
    nameEdit_->setPlaceholderText(QStringLiteral("例如 WAFER-A1-流程"));
    editorRow->addWidget(nameEdit_, 0, 1);
    editorRow->addWidget(new QLabel(QStringLiteral("版本："), this), 0, 2);
    versionSpin_ = new QSpinBox(this);
    versionSpin_->setObjectName(QStringLiteral("programVersionSpin"));
    versionSpin_->setRange(1, 9999);
    versionSpin_->setValue(1);
    editorRow->addWidget(versionSpin_, 0, 3);
    editorRow->setColumnStretch(1, 1);
    programLayout->addLayout(editorRow);

    auto* programButtons = new QHBoxLayout();
    newButton_ = new QPushButton(QStringLiteral("新建（清空编辑器）"), this);
    newButton_->setObjectName(QStringLiteral("programNewButton"));
    saveButton_ = new QPushButton(QStringLiteral("保存"), this);
    saveButton_->setObjectName(QStringLiteral("programSaveButton"));
    deleteButton_ = new QPushButton(QStringLiteral("删除"), this);
    deleteButton_->setObjectName(QStringLiteral("programDeleteButton"));
    programButtons->addWidget(newButton_);
    programButtons->addWidget(saveButton_);
    programButtons->addWidget(deleteButton_);
    programButtons->addStretch(1);
    programLayout->addLayout(programButtons);

    programHint_ = new QLabel(this);
    programHint_->setObjectName(QStringLiteral("programHintLabel"));
    programHint_->setWordWrap(true);
    programHint_->setStyleSheet(QStringLiteral("color:#444;"));
    programLayout->addWidget(programHint_);

    resultLabel_ = new QLabel(this);
    resultLabel_->setObjectName(QStringLiteral("sequenceResultLabel"));
    resultLabel_->setWordWrap(true);
    programLayout->addWidget(resultLabel_);
    lists->addWidget(programGroup, 2);

    // --- 步骤 ---------------------------------------------------------------
    auto* stepGroup = new QGroupBox(QStringLiteral("步骤（按顺序执行）"), this);
    auto* stepLayout = new QVBoxLayout(stepGroup);
    stepTable_ = new QTableWidget(0, kStepColumns, this);
    stepTable_->setObjectName(QStringLiteral("sequenceStepTable"));
    stepTable_->setHorizontalHeaderLabels({QStringLiteral("步"), QStringLiteral("类型"),
                                           QStringLiteral("参数"), QStringLiteral("说明")});
    stepTable_->verticalHeader()->setVisible(false);
    stepTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    stepTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    stepTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    stepTable_->horizontalHeader()->setStretchLastSection(true);
    stepLayout->addWidget(stepTable_, 1);

    auto* addRow = new QGridLayout();
    addRow->addWidget(new QLabel(QStringLiteral("类型："), this), 0, 0);
    kindCombo_ = new QComboBox(this);
    kindCombo_->setObjectName(QStringLiteral("stepKindCombo"));
    for (int i = 0; i <= static_cast<int>(mc::SequenceStepKind::Home); ++i) {
        const auto kind = static_cast<mc::SequenceStepKind>(i);
        kindCombo_->addItem(sequenceStepKindText(kind), i);
    }
    addRow->addWidget(kindCombo_, 0, 1);

    addRow->addWidget(new QLabel(QStringLiteral("模式："), this), 0, 2);
    modeCombo_ = new QComboBox(this);
    modeCombo_->setObjectName(QStringLiteral("stepModeCombo"));
    modeCombo_->addItem(QStringLiteral("0 停止控制"), 0);
    modeCombo_->addItem(QStringLiteral("1 速度环"), 1);
    modeCombo_->addItem(QStringLiteral("2 位置环"), 2);
    modeCombo_->setCurrentIndex(1);
    addRow->addWidget(modeCombo_, 0, 3);

    targetLabel_ = new QLabel(QStringLiteral("目标："), this);
    addRow->addWidget(targetLabel_, 0, 4);
    targetSpin_ = new QDoubleSpinBox(this);
    targetSpin_->setObjectName(QStringLiteral("stepTargetSpin"));
    targetSpin_->setDecimals(3);
    targetSpin_->setRange(-1.0e9, 1.0e9);
    targetSpin_->setValue(200.0);
    addRow->addWidget(targetSpin_, 0, 5);

    waitLabel_ = new QLabel(QStringLiteral("等待(ms)："), this);
    addRow->addWidget(waitLabel_, 0, 6);
    waitSpin_ = new QSpinBox(this);
    waitSpin_->setObjectName(QStringLiteral("stepWaitSpin"));
    waitSpin_->setRange(0, static_cast<int>(mc::kMaxWaitMs));
    waitSpin_->setSingleStep(100);
    waitSpin_->setValue(1000);
    addRow->addWidget(waitSpin_, 0, 7);
    addRow->setColumnStretch(8, 1);

    auto* pidRow = new QGridLayout();
    pidRow->addWidget(new QLabel(QStringLiteral("Kp："), this), 0, 0);
    kpSpin_ = new QDoubleSpinBox(this);
    kpSpin_->setObjectName(QStringLiteral("stepKpSpin"));
    kpSpin_->setDecimals(4);
    kpSpin_->setRange(0.0, 1.0e6);
    kpSpin_->setValue(1.5);
    pidRow->addWidget(kpSpin_, 0, 1);
    pidRow->addWidget(new QLabel(QStringLiteral("Ki："), this), 0, 2);
    kiSpin_ = new QDoubleSpinBox(this);
    kiSpin_->setObjectName(QStringLiteral("stepKiSpin"));
    kiSpin_->setDecimals(4);
    kiSpin_->setRange(0.0, 1.0e6);
    kiSpin_->setValue(0.3);
    pidRow->addWidget(kiSpin_, 0, 3);
    pidRow->addWidget(new QLabel(QStringLiteral("Kd："), this), 0, 4);
    kdSpin_ = new QDoubleSpinBox(this);
    kdSpin_->setObjectName(QStringLiteral("stepKdSpin"));
    kdSpin_->setDecimals(4);
    kdSpin_->setRange(0.0, 1.0e6);
    kdSpin_->setValue(0.0);
    pidRow->addWidget(kdSpin_, 0, 5);
    pidRow->addWidget(new QLabel(QStringLiteral("说明："), this), 0, 6);
    noteEdit_ = new QLineEdit(this);
    noteEdit_->setObjectName(QStringLiteral("stepNoteEdit"));
    noteEdit_->setPlaceholderText(QStringLiteral("这一步是干什么的（复核时要看的就是它）"));
    pidRow->addWidget(noteEdit_, 0, 7);
    pidRow->setColumnStretch(7, 1);
    stepLayout->addLayout(addRow);
    stepLayout->addLayout(pidRow);

    auto* stepButtons = new QHBoxLayout();
    addStepButton_ = new QPushButton(QStringLiteral("添加这一步"), this);
    addStepButton_->setObjectName(QStringLiteral("addStepButton"));
    stepUpButton_ = new QPushButton(QStringLiteral("上移"), this);
    stepUpButton_->setObjectName(QStringLiteral("stepUpButton"));
    stepDownButton_ = new QPushButton(QStringLiteral("下移"), this);
    stepDownButton_->setObjectName(QStringLiteral("stepDownButton"));
    stepRemoveButton_ = new QPushButton(QStringLiteral("删除选中步"), this);
    stepRemoveButton_->setObjectName(QStringLiteral("stepRemoveButton"));
    stepButtons->addWidget(addStepButton_);
    stepButtons->addStretch(1);
    stepButtons->addWidget(stepUpButton_);
    stepButtons->addWidget(stepDownButton_);
    stepButtons->addWidget(stepRemoveButton_);
    stepLayout->addLayout(stepButtons);
    lists->addWidget(stepGroup, 3);
    lists->setStretch(0, 2);
    lists->setStretch(1, 3);
    root->addLayout(lists, 2);

    // --- 运行 ---------------------------------------------------------------
    auto* runGroup = new QGroupBox(QStringLiteral("运行"), this);
    auto* runLayout = new QGridLayout(runGroup);

    runButton_ = new QPushButton(QStringLiteral("执行当前程序"), this);
    runButton_->setObjectName(QStringLiteral("runProgramButton"));
    abortButton_ = new QPushButton(QStringLiteral("中止（不等于撤销）"), this);
    abortButton_->setObjectName(QStringLiteral("abortProgramButton"));
    runLayout->addWidget(runButton_, 0, 0);
    runLayout->addWidget(abortButton_, 0, 1);

    runStatusLabel_ = new QLabel(this);
    runStatusLabel_->setObjectName(QStringLiteral("runStatusLabel"));
    runStatusLabel_->setWordWrap(true);
    runLayout->addWidget(runStatusLabel_, 1, 0, 1, 2);

    runDetailLabel_ = new QLabel(this);
    runDetailLabel_->setObjectName(QStringLiteral("runDetailLabel"));
    runDetailLabel_->setWordWrap(true);
    runDetailLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    runLayout->addWidget(runDetailLabel_, 2, 0, 1, 2);

    runTable_ = new QTableWidget(0, kRunColumns, this);
    runTable_->setObjectName(QStringLiteral("runReportTable"));
    runTable_->setHorizontalHeaderLabels({QStringLiteral("步"), QStringLiteral("类型"),
                                         QStringLiteral("结局"), QStringLiteral("说明")});
    runTable_->verticalHeader()->setVisible(false);
    runTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    runTable_->horizontalHeader()->setStretchLastSection(true);
    runLayout->addWidget(runTable_, 3, 0, 1, 2);
    runLayout->setColumnStretch(1, 1);
    root->addWidget(runGroup, 1);

    connect(programTable_, &QTableWidget::itemSelectionChanged, this,
            &SequencePage::onSelectionChanged);
    // 名字与版本也要跟着校验：不然提示会停在上一句上，而那一句已经不是
    // 现在这部程序的问题了。
    connect(nameEdit_, &QLineEdit::textChanged, this, [this]() { refreshDraft(); });
    connect(versionSpin_, &QSpinBox::valueChanged, this, [this]() { refreshDraft(); });
    connect(newButton_, &QPushButton::clicked, this, &SequencePage::onNewProgram);
    connect(saveButton_, &QPushButton::clicked, this, &SequencePage::saveProgram);
    connect(deleteButton_, &QPushButton::clicked, this, &SequencePage::deleteProgram);
    connect(kindCombo_, &QComboBox::currentIndexChanged, this, &SequencePage::onKindChanged);
    connect(modeCombo_, &QComboBox::currentIndexChanged, this, [this]() { refreshDraft(); });
    connect(targetSpin_, &QDoubleSpinBox::valueChanged, this, [this]() { refreshDraft(); });
    connect(kpSpin_, &QDoubleSpinBox::valueChanged, this, [this]() { refreshDraft(); });
    connect(kiSpin_, &QDoubleSpinBox::valueChanged, this, [this]() { refreshDraft(); });
    connect(kdSpin_, &QDoubleSpinBox::valueChanged, this, [this]() { refreshDraft(); });
    connect(waitSpin_, &QSpinBox::valueChanged, this, [this]() { refreshDraft(); });
    connect(addStepButton_, &QPushButton::clicked, this, &SequencePage::addStep);
    connect(stepUpButton_, &QPushButton::clicked, this, &SequencePage::moveStepUp);
    connect(stepDownButton_, &QPushButton::clicked, this, &SequencePage::moveStepDown);
    connect(stepRemoveButton_, &QPushButton::clicked, this, &SequencePage::removeStep);
    connect(stepTable_, &QTableWidget::itemSelectionChanged, this,
            [this]() { updateButtons(); });
    connect(runButton_, &QPushButton::clicked, this, &SequencePage::runProgram);
    connect(abortButton_, &QPushButton::clicked, this, &SequencePage::abortProgram);
    connect(&manager_, &DeviceManager::sequenceChanged, this, &SequencePage::refreshRuntime);
    connect(&manager_, &DeviceManager::connectionChanged, this,
            [this](bool) { refreshRuntime(); });
    connect(&manager_, &DeviceManager::statsRefreshed, this, &SequencePage::refreshRuntime);

    onKindChanged();
    refreshList();
    refreshRuntime();
}

std::uint8_t SequencePage::effectiveModeForValidation() const {
    const IDevice* device = manager_.device();
    if (device == nullptr || !device->sessionReady()) {
        // 没有设备可问的时候按 0 判（= 不做量纲上界检查）：猜一个模式出来
        // 比不猜更糟 —— 猜错的那次会以"合法"的样子放过一个错的程序。
        return 0U;
    }
    return static_cast<std::uint8_t>(device->runMode());
}

void SequencePage::onKindChanged() {
    const auto kind = static_cast<mc::SequenceStepKind>(kindCombo_->currentData().toInt());
    // 只有用得上的输入才可编辑：一列全亮着的输入框会让人以为
    // "设置模式"这一步也要填 PID。
    modeCombo_->setEnabled(kind == mc::SequenceStepKind::SetMode);
    const bool home = (kind == mc::SequenceStepKind::Home);
    targetSpin_->setEnabled(kind == mc::SequenceStepKind::SetTarget || home);
    const bool pid = (kind == mc::SequenceStepKind::SetPid);
    kpSpin_->setEnabled(pid);
    kiSpin_->setEnabled(pid);
    kdSpin_->setEnabled(pid);
    // 等待与"等设备就绪"用同一个输入框，但含义不同：前者是要等多久，
    // 后者是**最多**等多久 —— 界面上的文案跟着变，免得把上限当成固定时长。
    waitSpin_->setEnabled(kind == mc::SequenceStepKind::Wait ||
                          kind == mc::SequenceStepKind::WaitReady || home);

    // 回零步里两个字段的含义是换过的，标签必须跟着换。见头文件那段的说明。
    targetLabel_->setText(home ? QStringLiteral("回零速度(deg/s)：") : QStringLiteral("目标："));
    waitLabel_->setText(home ? QStringLiteral("时限(ms)：") : QStringLiteral("等待(ms)："));
    if (kind == mc::SequenceStepKind::WaitReady) {
        waitSpin_->setSuffix(QStringLiteral(" ms（上限）"));
    } else if (home) {
        waitSpin_->setSuffix(QStringLiteral(" ms（走不到就算失败）"));
    } else {
        waitSpin_->setSuffix(QStringLiteral(" ms"));
    }
}

mc::SequenceProgram SequencePage::draftProgram() const {
    mc::SequenceProgram program = draft_;
    program.name = nameEdit_->text().toStdString();
    program.version = static_cast<std::uint16_t>(versionSpin_->value());
    return program;
}

void SequencePage::rebuildStepTable() {
    stepTable_->setRowCount(static_cast<int>(draft_.steps.size()));
    for (int row = 0; row < static_cast<int>(draft_.steps.size()); ++row) {
        const mc::SequenceStep& step = draft_.steps[static_cast<std::size_t>(row)];
        stepTable_->setItem(row, 0, new QTableWidgetItem(QString::number(row + 1)));
        stepTable_->setItem(row, 1,
                            new QTableWidgetItem(sequenceStepKindText(step.kind)));
        stepTable_->setItem(row, 2, new QTableWidgetItem(sequenceStepSummary(step)));
        stepTable_->setItem(row, 3, new QTableWidgetItem(QString::fromStdString(step.note)));
    }
    stepTable_->resizeColumnsToContents();
}

int SequencePage::selectedStepRow() const {
    return stepTable_->currentRow();
}

void SequencePage::refreshDraft() {
    const mc::SequenceProgram program = draftProgram();
    const mc::SequenceError verdict =
        mc::validateSequence(program, effectiveModeForValidation());

    if (verdict == mc::SequenceError::None) {
        programHint_->setText(
            QStringLiteral("这部程序是合法的（共 %1 步）；按什么模式判量纲：%2")
                .arg(static_cast<int>(program.steps.size()))
                .arg(static_cast<int>(effectiveModeForValidation())));
    } else {
        programHint_->setText(QStringLiteral("还不能保存或执行：%1").arg(sequenceErrorText(verdict)));
    }
    updateButtons();
}

void SequencePage::updateButtons() {
    const mc::SequenceProgram program = draftProgram();
    const bool valid = mc::validateSequence(program, effectiveModeForValidation()) ==
                       mc::SequenceError::None;

    const bool storeOpen = manager_.sequencesOpen();
    saveButton_->setEnabled(storeOpen && valid);
    deleteButton_->setEnabled(storeOpen && selectedId_ > 0);
    // 删除的二次确认：按钮上的字跟着变，而不是弹一个测不到的框。
    deleteButton_->setText(deleteArmed_ ? QStringLiteral("再点一次确认删除")
                                        : QStringLiteral("删除"));
    deleteButton_->setToolTip(deleteArmed_ ? QStringLiteral("再点一次就真删了")
                                           : QStringLiteral("点两次才会删：这一步不可逆"));

    const int row = selectedStepRow();
    stepUpButton_->setEnabled(row > 0);
    stepDownButton_->setEnabled(row >= 0 && row + 1 < static_cast<int>(draft_.steps.size()));
    stepRemoveButton_->setEnabled(row >= 0);

    const bool running = manager_.sequencer().status().phase == mc::SequencePhase::Running;
    runButton_->setEnabled(valid && !running);
    abortButton_->setEnabled(running);
    addStepButton_->setEnabled(!running);
    runButton_->setToolTip(running ? QStringLiteral("已经有一部程序在跑：先等它结束或中止它")
                                   : QStringLiteral("从 Ready 开始按顺序执行这部程序"));
}

void SequencePage::onSelectionChanged() {
    if (programTable_->selectedItems().isEmpty()) {
        return;
    }
    const int row = programTable_->currentRow();
    const QTableWidgetItem* item = programTable_->item(row, 0);
    if (item == nullptr) {
        return;
    }
    const qint64 id = item->data(Qt::UserRole).toLongLong();

    SequenceStore* store = manager_.sequences();
    if (store == nullptr) {
        return;
    }
    QString error;
    const std::vector<SequenceStore::Row> rows = store->list(error);
    for (const SequenceStore::Row& candidate : rows) {
        if (candidate.id != id) {
            continue;
        }
        if (candidate.corrupted) {
            // 坏掉的行**不许被当成能用的程序载进编辑器** —— 那会把一份
            // 已经坏了的东西显示成"好好的"。只把名字填上，让人知道是哪一份坏了。
            draft_ = mc::SequenceProgram{};
            draft_.name = candidate.program.name;
            draft_.version = candidate.program.version;
            selectedId_ = 0;
            resultLabel_->setText(QStringLiteral("这一份不能被编辑或执行：%1")
                                      .arg(candidate.problem));
        } else {
            draft_ = candidate.program;
            selectedId_ = candidate.id;
            resultLabel_->clear();
        }
        nameEdit_->setText(QString::fromStdString(draft_.name));
        versionSpin_->setValue(static_cast<int>(draft_.version));
        rebuildStepTable();
        deleteArmed_ = false;
        refreshDraft();
        return;
    }
}

void SequencePage::refreshList() {
    SequenceStore* store = manager_.sequences();
    if (store == nullptr) {
        storeValue_->setText(QStringLiteral("未启用"));
        storeErrorLabel_->setText(QStringLiteral("没有程序库"));
        return;
    }
    storeValue_->setText(store->isOpen() ? store->path() : QStringLiteral("未启用"));

    QString error;
    const std::vector<SequenceStore::Row> rows = store->list(error);
    storeErrorLabel_->setText(error.isEmpty() ? QString()
                                              : QStringLiteral("读程序库失败：%1").arg(error));

    programTable_->setRowCount(static_cast<int>(rows.size()));
    int row = 0;
    for (const SequenceStore::Row& candidate : rows) {
        auto* nameItem = new QTableWidgetItem(QString::fromStdString(candidate.program.name));
        nameItem->setData(Qt::UserRole, candidate.id);
        programTable_->setItem(row, 0, nameItem);
        programTable_->setItem(row, 1,
                               new QTableWidgetItem(QString::number(candidate.program.version)));
        programTable_->setItem(
            row, 2,
            new QTableWidgetItem(QString::number(
                static_cast<int>(candidate.program.steps.size()))));
        auto* stateItem = new QTableWidgetItem(
            candidate.corrupted ? QStringLiteral("损坏：%1").arg(candidate.problem)
                                : QStringLiteral("可用"));
        if (candidate.corrupted) {
            stateItem->setForeground(QBrush(QColor(QStringLiteral("#b00020"))));
        }
        programTable_->setItem(row, 3, stateItem);
        ++row;
    }
    programTable_->resizeColumnsToContents();
    refreshDraft();
}

void SequencePage::onNewProgram() {
    draft_ = mc::SequenceProgram{};
    selectedId_ = 0;
    deleteArmed_ = false;
    nameEdit_->clear();
    versionSpin_->setValue(1);
    rebuildStepTable();
    programTable_->clearSelection();
    resultLabel_->clear();
    refreshDraft();
}

void SequencePage::saveProgram() {
    // 与配方页同一个道理：写库走管理层的入口，权限与审计才有唯一的一处。
    const bool updating = selectedId_ > 0;
    const mc::SequenceProgram program = draftProgram();
    QString error;
    if (!manager_.saveSequence(program, &selectedId_, error)) {
        resultLabel_->setText(QStringLiteral("没保存：%1").arg(error));
        deleteArmed_ = false;
        refreshList();
        return;
    }
    resultLabel_->setText(QStringLiteral("%1 %2 v%3")
                              .arg(updating ? QStringLiteral("已更新") : QStringLiteral("已保存"),
                                   QString::fromStdString(program.name))
                              .arg(program.version));
    deleteArmed_ = false;
    refreshList();
}

void SequencePage::deleteProgram() {
    if (selectedId_ <= 0) {
        return;
    }
    if (!deleteArmed_) {
        deleteArmed_ = true;
        resultLabel_->setText(QStringLiteral("再点一次「删除」就真的删掉了（不可逆）。"));
        updateButtons();
        return;
    }
    deleteArmed_ = false;
    QString error;
    if (!manager_.deleteSequence(selectedId_, error)) {
        resultLabel_->setText(QStringLiteral("没删除：%1").arg(error));
        refreshList();
        return;
    }
    resultLabel_->setText(QStringLiteral("已删除"));
    draft_ = mc::SequenceProgram{};
    selectedId_ = 0;
    nameEdit_->clear();
    versionSpin_->setValue(1);
    rebuildStepTable();
    refreshList();
}

void SequencePage::addStep() {
    if (draft_.steps.size() >= mc::kMaxSequenceSteps) {
        programHint_->setText(QStringLiteral("步数已达上限（%1）：太长就没人核得完了")
                                  .arg(static_cast<int>(mc::kMaxSequenceSteps)));
        return;
    }
    mc::SequenceStep step;
    step.kind = static_cast<mc::SequenceStepKind>(kindCombo_->currentData().toInt());
    step.mode = static_cast<std::uint8_t>(modeCombo_->currentData().toInt());
    step.target = static_cast<float>(targetSpin_->value());
    step.kp = static_cast<float>(kpSpin_->value());
    step.ki = static_cast<float>(kiSpin_->value());
    step.kd = static_cast<float>(kdSpin_->value());
    step.waitMs = static_cast<std::uint64_t>(waitSpin_->value());
    step.note = noteEdit_->text().toStdString();

    draft_.steps.push_back(std::move(step));
    rebuildStepTable();
    stepTable_->selectRow(static_cast<int>(draft_.steps.size()) - 1);
    refreshDraft();
}

void SequencePage::moveStepUp() {
    const int row = selectedStepRow();
    if (row <= 0 || row >= static_cast<int>(draft_.steps.size())) {
        return;
    }
    std::swap(draft_.steps[static_cast<std::size_t>(row)],
              draft_.steps[static_cast<std::size_t>(row - 1)]);
    rebuildStepTable();
    stepTable_->selectRow(row - 1);
    refreshDraft();
}

void SequencePage::moveStepDown() {
    const int row = selectedStepRow();
    if (row < 0 || row + 1 >= static_cast<int>(draft_.steps.size())) {
        return;
    }
    std::swap(draft_.steps[static_cast<std::size_t>(row)],
              draft_.steps[static_cast<std::size_t>(row + 1)]);
    rebuildStepTable();
    stepTable_->selectRow(row + 1);
    refreshDraft();
}

void SequencePage::removeStep() {
    const int row = selectedStepRow();
    if (row < 0 || row >= static_cast<int>(draft_.steps.size())) {
        return;
    }
    draft_.steps.erase(draft_.steps.begin() + row);
    rebuildStepTable();
    refreshDraft();
}

void SequencePage::runProgram() {
    const bool started = manager_.startSequence(draftProgram());
    refreshRuntime();
    if (!started) {
        // 没启动起来的原因**必须原样显示**：门禁拦下的理由（报警未确认、设备没停在
        // Ready、会话未就绪）是操作员唯一能照着改的东西。
        // 放在 refreshRuntime 之后写，免得被它按状态刷掉。
        runDetailLabel_->setText(manager_.sequenceStartError());
    }
}

void SequencePage::abortProgram() {
    if (!manager_.abortSequence()) {
        runDetailLabel_->setText(manager_.lastRefusalText());
        return;
    }
    refreshRuntime();
}

void SequencePage::refreshRuntime() {
    const mc::Sequencer& engine = manager_.sequencer();
    const mc::SequenceStatus& status = engine.status();

    QString line = QStringLiteral("状态：%1").arg(sequencePhaseText(status.phase));
    if (engine.loaded()) {
        line += QStringLiteral("｜程序：%1 v%2")
                    .arg(QString::fromStdString(engine.program().name))
                    .arg(engine.program().version);
        line += QStringLiteral("｜进度：%1/%2 步已确认")
                    .arg(static_cast<int>(status.completedSteps))
                    .arg(static_cast<int>(engine.program().steps.size()));
        if (status.phase == mc::SequencePhase::Running) {
            line += QStringLiteral("｜当前第 %1 步")
                        .arg(static_cast<int>(status.stepIndex) + 1);
        }
    }
    runStatusLabel_->setText(line);
    runStatusLabel_->setStyleSheet(
        QStringLiteral("color:%1;").arg(phaseColor(status.phase).name()));

    const std::vector<mc::StepReport>& reports = engine.reports();
    runTable_->setRowCount(static_cast<int>(reports.size()));
    for (int row = 0; row < static_cast<int>(reports.size()); ++row) {
        const mc::StepReport& report = reports[static_cast<std::size_t>(row)];
        runTable_->setItem(row, 0, new QTableWidgetItem(QString::number(report.index + 1)));
        runTable_->setItem(row, 1,
                           new QTableWidgetItem(sequenceStepKindText(report.kind)));
        auto* outcomeItem = new QTableWidgetItem(stepOutcomeText(report.outcome));
        outcomeItem->setForeground(QBrush(outcomeColor(report.outcome)));
        runTable_->setItem(row, 2, outcomeItem);
        runTable_->setItem(row, 3, new QTableWidgetItem(QString::fromStdString(report.detail)));
    }
    runTable_->resizeColumnsToContents();

    // 详情跟着状态走：跑到哪一步、为什么停，都在这一行里。
    if (status.phase != mc::SequencePhase::Idle && !status.detail.empty()) {
        runDetailLabel_->setText(QStringLiteral("%1（%2）")
                                     .arg(sequencePhaseText(status.phase))
                                     .arg(QString::fromStdString(status.detail)));
    }
    updateButtons();
}

void SequencePage::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    refreshList();
    refreshRuntime();
}

}  // namespace mchost
