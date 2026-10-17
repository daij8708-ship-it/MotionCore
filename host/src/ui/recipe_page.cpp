#include "mc_host/ui/recipe_page.hpp"

#include <QBrush>
#include <QColor>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFont>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

#include "mc/payload.hpp"
#include "mc_host/device.hpp"
#include "mc_host/device_manager.hpp"
#include "mc_host/recipe_store.hpp"
#include "mc_host/recipe_text.hpp"

namespace mchost {
namespace {

constexpr int kListColumns = 9;
constexpr int kDownloadColumns = 3;
constexpr int kDownloadRows = 4;   // 三条命令 + 一格"回读核对"

QString modeText(std::uint8_t mode) {
    switch (mode) {
        case 0U: return QStringLiteral("0 停止控制");
        case 1U: return QStringLiteral("1 速度环");
        case 2U: return QStringLiteral("2 位置环");
        default: return QStringLiteral("%1（规范外）").arg(static_cast<int>(mode));
    }
}

QString checksumText(std::uint16_t value) {
    return QStringLiteral("0x%1").arg(static_cast<uint>(value), 4, 16, QLatin1Char('0')).toUpper();
}

QString maybeNumber(float value) {
    return QString::number(static_cast<double>(value), 'f', 3);
}

QString phaseText(RecipeDownloadPhase phase) {
    switch (phase) {
        case RecipeDownloadPhase::Idle:      return QStringLiteral("尚未下装");
        case RecipeDownloadPhase::Sending:   return QStringLiteral("正在下装");
        case RecipeDownloadPhase::Verifying: return QStringLiteral("正在核对回读值");
        case RecipeDownloadPhase::Succeeded: return QStringLiteral("下装成功");
        case RecipeDownloadPhase::Failed:    return QStringLiteral("下装未完成");
    }
    return QStringLiteral("未知");
}

}  // namespace

RecipePage::RecipePage(DeviceManager& manager, QWidget* parent)
    : QWidget(parent), manager_(manager) {
    auto* root = new QVBoxLayout(this);

    auto* intro = new QLabel(
        QStringLiteral(
            "配方 = 一组具名、带版本、可校验的参数（模式 / 目标 / 三个 PID 增益）。存进配方库，"
            "需要时下装到设备。参数从此不再只活在界面控件里。\n"
            "下装 = 四条命令：SET_MODE → SET_PID → SET_TARGET，再加一条 READ_PARAMS 把设备"
            "**存着的**参数读回来逐字段核对。顺序不是随意的 —— 模式决定目标值的**单位语义**"
            "（deg/s 还是 deg），先发目标值的话，那段时间里它是什么意思说不清。\n"
            "为什么非要有最后那条回读：前三条的确认只说明“设备收到了、也通过了校验”，"
            "不说明“它存下来的就是我们要的那个数”。下装是照一份存档去配置设备，"
            "配置完不核对一遍，就只剩“我相信它”这一种说法。\n"
            "⚠️ 遥测帧里的 target 与回读的 target **不是一个东西**：前者是“此刻在追什么”"
            "（设备没在运行时恒为 0），后者是“被设成了多少”。要核对下装结果，只能看后者。"),
        this);
    intro->setWordWrap(true);
    intro->setStyleSheet(QStringLiteral("color:#444;"));
    root->addWidget(intro);

    // --- 配方库里的清单 ------------------------------------------------------
    auto* listGroup = new QGroupBox(QStringLiteral("配方库"), this);
    auto* listBox = new QVBoxLayout(listGroup);

    storeValue_ = new QLabel(QStringLiteral("配方库：未打开"), this);
    storeValue_->setObjectName(QStringLiteral("recipeStoreValue"));
    storeValue_->setWordWrap(true);
    listBox->addWidget(storeValue_);

    storeErrorLabel_ = new QLabel(this);
    storeErrorLabel_->setObjectName(QStringLiteral("recipeStoreErrorLabel"));
    storeErrorLabel_->setWordWrap(true);
    storeErrorLabel_->setStyleSheet(QStringLiteral("color:#b00020;"));
    listBox->addWidget(storeErrorLabel_);

    table_ = new QTableWidget(0, kListColumns, this);
    table_->setObjectName(QStringLiteral("recipeTable"));
    table_->setHorizontalHeaderLabels({QStringLiteral("名称"), QStringLiteral("版本"),
                                       QStringLiteral("模式"), QStringLiteral("目标"),
                                       QStringLiteral("Kp"), QStringLiteral("Ki"),
                                       QStringLiteral("Kd"), QStringLiteral("校验和"),
                                       QStringLiteral("状态")});
    table_->verticalHeader()->setVisible(false);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->horizontalHeader()->setStretchLastSection(true);
    listBox->addWidget(table_, 1);
    root->addWidget(listGroup, 1);

    // --- 编辑器 ---------------------------------------------------------------
    auto* editGroup = new QGroupBox(QStringLiteral("配方内容（选中上面一行可载入，或点“新建”清空）"),
                                    this);
    auto* grid = new QGridLayout(editGroup);

    grid->addWidget(new QLabel(QStringLiteral("名称"), this), 0, 0);
    nameEdit_ = new QLineEdit(this);
    nameEdit_->setObjectName(QStringLiteral("recipeNameEdit"));
    nameEdit_->setMaxLength(static_cast<int>(mc::kMaxRecipeNameBytes));
    grid->addWidget(nameEdit_, 0, 1);

    grid->addWidget(new QLabel(QStringLiteral("版本"), this), 0, 2);
    versionSpin_ = new QSpinBox(this);
    versionSpin_->setObjectName(QStringLiteral("recipeVersionSpin"));
    // 下限是 1 而不是 0：0 在核心判定里就不是合法版本（它只可能表示"没填"），
    // 让界面能造出一个必然被拒的值，只是把错误推给下一个人。
    versionSpin_->setRange(1, 65535);
    grid->addWidget(versionSpin_, 0, 3);

    grid->addWidget(new QLabel(QStringLiteral("模式"), this), 1, 0);
    modeCombo_ = new QComboBox(this);
    modeCombo_->setObjectName(QStringLiteral("recipeModeCombo"));
    modeCombo_->addItem(QStringLiteral("0 停止控制"), 0);
    modeCombo_->addItem(QStringLiteral("1 速度环"), 1);
    modeCombo_->addItem(QStringLiteral("2 位置环"), 2);
    grid->addWidget(modeCombo_, 1, 1);

    grid->addWidget(new QLabel(QStringLiteral("目标值"), this), 1, 2);
    targetSpin_ = new QDoubleSpinBox(this);
    targetSpin_->setObjectName(QStringLiteral("recipeTargetSpin"));
    targetSpin_->setDecimals(3);
    targetSpin_->setSingleStep(10.0);
    grid->addWidget(targetSpin_, 1, 3);

    grid->addWidget(new QLabel(QStringLiteral("Kp"), this), 2, 0);
    kpSpin_ = new QDoubleSpinBox(this);
    kpSpin_->setObjectName(QStringLiteral("recipeKpSpin"));
    kpSpin_->setRange(0.0, static_cast<double>(mc::kSanityMaxPidGain));
    kpSpin_->setDecimals(3);
    kpSpin_->setSingleStep(0.05);
    grid->addWidget(kpSpin_, 2, 1);

    grid->addWidget(new QLabel(QStringLiteral("Ki"), this), 2, 2);
    kiSpin_ = new QDoubleSpinBox(this);
    kiSpin_->setObjectName(QStringLiteral("recipeKiSpin"));
    kiSpin_->setRange(0.0, static_cast<double>(mc::kSanityMaxPidGain));
    kiSpin_->setDecimals(3);
    kiSpin_->setSingleStep(0.05);
    grid->addWidget(kiSpin_, 2, 3);

    grid->addWidget(new QLabel(QStringLiteral("Kd"), this), 3, 0);
    kdSpin_ = new QDoubleSpinBox(this);
    kdSpin_->setObjectName(QStringLiteral("recipeKdSpin"));
    kdSpin_->setRange(0.0, static_cast<double>(mc::kSanityMaxPidGain));
    kdSpin_->setDecimals(3);
    kdSpin_->setSingleStep(0.05);
    grid->addWidget(kdSpin_, 3, 1);

    newButton_ = new QPushButton(QStringLiteral("新建"), this);
    newButton_->setObjectName(QStringLiteral("recipeNewButton"));
    saveButton_ = new QPushButton(QStringLiteral("保存到配方库"), this);
    saveButton_->setObjectName(QStringLiteral("recipeSaveButton"));
    deleteButton_ = new QPushButton(QStringLiteral("删除选中配方"), this);
    deleteButton_->setObjectName(QStringLiteral("recipeDeleteButton"));
    grid->addWidget(newButton_, 4, 0);
    grid->addWidget(saveButton_, 4, 1);
    grid->addWidget(deleteButton_, 4, 2);

    validationLabel_ = new QLabel(this);
    validationLabel_->setObjectName(QStringLiteral("recipeValidationLabel"));
    validationLabel_->setWordWrap(true);
    QFont validationFont = validationLabel_->font();
    validationFont.setBold(true);
    validationLabel_->setFont(validationFont);
    grid->addWidget(validationLabel_, 5, 0, 1, 4);

    resultLabel_ = new QLabel(this);
    resultLabel_->setObjectName(QStringLiteral("recipeResultLabel"));
    resultLabel_->setWordWrap(true);
    grid->addWidget(resultLabel_, 6, 0, 1, 4);

    root->addWidget(editGroup);

    // --- 下装 -----------------------------------------------------------------
    auto* downloadGroup = new QGroupBox(QStringLiteral("下装到设备"), this);
    auto* downloadBox = new QVBoxLayout(downloadGroup);

    auto* buttonRow = new QHBoxLayout();
    downloadButton_ = new QPushButton(QStringLiteral("下装到设备"), this);
    downloadButton_->setObjectName(QStringLiteral("recipeDownloadButton"));
    cancelButton_ = new QPushButton(QStringLiteral("中止下装"), this);
    cancelButton_->setObjectName(QStringLiteral("recipeCancelButton"));
    buttonRow->addWidget(downloadButton_);
    buttonRow->addWidget(cancelButton_);
    buttonRow->addStretch(1);
    downloadBox->addLayout(buttonRow);

    downloadStatusLabel_ = new QLabel(this);
    downloadStatusLabel_->setObjectName(QStringLiteral("recipeDownloadLabel"));
    downloadStatusLabel_->setWordWrap(true);
    downloadBox->addWidget(downloadStatusLabel_);

    downloadTable_ = new QTableWidget(kDownloadRows, kDownloadColumns, this);
    downloadTable_->setObjectName(QStringLiteral("recipeDownloadTable"));
    downloadTable_->setHorizontalHeaderLabels({QStringLiteral("步骤"), QStringLiteral("动作"),
                                               QStringLiteral("结果")});
    downloadTable_->verticalHeader()->setVisible(false);
    downloadTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    downloadTable_->horizontalHeader()->setStretchLastSection(true);
    downloadTable_->setItem(0, 0, new QTableWidgetItem(QStringLiteral("1/3")));
    downloadTable_->setItem(0, 1, new QTableWidgetItem(QStringLiteral("SET_MODE")));
    downloadTable_->setItem(1, 0, new QTableWidgetItem(QStringLiteral("2/3")));
    downloadTable_->setItem(1, 1, new QTableWidgetItem(QStringLiteral("SET_PID")));
    downloadTable_->setItem(2, 0, new QTableWidgetItem(QStringLiteral("3/3")));
    downloadTable_->setItem(2, 1, new QTableWidgetItem(QStringLiteral("SET_TARGET")));
    downloadTable_->setItem(3, 0, new QTableWidgetItem(QStringLiteral("核对")));
    downloadTable_->setItem(3, 1,
                            new QTableWidgetItem(QStringLiteral("READ_PARAMS 逐字段核对")));
    downloadTable_->resizeColumnsToContents();
    downloadBox->addWidget(downloadTable_);

    root->addWidget(downloadGroup);

    // --- 接线 -----------------------------------------------------------------
    connect(table_, &QTableWidget::itemSelectionChanged, this, &RecipePage::onSelectionChanged);
    connect(newButton_, &QPushButton::clicked, this, &RecipePage::onNewRecipe);
    connect(saveButton_, &QPushButton::clicked, this, &RecipePage::saveRecipe);
    connect(deleteButton_, &QPushButton::clicked, this, &RecipePage::deleteRecipe);
    connect(targetSpin_, &QDoubleSpinBox::valueChanged, this, [this]() { validateEditor(); });
    connect(kpSpin_, &QDoubleSpinBox::valueChanged, this, [this]() { validateEditor(); });
    connect(kiSpin_, &QDoubleSpinBox::valueChanged, this, [this]() { validateEditor(); });
    connect(kdSpin_, &QDoubleSpinBox::valueChanged, this, [this]() { validateEditor(); });
    connect(versionSpin_, &QSpinBox::valueChanged, this, [this]() { validateEditor(); });
    connect(nameEdit_, &QLineEdit::textChanged, this, [this]() { validateEditor(); });
    connect(modeCombo_, &QComboBox::currentIndexChanged, this, [this]() {
        // 先按模式定量程，再校验：量程不跟着模式走的话，位置环里根本填不进
        // 一个像样的角度值（换个模式就白填了）。
        applyModeToTargetRange();
        validateEditor();
    });
    connect(downloadButton_, &QPushButton::clicked, this, &RecipePage::downloadRecipe);
    connect(cancelButton_, &QPushButton::clicked, this, &RecipePage::cancelDownload);
    connect(&manager_, &DeviceManager::recipeDownloadChanged, this,
            [this]() { refreshRuntimeState(); });
    connect(&manager_, &DeviceManager::statsRefreshed, this,
            [this]() { refreshRuntimeState(); });
    connect(&manager_, &DeviceManager::connectionChanged, this,
            [this]() { refreshRuntimeState(); });

    onNewRecipe();
    refreshList();
    refreshRuntimeState();
}

void RecipePage::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    // 切到本页时重读一次库：条数与内容必须以**现在的库**为准，
    // 而不是上次离开时那一刻的快照。
    refreshList();
    refreshRuntimeState();
}

mc::Recipe RecipePage::editorRecipe() const {
    mc::Recipe recipe;
    recipe.name = nameEdit_->text().toStdString();
    recipe.version = static_cast<std::uint16_t>(versionSpin_->value());
    recipe.mode = static_cast<std::uint8_t>(modeCombo_->currentData().toInt());
    recipe.target = static_cast<float>(targetSpin_->value());
    recipe.kp = static_cast<float>(kpSpin_->value());
    recipe.ki = static_cast<float>(kiSpin_->value());
    recipe.kd = static_cast<float>(kdSpin_->value());
    return recipe;
}

void RecipePage::loadIntoEditor(const mc::Recipe& recipe) {
    nameEdit_->setText(QString::fromStdString(recipe.name));
    versionSpin_->setValue(static_cast<int>(recipe.version));
    const int index = modeCombo_->findData(static_cast<int>(recipe.mode));
    modeCombo_->setCurrentIndex(index >= 0 ? index : 0);
    // 顺序要紧：先把量程按模式调好，再填目标值。反过来的话，
    // 一个位置环的角度值会先被夹进速度环的量程里，再也回不来。
    applyModeToTargetRange();
    targetSpin_->setValue(static_cast<double>(recipe.target));
    kpSpin_->setValue(static_cast<double>(recipe.kp));
    kiSpin_->setValue(static_cast<double>(recipe.ki));
    kdSpin_->setValue(static_cast<double>(recipe.kd));
    validateEditor();
}

void RecipePage::applyModeToTargetRange() {
    const int mode = modeCombo_->currentData().toInt();
    // 量程取自协议里那两条既有的量纲上界，不另立一套。模式 0（停止控制）
    // 沿用速度环的量程：它的目标是"必须为 0"，但让界面**能**填进一个非零值，
    // 校验面板才会把这条规则显示出来 —— 直接禁掉输入框，规则就没人看得见了。
    const double bound = (mode == 2) ? static_cast<double>(mc::kSanityMaxAbsPositionDeg)
                                     : static_cast<double>(mc::kSanityMaxAbsVelocityDegPerSec);
    targetSpin_->setRange(-bound, bound);
}

void RecipePage::validateEditor() {
    const mc::Recipe recipe = editorRecipe();
    const mc::RecipeError verdict = mc::validateRecipe(recipe);
    if (verdict == mc::RecipeError::None) {
        validationLabel_->setText(QStringLiteral("校验通过（校验和 %1）").arg(checksumText(mc::recipeChecksum(recipe))));
        validationLabel_->setStyleSheet(QStringLiteral("color:#1b5e20;"));
    } else {
        validationLabel_->setText(QStringLiteral("不合法：%1").arg(recipeErrorText(verdict)));
        validationLabel_->setStyleSheet(QStringLiteral("color:#b00020;"));
    }
    updateDownloadButton();
}

void RecipePage::updateDownloadButton() {
    const IDevice* device = manager_.device();
    const bool ready = manager_.isConnected() && device != nullptr && device->sessionReady();
    const bool valid = mc::validateRecipe(editorRecipe()) == mc::RecipeError::None;

    downloadButton_->setEnabled(ready && valid);
    if (!ready) {
        downloadButton_->setToolTip(QStringLiteral("会话未就绪：握手完成前发的命令到不了设备"));
    } else if (!valid) {
        downloadButton_->setToolTip(QStringLiteral("配方没通过校验：先把它改对"));
    } else {
        // 设备不在 Ready 时照样可以点 —— 让应用服务层把理由说出来。
        // 把按钮灰掉而不说明，操作员只会看到"点不动"。
        downloadButton_->setToolTip(QStringLiteral("设备必须停在 Ready：SET_MODE 会改变目标值的单位语义"));
    }
}

void RecipePage::onSelectionChanged() {
    // 选择被整体清掉（新建 / 删除）时什么都不做。少了这一句，"点新建"会被
    // 那次清选择触发的信号把上一行重新载入编辑器，而 selectedId_ 也跟着被改回去 ——
    // 表现就是"新建之后保存，结果更新了旧配方"。
    if (table_->selectedItems().isEmpty()) {
        return;
    }
    const int row = table_->currentRow();
    if (row < 0) {
        return;
    }
    const QTableWidgetItem* nameItem = table_->item(row, 0);
    if (nameItem == nullptr) {
        return;
    }
    const qint64 id = nameItem->data(Qt::UserRole).toLongLong();
    if (id == selectedId_) {
        return;
    }

    RecipeStore* store = manager_.recipes();
    if (store == nullptr || !store->isOpen()) {
        return;
    }
    QString error;
    const std::vector<RecipeStore::Row> rows = store->list(error);
    for (const RecipeStore::Row& candidate : rows) {
        if (candidate.id == id) {
            selectedId_ = id;
            deleteArmed_ = false;
            loadIntoEditor(candidate.recipe);
            resultLabel_->setText(
                candidate.corrupted
                    ? QStringLiteral("⚠ 这一行的校验和与内容不符：它可能被改动过或写坏了。"
                                     "下装之前请重新核对参数。")
                    : QString());
            return;
        }
    }
}

void RecipePage::refreshList() {
    RecipeStore* store = manager_.recipes();
    {
        const QSignalBlocker blocker(table_);
        table_->setRowCount(0);

        if (store == nullptr || !store->isOpen()) {
            storeValue_->setText(QStringLiteral("配方库：未打开"));
            storeErrorLabel_->setText(store != nullptr ? store->error()
                                                       : QStringLiteral("没有配方库"));
            return;
        }

        QString error;
        const std::vector<RecipeStore::Row> rows = store->list(error);
        if (!error.isEmpty()) {
            storeValue_->setText(QStringLiteral("配方库：读取失败"));
            storeErrorLabel_->setText(error);
            return;
        }
        storeErrorLabel_->clear();
        storeValue_->setText(QStringLiteral("配方库：%1 条配方　%2")
                                 .arg(static_cast<int>(rows.size()))
                                 .arg(store->path()));

        table_->setRowCount(static_cast<int>(rows.size()));
        int rowToSelect = -1;
        for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
            const RecipeStore::Row& row = rows[i];
            auto* nameItem = new QTableWidgetItem(QString::fromStdString(row.recipe.name));
            nameItem->setData(Qt::UserRole, static_cast<qlonglong>(row.id));
            table_->setItem(i, 0, nameItem);
            table_->setItem(i, 1, new QTableWidgetItem(QString::number(row.recipe.version)));
            table_->setItem(i, 2, new QTableWidgetItem(modeText(row.recipe.mode)));
            table_->setItem(i, 3, new QTableWidgetItem(maybeNumber(row.recipe.target)));
            table_->setItem(i, 4, new QTableWidgetItem(maybeNumber(row.recipe.kp)));
            table_->setItem(i, 5, new QTableWidgetItem(maybeNumber(row.recipe.ki)));
            table_->setItem(i, 6, new QTableWidgetItem(maybeNumber(row.recipe.kd)));
            table_->setItem(i, 7, new QTableWidgetItem(checksumText(row.storedChecksum)));

            auto* stateItem = new QTableWidgetItem(
                row.corrupted ? QStringLiteral("校验和不符（可能被改动过）") : QStringLiteral("正常"));
            if (row.corrupted) {
                stateItem->setForeground(QColor(QStringLiteral("#b00020")));
            }
            table_->setItem(i, 8, stateItem);

            if (row.id == selectedId_) {
                rowToSelect = i;
            }
        }
        if (rowToSelect >= 0) {
            table_->setCurrentCell(rowToSelect, 0);
        } else if (selectedId_ != 0) {
            // 选中的那条已经不在库里了（被删掉、或换了个库文件）——
            // 让"未选中"重新成为一个明确状态，而不是留着一个悬空的 id。
            selectedId_ = 0;
        }
    }
    table_->resizeColumnsToContents();
}

void RecipePage::onNewRecipe() {
    selectedId_ = 0;
    deleteArmed_ = false;
    table_->clearSelection();

    mc::Recipe fresh;
    fresh.mode = 0U;
    // 名称留空、增益留 0：一上来就摆一份"还没填完"的配方，校验面板会立刻
    // 指出缺什么。给一个像模像样的默认值反而更容易让人不去核对它。
    loadIntoEditor(fresh);
    resultLabel_->setText(QStringLiteral("编辑器已清空。填好名称与参数后点「保存到配方库」。"));
}

void RecipePage::saveRecipe() {
    // 写库走管理层的入口，而不是直接拿 store：配方是"这台设备按什么参数干活"的定义，
    // 谁改的、什么时候改的必须查得出来 —— 权限与审计只有那一处。
    const bool updating = selectedId_ > 0;
    const mc::Recipe recipe = editorRecipe();
    QString error;
    if (!manager_.saveRecipe(recipe, &selectedId_, error)) {
        resultLabel_->setText(QStringLiteral("没保存：%1").arg(error));
        return;
    }
    resultLabel_->setText(updating
                              ? QStringLiteral("已更新「%1」v%2。")
                                    .arg(QString::fromStdString(recipe.name))
                                    .arg(recipe.version)
                              : QStringLiteral("已存入配方库（id=%1）。").arg(selectedId_));
    deleteArmed_ = false;
    refreshList();
}

void RecipePage::deleteRecipe() {
    const int row = table_->currentRow();
    if (selectedId_ <= 0 || row < 0) {
        resultLabel_->setText(QStringLiteral("先在上面的清单里选中一条配方。"));
        return;
    }

    if (!deleteArmed_) {
        // 第一次点击只是"预备"。删除不可逆，而弹出的确认框在无头环境里没人点 ——
        // 与其做一个测不到的确认框，不如做一个看得见的二次确认。
        const QTableWidgetItem* item = table_->item(row, 0);
        deleteArmed_ = true;
        resultLabel_->setText(
            QStringLiteral("再点一次「删除选中配方」确认删除「%1」。配方库里的东西删了就没了。")
                .arg(item != nullptr ? item->text() : QString()));
        return;
    }

    QString error;
    if (!manager_.deleteRecipe(selectedId_, error)) {
        resultLabel_->setText(QStringLiteral("没删除：%1").arg(error));
        return;
    }
    resultLabel_->setText(QStringLiteral("已从配方库删除。编辑器里的内容留着 —— 存错了可以再存回去。"));
    selectedId_ = 0;
    deleteArmed_ = false;
    refreshList();
}

void RecipePage::downloadRecipe() {
    // 本地校验由按钮的可用性把关，但这里**照样调** sendRecipe：它是唯一的入口，
    // 门禁在那儿只有一份。返回 false（没开始）时原因已经写进了状态里。
    const bool started = manager_.sendRecipe(editorRecipe());
    refreshRuntimeState();
    if (!started) {
        // 下装区的说明里已经有**具体**原因（联锁、状态、没权限、配方不合法……），
        // 这里把同一句话再摆在上面的结果标签上：两块内容挨着看才知道该去哪儿处理。
        resultLabel_->setText(QStringLiteral("这次下装没有开始：%1")
                                  .arg(manager_.lastRefusalText()));
    }
}

void RecipePage::cancelDownload() {
    manager_.cancelRecipeDownload();
    refreshRuntimeState();
}

void RecipePage::refreshRuntimeState() {
    updateDownloadButton();

    const RecipeDownloadStatus& status = manager_.recipeDownload();
    downloadStatusLabel_->setText(QStringLiteral("阶段：%1　已确认 %2/3\n%3")
                                      .arg(phaseText(status.phase))
                                      .arg(status.commandsAcked)
                                      .arg(status.detail));

    const auto setResult = [this](int row, const QString& text, bool bad) {
        auto* item = downloadTable_->item(row, 2);
        if (item == nullptr) {
            item = new QTableWidgetItem();
            downloadTable_->setItem(row, 2, item);
        }
        item->setText(text);
        // 空 QBrush 是"不设颜色"，回到默认前景色 —— 不能拿 QColor() 去凑，
        // 那是一个无效颜色，在不同平台上的表现并不一致。
        item->setForeground(bad ? QBrush(QColor(QStringLiteral("#b00020"))) : QBrush());
    };

    for (int i = 0; i < 3; ++i) {
        QString text;
        bool bad = false;
        if (status.phase == RecipeDownloadPhase::Idle) {
            text = QStringLiteral("—");
        } else if (i < status.commandsAcked) {
            text = QStringLiteral("已确认");
        } else if (i == status.commandsAcked) {
            if (status.phase == RecipeDownloadPhase::Sending) {
                text = QStringLiteral("等确认…");
            } else if (status.phase == RecipeDownloadPhase::Failed) {
                text = QStringLiteral("未完成");
                bad = true;
            } else {
                text = QStringLiteral("已确认");
            }
        } else {
            text = QStringLiteral("未下发");
        }
        setResult(i, text, bad);
    }

    // 第 4 行是"逐字段核对"：它拿的是设备**存着的**参数，不是遥测里的"此刻在追什么"。
    if (status.phase == RecipeDownloadPhase::Idle) {
        setResult(3, QStringLiteral("—"), false);
    } else if (status.phase == RecipeDownloadPhase::Verifying) {
        setResult(3, QStringLiteral("等设备回读…"), false);
    } else if (status.phase == RecipeDownloadPhase::Succeeded) {
        setResult(3, QStringLiteral("逐字段一致"), false);
    } else if (status.phase == RecipeDownloadPhase::Failed) {
        setResult(3, status.commandsAcked >= 3 ? QStringLiteral("未核对上") : QStringLiteral("未开始"),
                  true);
    } else {
        setResult(3, QStringLiteral("未开始"), false);
    }

    const bool active = (status.phase == RecipeDownloadPhase::Sending ||
                         status.phase == RecipeDownloadPhase::Verifying);
    cancelButton_->setEnabled(active);
}

}  // namespace mchost
