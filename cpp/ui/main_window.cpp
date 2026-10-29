#include "main_window.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <map>
#include <numeric>

// Referencing the resource initializer also retains the .qrc object when the UI
// is linked as a static library into the application and Qt Test executable.
static void initUiResources() {
    Q_INIT_RESOURCE(ui);
}

namespace xm::ui {
namespace {
QString q(const std::string &value) {
    return QString::fromUtf8(value.data(), static_cast<int>(value.size()));
}
std::string s(const QString &value) {
    return value.toUtf8().toStdString();
}
QStringList qs(const std::vector<std::string> &values) {
    QStringList result;
    for (const auto &value : values)
        result.push_back(q(value));
    return result;
}
std::vector<std::string> ss(const QStringList &values) {
    std::vector<std::string> result;
    for (const auto &value : values)
        result.push_back(s(value));
    return result;
}
QString startTime(const SuiteCandidate &candidate) {
    auto value = q(candidate.suite_starttime).trimmed();
    // RF's fractional seconds can have 1-6 digits. Display seconds only, as in
    // the Python interface, without turning end time into a start-time fallback.
    auto match = QRegularExpression("^(\\d{8} \\d{2}:\\d{2}:\\d{2})(?:\\.\\d{1,6})?$").match(value);
    if (!match.hasMatch())
        return QStringLiteral("—");
    auto date = QDateTime::fromString(match.captured(1), "yyyyMMdd HH:mm:ss");
    return date.isValid() ? date.toString("yyyy-MM-dd HH:mm:ss") : QStringLiteral("—");
}
QLabel *label(const QString &text, const QString &name = {}) {
    auto *item = new QLabel(text);
    item->setObjectName(name);
    return item;
}
QPushButton *button(const QString &text, const QString &name = {}) {
    auto *item = new QPushButton(text);
    item->setObjectName(name);
    return item;
}
QFrame *frame(const QString &name) {
    auto *item = new QFrame;
    item->setObjectName(name);
    return item;
}
QComboBox *combo(const QString &name, const QList<QPair<QString, QString>> &choices) {
    auto *item = new QComboBox;
    item->setObjectName(name);
    for (const auto &choice : choices)
        item->addItem(choice.first, choice.second);
    return item;
}
QString statusColor(const QString &status) {
    if (status == "PASS")
        return "#0f8f4f";
    if (status == "SKIP")
        return "#b06f00";
    if (status == "FAIL")
        return "#c62828";
    return "#394150";
}
// Match os.path.splitext: a leading dot is part of a hidden filename, while a
// trailing dot is an existing extension. QFileInfo::suffix differs for both.
int extensionOffset(const QString &path) {
    const auto name = QFileInfo(path).fileName();
    const int dot = name.lastIndexOf('.');
    if (dot < 0)
        return -1;
    for (int index = 0; index < dot; ++index)
        if (name[index] != '.')
            return path.size() - name.size() + dot;
    return -1;
}
} // namespace
MainWindow::MainWindow(QWidget *parent, Loader loader, const QString &settingsFile)
    : QMainWindow(parent),
      settings_(settingsFile.isEmpty()
                    ? std::make_unique<QSettings>("Robotframework-XMLMergeTool", "MainWindow")
                    : std::make_unique<QSettings>(settingsFile, QSettings::IniFormat)),
      loader_(std::move(loader)), outputDir_(QDir::currentPath()) {
    initUiResources();
    buildUi();
    connect(&watcher_, &QFutureWatcher<JobResult>::finished, this, &MainWindow::jobFinished);
    auto stored = settings_->value("candidate/view_mode", "flat").toString();
    mode_ = stored == "tree" ? stored : "flat";
    {
        QSignalBlocker block(viewMode_);
        viewMode_->setCurrentIndex(viewMode_->findData(mode_));
    }
    rebuildModel();
    loadHeader();
    setOutputDir(outputDir_);
    refreshDetail();
}
MainWindow::~MainWindow() {
    // Jobs own value snapshots, never this or widgets. Joining is safe even if a
    // caller deletes the window directly instead of delivering closeEvent.
    disconnect(&watcher_, nullptr, this, nullptr);
    watcher_.waitForFinished();
}
void MainWindow::buildUi() {
    setWindowTitle("Robot XML Merge Tool");
    setWindowIcon(QIcon(":/ui/logo.ico"));
    QApplication::setWindowIcon(windowIcon());
    resize(1320, 800);
    QFile theme(":/ui/theme.qss");
    if (theme.open(QIODevice::ReadOnly))
        setStyleSheet(QString::fromUtf8(theme.readAll()));
    auto *open = button("打开 XML 文件", "OpenFilesButton");
    auto *restore = button("恢复默认勾选", "RestoreDefaultButton");
    auto *clear = button("清空列表", "DangerButton");
    auto *excel = button("导出勾选 Excel", "ExportExcelButton");
    auto *merge = button("合并勾选结果", "PrimaryButton");
    auto *folder = button("设置输出目录", "SelectFolderButton");
    auto *options = button("高级选项", "ReportOptionsButton");
    search_ = new QLineEdit;
    search_->setObjectName("SearchEdit");
    search_->setPlaceholderText("搜索：文件名 / 套件路径");
    search_->setClearButtonEnabled(true);
    search_->setMinimumWidth(320);
    conflictOnly_ = new QCheckBox("只看冲突");
    conflictOnly_->setObjectName("ConflictOnlyCheckBox");
    selectedOnly_ = new QCheckBox("只看已勾选");
    selectedOnly_->setObjectName("SelectedOnlyCheckBox");
    compact_ = new QCheckBox("紧凑模式");
    compact_->setObjectName("CompactCheckBox");
    viewMode_ = combo("ViewModeCombo", {{"平铺表格", "flat"}, {"路径树", "tree"}});
    viewMode_->setToolTip("切换候选区域的展示方式。");
    statusFilter_ =
        combo("StatusFilterCombo",
              {{"全部状态", "ALL"}, {"PASS", "PASS"}, {"SKIP", "SKIP"}, {"FAIL", "FAIL"}});
    summary_ = label("未加载 XML 文件", "SummaryLabel");
    visibleCount_ = label("可见: 0", "MutedText");
    progress_ = new QProgressBar;
    progress_->setFixedWidth(180);
    progress_->hide();
    outputLabel_ = label({}, "OutputDirLabel");
    outputLabel_->setCursor(Qt::PointingHandCursor);
    outputLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    outputLabel_->setMinimumWidth(160);
    outputLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    outputLabel_->installEventFilter(this);
    treeView = new CandidateView;
    treeView->setObjectName("CandidateView");
    treeView->setProperty("compact", false);
    model = new QStandardItemModel(0, ColumnCount, this);
    model->setHorizontalHeaderLabels(
        {"勾选", "测试套件路径", "状态", "默认推荐", "冲突组大小", "开始时间", "文件"});
    proxy = new CandidateFilter(this);
    proxy->setSourceModel(model);
    treeView->setModel(proxy);
    auto *header = treeView->header();
    header->setStretchLastSection(false);
    header->setSectionsMovable(true);
    header->setSectionResizeMode(QHeaderView::Interactive);
    header->setContextMenuPolicy(Qt::CustomContextMenu);
    buildDialogs();

    auto *commandCard = frame("CommandCard");
    auto *commands = new QVBoxLayout(commandCard);
    commands->setContentsMargins(12, 10, 12, 10);
    commands->setSpacing(8);
    auto *primary = new QHBoxLayout;
    primary->setSpacing(10);
    auto *logo = label({}, "HeroLogo");
    logo->setFixedSize(36, 36);
    logo->setPixmap(windowIcon().pixmap(26, 26));
    logo->setAlignment(Qt::AlignCenter);
    primary->addWidget(logo);
    auto *titleColumn = new QVBoxLayout;
    titleColumn->setSpacing(2);
    titleColumn->addWidget(label("Robot XML Merge Tool", "HeroTitle"));
    titleColumn->addWidget(label("Merge & Resolve Robot Output Suites", "HeroSubtitle"));
    primary->addLayout(titleColumn, 1);
    auto *headerTools = new QWidget;
    headerTools->setObjectName("HeaderToolGroup");
    auto *toolLayout = new QHBoxLayout(headerTools);
    toolLayout->setContentsMargins(0, 0, 0, 0);
    for (auto *item : {open, restore, clear})
        toolLayout->addWidget(item);
    primary->addWidget(headerTools);
    primary->addWidget(excel);
    primary->addWidget(merge);
    commands->addLayout(primary);
    auto *secondary = new QHBoxLayout;
    secondary->addWidget(search_, 1);
    auto *utilities = frame("ToolbarGroup");
    auto *utilityLayout = new QHBoxLayout(utilities);
    utilityLayout->setContentsMargins(8, 6, 8, 6);
    utilityLayout->addWidget(folder);
    utilityLayout->addWidget(options);
    secondary->addWidget(utilities);
    commands->addLayout(secondary);
    auto *workspace = frame("WorkspaceCard");
    auto *workspaceLayout = new QVBoxLayout(workspace);
    workspaceLayout->setContentsMargins(10, 10, 10, 10);
    workspaceLayout->setSpacing(8);
    auto *statusRow = new QHBoxLayout;
    statusRow->addWidget(summary_, 1);
    statusRow->addWidget(visibleCount_);
    statusRow->addWidget(progress_);
    workspaceLayout->addLayout(statusRow);
    workspaceLayout->addWidget(treeView, 1);
    auto *footer = frame("FooterFilterGroup");
    auto *footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(10, 6, 10, 6);
    footerLayout->setSpacing(10);
    footerLayout->addWidget(conflictOnly_);
    footerLayout->addWidget(selectedOnly_);
    footerLayout->addSpacing(10);
    footerLayout->addWidget(label("状态筛选", "FieldLabel"));
    footerLayout->addWidget(statusFilter_);
    footerLayout->addWidget(label("展示", "FieldLabel"));
    footerLayout->addWidget(viewMode_);
    footerLayout->addWidget(compact_);
    footerLayout->addSpacing(10);
    footerLayout->addStretch();
    footerLayout->addWidget(outputLabel_, 1);
    workspaceLayout->addWidget(footer);
    auto *container = new QWidget;
    auto *layout = new QVBoxLayout(container);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(8);
    layout->addWidget(commandCard);
    layout->addWidget(workspace, 1);
    setCentralWidget(container);
    actionWidgets_ += {open,     restore,   clear,         excel,         merge,
                       folder,   options,   search_,       conflictOnly_, selectedOnly_,
                       compact_, viewMode_, statusFilter_, outputLabel_,  treeView};

    connect(open, &QPushButton::clicked, this, [this] {
        auto paths = QFileDialog::getOpenFileNames(this, "Open XML Files", {},
                                                   "XML Files (*.xml);;All Files (*)");
        if (!paths.isEmpty())
            loadFiles(paths);
    });
    connect(restore, &QPushButton::clicked, this, &MainWindow::restoreDefaultSelection);
    connect(clear, &QPushButton::clicked, this, &MainWindow::clearCandidates);
    connect(excel, &QPushButton::clicked, this, &MainWindow::chooseExcelPath);
    connect(merge, &QPushButton::clicked, this, &MainWindow::mergeSelected);
    connect(folder, &QPushButton::clicked, this, &MainWindow::chooseOutputDir);
    connect(options, &QPushButton::clicked, this,
            [this] { showDialog(optionsDialog_, "report_options/dialog_geometry", 72); });
    connect(search_, &QLineEdit::textChanged, this, [this] { refreshFilter(); });
    connect(conflictOnly_, &QCheckBox::toggled, this, [this] { refreshFilter(); });
    connect(selectedOnly_, &QCheckBox::toggled, this, [this] { refreshFilter(); });
    connect(statusFilter_, &QComboBox::currentIndexChanged, this, [this] { refreshFilter(); });
    connect(viewMode_, &QComboBox::currentIndexChanged, this,
            [this] { setViewMode(viewMode_->currentData().toString()); });
    connect(compact_, &QCheckBox::toggled, this, [this](bool enabled) {
        treeView->setProperty("compact", enabled);
        treeView->style()->unpolish(treeView);
        treeView->style()->polish(treeView);
        treeView->doItemsLayout();
        treeView->viewport()->update();
    });
    connect(treeView, &CandidateView::filesDropped, this, &MainWindow::loadFiles);
    connect(model, &QStandardItemModel::itemChanged, this, &MainWindow::itemChanged);
    connect(treeView->selectionModel(), &QItemSelectionModel::currentChanged, this, [this] {
        if (!rebuilding_)
            refreshDetail();
    });
    connect(treeView, &QTreeView::doubleClicked, this, &MainWindow::handleDoubleClick);
    connect(this, &MainWindow::candidateDoubleClicked, this, &MainWindow::openSingleLog);
    connect(treeView, &QWidget::customContextMenuRequested, this, [this](const QPoint &position) {
        const auto clicked =
            treeView->indexAt(position).siblingAtColumn(Check).data(CandidateIdRole).toString();
        auto *menu = buildContextMenu(contextSelection(position), clicked);
        menu->exec(treeView->viewport()->mapToGlobal(position));
        delete menu;
    });
    connect(header, &QHeaderView::sectionResized, this, [this] {
        if (!restoringHeader_)
            saveHeader();
    });
    connect(header, &QHeaderView::sectionMoved, this, [this] {
        if (!restoringHeader_)
            saveHeader();
    });
    connect(
        header, &QWidget::customContextMenuRequested, this, [this, header](const QPoint &position) {
            QMenu menu(header);
            for (int col = 1; col < ColumnCount; ++col) {
                auto *action = menu.addAction(model->headerData(col, Qt::Horizontal).toString());
                action->setCheckable(true);
                action->setChecked(!treeView->isColumnHidden(col));
                action->setEnabled(mode_ != "tree" || col != SuitePath);
                connect(action, &QAction::toggled, this,
                        [this, col](bool value) { setColumnVisible(col, value); });
            }
            menu.exec(header->mapToGlobal(position));
        });
}
void MainWindow::buildDialogs() {
    detailDialog_ = new QDialog(this);
    detailDialog_->setObjectName("DetailDialog");
    detailDialog_->setWindowTitle("候选项详情");
    detailDialog_->resize(520, 580);
    detailDialog_->installEventFilter(this);
    auto *detailLayout = new QVBoxLayout(detailDialog_);
    detailLayout->setContentsMargins(12, 12, 12, 12);
    auto *panel = frame("DetailPanel");
    auto *panelLayout = new QVBoxLayout(panel);
    auto *form = new QFormLayout;
    auto detailLabel = [] {
        auto *item = label("—");
        item->setWordWrap(true);
        item->setTextInteractionFlags(Qt::TextSelectableByMouse);
        return item;
    };
    detailSelected_ = detailLabel();
    detailDefault_ = detailLabel();
    detailPath_ = detailLabel();
    detailFile_ = detailLabel();
    detailFilePath_ = detailLabel();
    detailStart_ = detailLabel();
    detailStatus_ = label("UNKNOWN", "StatusBadge");
    detailTests_ = new QPlainTextEdit;
    detailTests_->setReadOnly(true);
    detailTests_->setMinimumHeight(56);
    detailTests_->setMaximumHeight(100);
    detailFailures_ = new QPlainTextEdit;
    detailFailures_->setReadOnly(true);
    detailFailures_->setMinimumHeight(56);
    detailFailures_->setMaximumHeight(140);
    form->addRow("当前勾选", detailSelected_);
    form->addRow("默认推荐", detailDefault_);
    form->addRow("状态", detailStatus_);
    form->addRow("套件路径", detailPath_);
    form->addRow("文件名", detailFile_);
    form->addRow("文件路径", detailFilePath_);
    form->addRow("测试名称", detailTests_);
    form->addRow("失败原因", detailFailures_);
    form->addRow("开始时间", detailStart_);
    panelLayout->addLayout(form);
    panelLayout->addStretch();
    detailLayout->addWidget(panel);

    optionsDialog_ = new QDialog(this);
    optionsDialog_->setObjectName("ReportOptionsDialog");
    optionsDialog_->setWindowTitle("高级输出选项");
    optionsDialog_->resize(700, 420);
    optionsDialog_->installEventFilter(this);
    auto *optionsLayout = new QVBoxLayout(optionsDialog_);
    optionsLayout->setContentsMargins(12, 12, 12, 12);
    auto *optionsPanel = frame("ReportOptionsPanel");
    auto *optionsPanelLayout = new QVBoxLayout(optionsPanel);
    optionsPanelLayout->setContentsMargins(14, 12, 14, 12);
    optionsPanelLayout->setSpacing(10);
    optionsPanelLayout->addWidget(label("报告与输出选项", "SectionTitle"));
    auto *hint = label("优先开放影响日志体积、可读性和报告展示的参数；筛选类 rebot "
                       "参数暂不放到这里，避免和当前勾选逻辑冲突。",
                       "MutedText");
    hint->setWordWrap(true);
    optionsPanelLayout->addWidget(hint);
    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(10);
    grid->setColumnStretch(1, 1);
    grid->setColumnStretch(3, 1);
    logLevel_ = combo("LogLevelCombo", {{"TRACE", "TRACE"},
                                        {"DEBUG", "DEBUG"},
                                        {"INFO", "INFO"},
                                        {"WARN", "WARN"},
                                        {"NONE", "NONE"}});
    logLevel_->setToolTip("对应 rebot --loglevel 的消息阈值；TRACE 最详细，NONE 不保留消息。");
    visibleLevel_ = combo("VisibleLogLevelCombo", {{"跟随阈值", ""},
                                                   {"TRACE", "TRACE"},
                                                   {"DEBUG", "DEBUG"},
                                                   {"INFO", "INFO"},
                                                   {"WARN", "WARN"}});
    visibleLevel_->setToolTip(
        "对应 --loglevel LOGLEVEL:DEFAULT 的后半段，控制 Log 页面默认展示级别。");
    splitLog_ = new QCheckBox("拆分大体积 Log");
    splitLog_->setObjectName("SplitLogCheckBox");
    splitLog_->setToolTip("对应 rebot --splitlog，合并结果较大时可显著降低单页 Log 体积。");
    suiteStatLevel_ = new QSpinBox;
    suiteStatLevel_->setObjectName("SuiteStatLevelSpin");
    suiteStatLevel_->setRange(0, 99);
    suiteStatLevel_->setSpecialValueText("全部层级");
    suiteStatLevel_->setToolTip("对应 rebot --suitestatlevel；0 表示保持默认，显示全部套件层级。");
    removeKeywords_ = combo("RemoveKeywordsCombo", {{"保留全部", ""},
                                                    {"移除通过关键字", "passed"},
                                                    {"仅保留 WUKS 最后失败", "wuks"},
                                                    {"裁剪 FOR 通过迭代", "for"},
                                                    {"裁剪 WHILE 通过迭代", "while"},
                                                    {"移除全部关键字", "all"}});
    removeKeywords_->setToolTip("对应 rebot --removekeywords，可压缩合并后日志体积。");
    flattenKeywords_ = combo("FlattenKeywordsCombo", {{"不扁平化", ""},
                                                      {"FOR 循环", "for"},
                                                      {"WHILE 循环", "while"},
                                                      {"循环迭代", "iteration"}});
    flattenKeywords_->setToolTip("对应 rebot --flattenkeywords，可减少日志层级深度。");
    logTitle_ = new QLineEdit;
    logTitle_->setObjectName("LogTitleEdit");
    logTitle_->setPlaceholderText("留空使用默认 Log 标题");
    logTitle_->setToolTip("对应 rebot --logtitle。");
    reportTitle_ = new QLineEdit;
    reportTitle_->setObjectName("ReportTitleEdit");
    reportTitle_->setPlaceholderText("留空使用默认 Report 标题");
    reportTitle_->setToolTip("对应 rebot --reporttitle。");
    reportBackground_ = new QLineEdit;
    reportBackground_->setObjectName("ReportBackgroundEdit");
    reportBackground_->setPlaceholderText("例如 green:red:yellow");
    reportBackground_->setToolTip("对应 rebot --reportbackground，格式为 passed:failed:skipped。");
    auto field = [grid](int row, int col, const QString &text, QWidget *widget, int span = 1) {
        grid->addWidget(label(text, "FieldLabel"), row, col);
        grid->addWidget(widget, row, col + 1, 1, span);
    };
    field(0, 0, "消息阈值", logLevel_);
    field(0, 2, "默认显示", visibleLevel_);
    grid->addWidget(splitLog_, 1, 0, 1, 2);
    field(1, 2, "套件统计", suiteStatLevel_);
    field(2, 0, "关键字裁剪", removeKeywords_);
    field(2, 2, "关键字扁平化", flattenKeywords_);
    field(3, 0, "报告背景", reportBackground_, 3);
    field(4, 0, "Log 标题", logTitle_);
    field(4, 2, "Report 标题", reportTitle_);
    optionsPanelLayout->addLayout(grid);
    optionsPanelLayout->addStretch();
    optionsLayout->addWidget(optionsPanel);
    actionWidgets_ +=
        {logLevel_,        visibleLevel_, splitLog_,    suiteStatLevel_,  removeKeywords_,
         flattenKeywords_, logTitle_,     reportTitle_, reportBackground_};
}
void MainWindow::populateCandidates(std::vector<SuiteCandidate> candidates,
                                    const SelectionSuggestion &suggestion) {
    candidates_ = std::move(candidates);
    suggestion_ = suggestion;
    selected_.clear();
    defaults_.clear();
    candidatePositions_.clear();
    groupSizes_.clear();
    for (const auto &id : suggestion.selected_ids)
        selected_.insert(q(id));
    defaults_ = selected_;
    for (std::size_t pos = 0; pos < candidates_.size(); ++pos) {
        candidatePositions_.insert(q(candidates_[pos].candidate_id), static_cast<int>(pos));
        ++groupSizes_[q(candidates_[pos].suite_path)];
    }
    rebuildModel();
}
void MainWindow::clearCandidates() {
    if (busy_)
        return;
    activePaths_.clear();
    parsedCount_ = failedCount_ = 0;
    populateCandidates({}, {});
}
QStringList MainWindow::selectedIds() const {
    QStringList result;
    for (const auto &candidate : candidates_)
        if (selected_.contains(q(candidate.candidate_id)))
            result.push_back(q(candidate.candidate_id));
    return result;
}
QStringList MainWindow::filteredIds() const {
    QStringList result;
    std::function<void(const QModelIndex &)> visit = [&](const QModelIndex &parent) {
        for (int row = 0; row < proxy->rowCount(parent); ++row) {
            auto index = proxy->index(row, Check, parent);
            auto id = index.data(CandidateIdRole).toString();
            if (!id.isEmpty())
                result.push_back(id);
            else
                visit(index);
        }
    };
    visit({});
    return result;
}
void MainWindow::applySelection(const QStringList &ids) {
    selected_.clear();
    for (const auto &id : ids)
        if (candidatePositions_.contains(id))
            selected_.insert(id);
    syncCheckStates();
    refreshFilter();
    emit selectionChanged();
}
int MainWindow::setChecked(const QStringList &ids, bool checked) {
    int changed = 0;
    for (const auto &id : ids) {
        if (!candidatePositions_.contains(id) || selected_.contains(id) == checked)
            continue;
        if (checked)
            selected_.insert(id);
        else
            selected_.remove(id);
        ++changed;
    }
    if (changed) {
        syncCheckStates();
        refreshFilter();
        emit selectionChanged();
    }
    return changed;
}
void MainWindow::selectAllFiltered() {
    setChecked(filteredIds(), true);
}
void MainWindow::invertFiltered() {
    const auto ids = filteredIds();
    for (const auto &id : ids) {
        if (selected_.contains(id))
            selected_.remove(id);
        else
            selected_.insert(id);
    }
    syncCheckStates();
    refreshFilter();
    emit selectionChanged();
}
void MainWindow::restoreDefaultSelection() {
    if (candidates_.empty()) {
        showError("当前没有可恢复的数据，请先加载 XML。");
        return;
    }
    applySelection(qs(suggestion_.selected_ids));
}
void MainWindow::rebuildModel() {
    const QString current = currentCandidateId();
    rebuilding_ = true;
    treeView->setUpdatesEnabled(false);
    treeView->selectionModel()->clear();
    checks_.clear();
    // Do not block structural signals. The proxy and the existing selection
    // model must invalidate their persistent indexes before rows are deleted.
    model->removeRows(0, model->rowCount());
    const bool tree = mode_ == "tree";
    treeView->setRootIsDecorated(tree);
    // Keep checkboxes in the fixed checkbox column. Putting indentation there
    // clips the indicators of nested suites once depth exceeds its 62px width.
    treeView->setTreePosition(tree ? SuitePath : Check);
    treeView->setItemsExpandable(tree);
    treeView->setExpandsOnDoubleClick(tree);
    treeView->setSortingEnabled(!tree);
    proxy->sort(tree ? -1 : SuitePath, Qt::AscendingOrder);
    std::vector<std::size_t> order(candidates_.size());
    std::iota(order.begin(), order.end(), 0);
    std::map<std::vector<std::string>, QStandardItem *> groups;
    std::map<std::vector<std::string>, int> groupCounts;
    if (tree) {
        for (const auto &candidate : candidates_) {
            auto parts = candidate.suite_path_parts;
            if (parts.empty())
                parts.push_back(candidate.suite_path);
            std::vector<std::string> key;
            for (const auto &part : parts) {
                key.push_back(part);
                ++groupCounts[key];
            }
        }
        std::stable_sort(order.begin(), order.end(), [this](std::size_t a, std::size_t b) {
            const auto &left = candidates_[a];
            const auto &right = candidates_[b];
            auto leftParts = qs(left.suite_path_parts), rightParts = qs(right.suite_path_parts);
            for (auto &part : leftParts)
                part = part.toCaseFolded();
            for (auto &part : rightParts)
                part = part.toCaseFolded();
            if (leftParts != rightParts)
                return std::lexicographical_compare(leftParts.begin(), leftParts.end(),
                                                    rightParts.begin(), rightParts.end());
            auto leftFile = q(left.file_name).toCaseFolded(),
                 rightFile = q(right.file_name).toCaseFolded();
            return leftFile != rightFile ? leftFile < rightFile
                                         : left.candidate_id < right.candidate_id;
        });
    }
    for (const auto position : order) {
        const auto &candidate = candidates_[position];
        auto *parent = model->invisibleRootItem();
        if (tree) {
            auto parts = candidate.suite_path_parts;
            if (parts.empty())
                parts.push_back(candidate.suite_path);
            std::vector<std::string> key;
            for (const auto &part : parts) {
                key.push_back(part);
                auto found = groups.find(key);
                if (found == groups.end()) {
                    QList<QStandardItem *> row;
                    for (int col = 0; col < ColumnCount; ++col) {
                        auto *item = new QStandardItem;
                        item->setEditable(false);
                        row.push_back(item);
                    }
                    row[Check]->setCheckable(true);
                    row[Check]->setData("group", NodeKindRole);
                    row[Check]->setCheckState(Qt::Unchecked);
                    row[SuitePath]->setText(q(part) + QString(" (%1)").arg(groupCounts[key]));
                    row[SuitePath]->setData(qs(key).join(" / "), FullPathRole);
                    row[SuitePath]->setToolTip(qs(key).join(" / "));
                    parent->appendRow(row);
                    groups[key] = row[Check];
                    parent = row[Check];
                } else
                    parent = found->second;
            }
        }
        const QString id = q(candidate.candidate_id),
                      status = q(candidate.status).trimmed().toUpper();
        const int conflictCount = groupSizes_.value(q(candidate.suite_path), 1);
        QStringList texts{"",
                          q(candidate.suite_path),
                          status,
                          defaults_.contains(id) ? "是" : "否",
                          QString::number(conflictCount),
                          startTime(candidate),
                          q(candidate.file_name)};
        QList<QStandardItem *> row;
        for (int col = 0; col < ColumnCount; ++col) {
            auto *item = new QStandardItem(texts[col]);
            item->setEditable(false);
            if (col >= Status && col <= ConflictSize)
                item->setTextAlignment(Qt::AlignCenter);
            if (conflictCount > 1 && col > Check)
                item->setBackground(QColor("#fff8ea"));
            row.push_back(item);
        }
        row[Check]->setCheckable(true);
        row[Check]->setData(id, CandidateIdRole);
        row[Check]->setData("candidate", NodeKindRole);
        row[Check]->setCheckState(selected_.contains(id) ? Qt::Checked : Qt::Unchecked);
        row[SuitePath]->setData(q(candidate.suite_path), FullPathRole);
        row[SuitePath]->setToolTip(q(candidate.suite_path));
        row[Status]->setForeground(QColor(statusColor(status)));
        row[File]->setToolTip(q(candidate.file_path));
        parent->appendRow(row);
        checks_.insert(id, row[Check]);
    }
    syncCheckStates();
    rebuilding_ = false;
    refreshFilter();
    if (tree)
        treeView->expandAll();
    ensureCurrent(current);
    treeView->setUpdatesEnabled(true);
}
std::pair<int, int> MainWindow::syncGroup(QStandardItem *item) {
    const auto id = item->data(CandidateIdRole).toString();
    if (!id.isEmpty()) {
        const bool checked = selected_.contains(id);
        item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
        return {checked ? 1 : 0, 1};
    }
    int checked = 0, total = 0;
    for (int row = 0; row < item->rowCount(); ++row) {
        const auto counts = syncGroup(item->child(row, Check));
        checked += counts.first;
        total += counts.second;
    }
    if (item != model->invisibleRootItem())
        item->setCheckState(checked == 0 ? Qt::Unchecked
                                         : (checked == total ? Qt::Checked : Qt::PartiallyChecked));
    return {checked, total};
}
void MainWindow::syncCheckStates() {
    syncing_ = true;
    // Data-change notifications remain enabled so the model/view accessibility
    // cache stays consistent. itemChanged ignores these guarded updates.
    syncGroup(model->invisibleRootItem());
    syncing_ = false;
}
QStringList MainWindow::descendantIds(const QModelIndex &sourceIndex) const {
    if (!sourceIndex.isValid())
        return {};
    auto check = sourceIndex.siblingAtColumn(Check);
    const auto id = check.data(CandidateIdRole).toString();
    if (!id.isEmpty())
        return {id};
    QStringList result;
    for (int row = 0; row < model->rowCount(check); ++row)
        result += descendantIds(model->index(row, Check, check));
    return result;
}
void MainWindow::itemChanged(QStandardItem *item) {
    if (syncing_ || rebuilding_ || item->column() != Check)
        return;
    const auto ids = descendantIds(item->index());
    const bool checked = item->checkState() != Qt::Unchecked;
    for (const auto &id : ids) {
        if (checked)
            selected_.insert(id);
        else
            selected_.remove(id);
    }
    syncCheckStates();
    // Defer proxy invalidation until the checkbox event has completely unwound.
    queueSelectionRefresh();
}
void MainWindow::queueSelectionRefresh() {
    if (refreshQueued_)
        return;
    refreshQueued_ = true;
    QTimer::singleShot(0, this, [this] {
        refreshQueued_ = false;
        refreshFilter();
        emit selectionChanged();
    });
}
void MainWindow::refreshFilter() {
    if (rebuilding_)
        return;
    const QString current = currentCandidateId();
    proxy->setFilters(conflictOnly_->isChecked(), selectedOnly_->isChecked(),
                      statusFilter_->currentData().toString(), search_->text());
    if (mode_ == "tree")
        treeView->expandAll();
    ensureCurrent(current);
    refreshSummary();
    refreshDetail();
}
void MainWindow::setFilters(bool conflicts, bool selected, const QString &status,
                            const QString &search) {
    QSignalBlocker a(conflictOnly_), b(selectedOnly_), c(statusFilter_), d(search_);
    conflictOnly_->setChecked(conflicts);
    selectedOnly_->setChecked(selected);
    statusFilter_->setCurrentIndex(qMax(0, statusFilter_->findData(status.toUpper())));
    search_->setText(search);
    refreshFilter();
}
void MainWindow::setViewMode(const QString &value) {
    const auto newMode = value == "tree" ? QString("tree") : QString("flat");
    if (mode_ == newMode)
        return;
    saveHeader();
    mode_ = newMode;
    {
        QSignalBlocker block(viewMode_);
        viewMode_->setCurrentIndex(viewMode_->findData(mode_));
    }
    settings_->setValue("candidate/view_mode", mode_);
    rebuildModel();
    loadHeader();
}
QModelIndex MainWindow::candidateIndex(const QString &id) const {
    auto *item = checks_.value(id, nullptr);
    return item ? proxy->mapFromSource(item->index()) : QModelIndex();
}
QModelIndex MainWindow::groupIndex(const QString &path) const {
    std::function<QModelIndex(const QModelIndex &)> find =
        [&](const QModelIndex &parent) -> QModelIndex {
        for (int row = 0; row < model->rowCount(parent); ++row) {
            auto index = model->index(row, Check, parent);
            if (index.data(NodeKindRole).toString() != "group")
                continue;
            if (index.siblingAtColumn(SuitePath).data(FullPathRole).toString() == path)
                return index;
            const auto child = find(index);
            if (child.isValid())
                return child;
        }
        return {};
    };
    return proxy->mapFromSource(find({}));
}
QString MainWindow::currentCandidateId() const {
    return treeView->currentIndex().siblingAtColumn(Check).data(CandidateIdRole).toString();
}
void MainWindow::ensureCurrent(const QString &preferred) {
    auto index = candidateIndex(preferred);
    if (index.isValid()) {
        index = index.siblingAtColumn(SuitePath);
        if (treeView->currentIndex() != index) {
            treeView->setCurrentIndex(index);
            treeView->scrollTo(index, QAbstractItemView::PositionAtCenter);
        }
        return;
    }
    // Retain focused parent rows while checking a group or refreshing filters.
    if (treeView->currentIndex().isValid())
        return;
    const auto ids = filteredIds();
    if (!ids.isEmpty()) {
        index = candidateIndex(ids.front()).siblingAtColumn(SuitePath);
        treeView->setCurrentIndex(index);
        treeView->scrollTo(index, QAbstractItemView::PositionAtCenter);
    } else {
        treeView->setCurrentIndex({});
        treeView->clearSelection();
    }
}
void MainWindow::refreshSummary() {
    const int visible = filteredIds().size();
    visibleCount_->setText(QString("可见: %1").arg(visible));
    if (busy_)
        return;
    if (candidates_.empty()) {
        summary_->setText(parsedCount_ || failedCount_
                              ? QString("未加载到可用套件，成功文件 %1，失败文件 %2。")
                                    .arg(parsedCount_)
                                    .arg(failedCount_)
                              : QString("未加载 XML 文件"));
        return;
    }
    summary_->setText(
        QString(
            "已加载 %1 个套件，可见 %2 个，当前勾选 %3 个，冲突组 %4，成功文件 %5，失败文件 %6。")
            .arg(candidates_.size())
            .arg(visible)
            .arg(selected_.size())
            .arg(suggestion_.conflicts.size())
            .arg(parsedCount_)
            .arg(failedCount_));
}
const SuiteCandidate *MainWindow::candidate(const QString &id) const {
    auto found = candidatePositions_.constFind(id);
    return found == candidatePositions_.cend() ? nullptr : &candidates_[*found];
}
void MainWindow::refreshDetail() {
    const auto id = currentCandidateId();
    const auto *item = candidate(id);
    const QString dash = QStringLiteral("—");
    detailSelected_->setText(item ? (selected_.contains(id) ? "已勾选" : "未勾选") : dash);
    detailDefault_->setText(item ? (defaults_.contains(id) ? "是" : "否") : dash);
    detailPath_->setText(item ? q(item->suite_path) : dash);
    detailFile_->setText(item ? q(item->file_name) : dash);
    detailFilePath_->setText(item ? q(item->file_path) : dash);
    detailStart_->setText(item ? startTime(*item) : dash);
    detailTests_->setPlainText(item && !item->test_names.empty() ? qs(item->test_names).join('\n')
                                                                 : dash);
    detailFailures_->setPlainText(
        item && !item->failure_reasons.empty() ? qs(item->failure_reasons).join("\n\n") : dash);
    auto status = item ? q(item->status).trimmed().toUpper() : QString("UNKNOWN");
    if (status != "PASS" && status != "FAIL" && status != "SKIP")
        status = "UNKNOWN";
    detailStatus_->setText(status);
    detailStatus_->setProperty("status", status);
    detailStatus_->style()->unpolish(detailStatus_);
    detailStatus_->style()->polish(detailStatus_);
}
QStringList MainWindow::contextSelection(const QPoint &position) {
    const auto index = treeView->indexAt(position);
    if (!index.isValid())
        return {};
    auto *selection = treeView->selectionModel();
    if (!selection->isSelected(index)) {
        selection->select(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        selection->setCurrentIndex(index, QItemSelectionModel::NoUpdate);
    }
    QSet<QString> ids;
    for (const auto &row : selection->selectedRows(Check))
        for (const auto &id : descendantIds(proxy->mapToSource(row)))
            ids.insert(id);
    QStringList result;
    for (const auto &item : candidates_)
        if (ids.contains(q(item.candidate_id)))
            result.push_back(q(item.candidate_id));
    return result;
}
QMenu *MainWindow::buildContextMenu(const QStringList &ids, const QString &clickedId) {
    auto *menu = new QMenu(treeView);
    auto add = [&](const QString &text, bool enabled, const std::function<void()> &callback) {
        auto *action = menu->addAction(text);
        action->setEnabled(enabled && !busy_);
        connect(action, &QAction::triggered, this, callback);
        return action;
    };
    add("勾选", !ids.isEmpty(), [this, ids] { setChecked(ids, true); });
    add("反勾选", !ids.isEmpty(), [this, ids] { setChecked(ids, false); });
    menu->addSeparator();
    add("全选当前过滤", !candidates_.empty(), [this] { selectAllFiltered(); });
    add("反选当前过滤", !candidates_.empty(), [this] { invertFiltered(); });
    menu->addSeparator();
    add("查看详情", candidate(clickedId) != nullptr, [this, clickedId] { showDetail(clickedId); });
    const auto *clicked = candidate(clickedId);
    const auto sourceDirectory = clicked && !clicked->file_path.empty()
                                     ? QFileInfo(q(clicked->file_path)).absolutePath()
                                     : QString();
    add("打开文件所在目录", !sourceDirectory.isEmpty(),
        [this, sourceDirectory] { openPath(sourceDirectory); });
    add("导出该测试 XML", candidate(clickedId) != nullptr,
        [this, clickedId] { chooseSingleXml(clickedId); });
    add("合并选中结果到指定目录…", !ids.isEmpty(), [this, ids] { chooseSelectedXml(ids); })
        ->setToolTip("将当前高亮选中的套件合并为一个 XML 文件。");
    menu->addSeparator();
    add("打开输出目录", true, [this] { openPath(outputDir_); });
    return menu;
}
void MainWindow::handleDoubleClick(const QModelIndex &index) {
    if (!index.isValid() || index.column() == Check || busy_)
        return;
    auto id = index.siblingAtColumn(Check).data(CandidateIdRole).toString();
    if (!id.isEmpty())
        emit candidateDoubleClicked(id);
}
void MainWindow::showDetail(const QString &id) {
    const auto index = candidateIndex(id);
    if (!index.isValid())
        return;
    treeView->selectionModel()->setCurrentIndex(index, QItemSelectionModel::NoUpdate);
    refreshDetail();
    showDialog(detailDialog_, "detail/dialog_geometry", 48);
}
void MainWindow::showDialog(QDialog *dialog, const QString &key, int offset) {
    if (!dialog->property("geometryRestored").toBool()) {
        const auto geometry = settings_->value(key).toByteArray();
        if (geometry.isEmpty() || !dialog->restoreGeometry(geometry)) {
            const auto available = screen()->availableGeometry();
            const int x =
                qBound(available.left() + 24, frameGeometry().right() - dialog->width(),
                       qMax(available.left() + 24, available.right() - dialog->width() - 24));
            const int y =
                qBound(available.top() + 24, frameGeometry().top() + offset,
                       qMax(available.top() + 24, available.bottom() - dialog->height() - 24));
            dialog->move(x, y);
        }
        dialog->setProperty("geometryRestored", true);
    }
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}
void MainWindow::setOutputDir(const QString &path) {
    outputDir_ = path;
    outputLabel_->setText(outputLabel_->fontMetrics().elidedText(
        "输出目录: " + path, Qt::ElideMiddle, outputLabel_->width()));
    outputLabel_->setToolTip(path);
}
void MainWindow::chooseOutputDir() {
    const auto directory = QFileDialog::getExistingDirectory(this, "选择输出目录", outputDir_);
    if (!directory.isEmpty())
        setOutputDir(directory);
}
rf::Options MainWindow::reportOptions() const {
    rf::Options options;
    options.level = s(logLevel_->currentData().toString());
    options.visible = s(visibleLevel_->currentData().toString());
    if (options.visible.empty() || options.level == "NONE")
        options.visible = options.level;
    options.split_log = splitLog_->isChecked();
    options.suite_stat_level = suiteStatLevel_->value() > 0 ? suiteStatLevel_->value() : -1;
    options.remove_keywords = s(removeKeywords_->currentData().toString());
    options.flatten_keywords = s(flattenKeywords_->currentData().toString());
    options.logtitle = s(logTitle_->text().trimmed());
    options.reporttitle = s(reportTitle_->text().trimmed());
    options.report_background = s(reportBackground_->text().trimmed());
    return options;
}
void MainWindow::loadFiles(const QStringList &paths) {
    if (busy_) {
        showError("当前任务仍在执行，请稍后再试。");
        return;
    }
    QStringList incoming;
    for (const auto &raw : paths) {
        const QFileInfo info(raw);
        const auto path = info.absoluteFilePath();
        if (info.exists() && info.isFile() && path.endsWith(".xml", Qt::CaseInsensitive) &&
            !incoming.contains(path))
            incoming.push_back(path);
    }
    if (incoming.isEmpty()) {
        showError("未检测到可用的 XML 文件。");
        return;
    }
    QStringList mergedPaths;
    for (const auto &path : activePaths_)
        if (!incoming.contains(path))
            mergedPaths.push_back(path);
    mergedPaths += incoming;
    const auto files = ss(mergedPaths);
    const auto loader = loader_;
    const auto service = service_;
    startJob("load", "读取 XML 并生成默认勾选中...", [files, loader, service] {
        JobResult result;
        result.loaded = loader ? loader(files) : service.load_candidates(files);
        result.suggestion = service.suggest_selection(result.loaded.candidates);
        return result;
    });
}
void MainWindow::mergeSelected() {
    if (candidates_.empty()) {
        showError("请先加载 XML 文件。");
        return;
    }
    const auto ids = ss(selectedIds());
    if (ids.empty()) {
        showError("请至少勾选一个测试套件。");
        return;
    }
    const auto items = candidates_;
    const auto directory = s(outputDir_);
    const auto options = reportOptions();
    const auto service = service_;
    startJob("merge", "合并勾选套件并生成报告中...", [items, ids, directory, options, service] {
        JobResult result;
        result.merged = service.merge_selected(items, ids, directory, options);
        return result;
    });
}
QString MainWindow::normalizeXlsxPath(const QString &path) {
    const auto dot = extensionOffset(path);
    if (dot >= 0 && path.mid(dot).compare(".xlsx", Qt::CaseInsensitive) == 0)
        return path;
    return (dot < 0 ? path : path.left(dot)) + ".xlsx";
}
QString MainWindow::safeExportName(QString name) {
    name.replace(QRegularExpression("[<>:\"/\\\\|?*\\x{0000}-\\x{001f}]"), "_");
    name.remove(QRegularExpression("^[ .]+|[ .]+$"));
    auto codePoints = name.toStdU32String();
    if (codePoints.size() > 120)
        codePoints.resize(120);
    name = QString::fromUcs4(codePoints.data(), static_cast<qsizetype>(codePoints.size()));
    if (name.isEmpty())
        return "suite";
    if (QRegularExpression("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])$",
                           QRegularExpression::CaseInsensitiveOption)
            .match(name)
            .hasMatch())
        name.prepend('_');
    return name;
}
void MainWindow::chooseExcelPath() {
    if (candidates_.empty()) {
        showError("请先加载 XML 文件。");
        return;
    }
    if (selected_.isEmpty()) {
        showError("请至少勾选一个测试套件。");
        return;
    }
    const auto directory = QFileInfo(outputDir_).isDir() ? outputDir_ : QDir::currentPath();
    const auto path = QFileDialog::getSaveFileName(
        this, "导出勾选测试 Excel",
        QDir(directory).filePath(
            "selected-tests-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss") + ".xlsx"),
        "Excel Files (*.xlsx)");
    if (!path.isEmpty())
        exportExcel(path);
}
void MainWindow::exportExcel(const QString &path) {
    if (path.isEmpty())
        return;
    if (candidates_.empty()) {
        showError("请先加载 XML 文件。");
        return;
    }
    const auto ids = ss(selectedIds());
    if (ids.empty()) {
        showError("请至少勾选一个测试套件。");
        return;
    }
    const auto items = candidates_;
    const auto output = s(normalizeXlsxPath(path));
    const auto service = service_;
    startJob("excel", "导出勾选测试 Excel 中...", [items, ids, output, service] {
        JobResult result;
        result.excel = service.export_selected_tests_excel(items, ids, output);
        return result;
    });
}
void MainWindow::chooseSingleXml(const QString &id) {
    const auto *item = candidate(id);
    if (!item)
        return;
    const auto directory = QFileInfo(outputDir_).isDir() ? outputDir_ : QDir::currentPath();
    auto path = QFileDialog::getSaveFileName(
        this, "导出该测试 XML",
        QDir(directory).filePath(safeExportName(q(item->suite_name)) + ".xml"),
        "XML Files (*.xml)");
    if (!path.isEmpty())
        exportSingleXml(id, path);
}
void MainWindow::exportSingleXml(const QString &id, const QString &path) {
    if (path.isEmpty())
        return;
    const auto *found = candidate(id);
    if (!found) {
        showError("当前记录已失效，请重新加载 XML 文件。");
        return;
    }
    const auto item = *found;
    const auto output = s(extensionOffset(path) < 0 ? path + ".xml" : path);
    const auto service = service_;
    startJob("single-xml", "导出 XML 中：" + q(item.suite_path), [item, output, service] {
        JobResult result;
        result.path = q(service.export_single_candidate_xml(item, output));
        return result;
    });
}
void MainWindow::chooseSelectedXml(const QStringList &ids) {
    if (ids.isEmpty() || busy_)
        return;
    const auto directory = QFileDialog::getExistingDirectory(
        this, QString("合并选中结果为 XML（%1 个套件）").arg(ids.size()), outputDir_);
    if (!directory.isEmpty())
        exportSelectedXml(ids, directory);
}
void MainWindow::exportSelectedXml(const QStringList &ids, const QString &directory) {
    if (directory.isEmpty())
        return;
    if (ids.isEmpty()) {
        showError("请至少选中一个测试套件。");
        return;
    }
    auto uniqueIds = ids;
    uniqueIds.removeDuplicates();
    const auto items = candidates_;
    const auto selected = ss(uniqueIds);
    const auto output = s(directory);
    const auto service = service_;
    startJob("selected-xml", QString("合并导出 %1 个选中套件中...").arg(uniqueIds.size()),
             [items, selected, output, service] {
                 JobResult result;
                 result.path = q(service.export_selected_xml(items, selected, output));
                 result.selectedCount = static_cast<int>(selected.size());
                 return result;
             });
}
void MainWindow::openSingleLog(const QString &id) {
    const auto *found = candidate(id);
    if (!found) {
        showError("当前记录已失效，请重新加载 XML 文件。");
        return;
    }
    const auto item = *found;
    const auto directory = s(outputDir_);
    const auto options = reportOptions();
    const auto service = service_;
    startJob("single-log", "生成日志中：" + q(item.suite_path),
             [item, directory, options, service] {
                 JobResult result;
                 result.path = q(service.generate_single_candidate_log(item, directory, options));
                 return result;
             });
}
void MainWindow::setBusy(bool busy, const QString &message) {
    busy_ = busy;
    for (auto *widget : actionWidgets_)
        widget->setEnabled(!busy);
    progress_->setVisible(busy);
    progress_->setRange(0, busy ? 0 : 100);
    if (!message.isEmpty())
        summary_->setText(message);
    if (!busy)
        refreshSummary();
}
bool MainWindow::startJob(const QString &kind, const QString &message,
                          std::function<JobResult()> job) {
    if (busy_) {
        showError("当前任务仍在执行，请稍后再试。");
        return false;
    }
    setBusy(true, message);
    watcher_.setFuture(QtConcurrent::run([job = std::move(job), kind] {
        JobResult result;
        try {
            result = job();
        } catch (const std::exception &error) {
            result.error = QString::fromUtf8(error.what());
        } catch (...) {
            result.error = "任务发生未知错误。";
        }
        result.kind = kind;
        return result;
    }));
    return true;
}
void MainWindow::jobFinished() {
    auto result = watcher_.result();
    setBusy(false);
    if (closing_) {
        emit taskFinished(result.kind, result.error.isEmpty());
        close();
        return;
    }
    if (!result.error.isEmpty()) {
        showError(result.error);
        emit taskFinished(result.kind, false);
        return;
    }
    if (result.kind == "load") {
        activePaths_.clear();
        for (const auto &path : result.loaded.successful_file_paths)
            activePaths_.push_back(QDir::fromNativeSeparators(q(path)));
        parsedCount_ = result.loaded.parsed_file_count;
        failedCount_ = result.loaded.failed_file_count;
        populateCandidates(std::move(result.loaded.candidates), result.suggestion);
        emit candidatesLoaded();
        if (!result.loaded.issues.empty()) {
            QStringList details;
            int count = 0;
            for (const auto &issue : result.loaded.issues) {
                if (count++ == 8)
                    break;
                details.push_back("- " + q(issue.file_path) + " [" + q(issue.stage) + "] " +
                                  q(issue.message));
            }
            if (result.loaded.issues.size() > 8)
                details.push_back(
                    QString("... 其余 %1 条省略").arg(result.loaded.issues.size() - 8));
            const auto message =
                (parsedCount_ > 0 ? QString("已跳过不可解析文件，其余文件可继续使用。\n\n")
                                  : QString("没有可用的 XML 数据。\n\n")) +
                details.join('\n');
            if (interactive_)
                QMessageBox::warning(this, parsedCount_ > 0 ? "部分文件加载失败" : "加载失败",
                                     message);
            else
                qWarning().noquote() << message;
        }
    } else if (result.kind == "merge") {
        const auto &value = result.merged;
        emit merged(q(value.merged_xml_path), q(value.log_path), q(value.report_path),
                    q(value.conflict_report_path));
        if (interactive_) {
            QMessageBox dialog(QMessageBox::Information, "合并完成",
                               QString("勾选整合完成：已选择 %1/%2 个套件。")
                                   .arg(value.selected_count)
                                   .arg(value.candidate_count),
                               QMessageBox::NoButton, this);
            dialog.setInformativeText("Merged XML: " + q(value.merged_xml_path) + "\nLog: " +
                                      q(value.log_path) + "\nReport: " + q(value.report_path) +
                                      "\nConflict CSV: " + q(value.conflict_report_path));
            auto *output = dialog.addButton("打开输出目录", QMessageBox::ActionRole);
            auto *log = dialog.addButton("打开 Log", QMessageBox::ActionRole);
            auto *report = dialog.addButton("打开 Report", QMessageBox::ActionRole);
            auto *conflict = dialog.addButton("打开冲突 CSV", QMessageBox::ActionRole);
            dialog.addButton(QMessageBox::Close);
            dialog.exec();
            if (dialog.clickedButton() == output)
                openPath(outputDir_);
            else if (dialog.clickedButton() == log)
                openPath(q(value.log_path));
            else if (dialog.clickedButton() == report)
                openPath(q(value.report_path));
            else if (dialog.clickedButton() == conflict)
                openPath(q(value.conflict_report_path));
        }
    } else if (result.kind == "excel") {
        emit excelExported(q(result.excel.output_path));
        showInfo("导出完成", QString("已导出 %1 条测试，来自 %2 个勾选套件：\n%3")
                                 .arg(result.excel.test_count)
                                 .arg(result.excel.selected_suite_count)
                                 .arg(q(result.excel.output_path)));
    } else if (result.kind == "single-xml") {
        emit singleXmlExported(result.path);
        showInfo("导出完成", "测试 XML 已导出：\n" + result.path);
    } else if (result.kind == "selected-xml") {
        emit selectedXmlExported(result.path);
        showInfo("导出完成", QString("已将 %1 个选中套件合并为 XML：\n%2")
                                 .arg(result.selectedCount)
                                 .arg(result.path));
    } else if (result.kind == "single-log") {
        emit singleLogReady(result.path);
        if (interactive_)
            openPath(result.path);
    }
    emit taskFinished(result.kind, true);
}
void MainWindow::showError(const QString &message) {
    qWarning().noquote() << message;
    emit errorOccurred(message);
    if (interactive_ && !closing_)
        QMessageBox::critical(this, "错误", message);
}
void MainWindow::showInfo(const QString &title, const QString &message) {
    if (interactive_ && !closing_)
        QMessageBox::information(this, title, message);
}
void MainWindow::openPath(const QString &path) {
    if (!QFileInfo::exists(path)) {
        showError("路径不存在: " + path);
        return;
    }
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath())))
        showError("无法打开路径: " + path);
}
void MainWindow::setColumnVisible(int column, bool visible) {
    if (column < 0 || column >= ColumnCount || column == Check ||
        (mode_ == "tree" && column == SuitePath && !visible))
        return;
    treeView->setColumnHidden(column, !visible);
    if (visible && treeView->columnWidth(column) <= 1)
        treeView->setColumnWidth(column, 120);
    saveHeader();
}
void MainWindow::saveHeader() {
    if (restoringHeader_)
        return;
    const auto state = treeView->header()->saveState();
    settings_->setValue("candidate/header_state_" + mode_, state);
    QVariantList columns;
    for (int col = 0; col < ColumnCount; ++col)
        if (!treeView->isColumnHidden(col))
            columns.push_back(col);
    settings_->setValue("candidate/visible_columns_" + mode_, columns);
    if (mode_ == "flat")
        settings_->setValue("candidate/header_state", state);
}
void MainWindow::loadHeader() {
    restoringHeader_ = true;
    const int widths[] = {62, 520, 96, 96, 120, 178, 240};
    for (int col = 0; col < ColumnCount; ++col) {
        treeView->setColumnWidth(col, widths[col]);
        treeView->setColumnHidden(col, col == ConflictSize);
    }
    auto state = settings_->value("candidate/header_state_" + mode_);
    if (!state.isValid() && mode_ == "flat")
        state = settings_->value("candidate/header_state");
    if (state.isValid())
        treeView->header()->restoreState(state.toByteArray());
    const auto saved = settings_->value("candidate/visible_columns_" + mode_);
    if (saved.isValid()) {
        QVariantList values = saved.toList();
        if (saved.typeId() == QMetaType::QString) {
            auto text = saved.toString();
            text.remove('[');
            text.remove(']');
            for (const auto &value : text.split(',', Qt::SkipEmptyParts))
                values.push_back(value.trimmed());
        }
        QSet<int> visible;
        for (const auto &value : values) {
            bool ok = false;
            const int column = value.toInt(&ok);
            if (ok)
                visible.insert(column);
        }
        visible.insert(Check);
        if (mode_ == "tree")
            visible.insert(SuitePath);
        for (int col = 0; col < ColumnCount; ++col)
            treeView->setColumnHidden(col, !visible.contains(col));
    }
    treeView->setColumnHidden(Check, false);
    if (mode_ == "tree")
        treeView->setColumnHidden(SuitePath, false);
    restoringHeader_ = false;
}
void MainWindow::savePreferences() {
    saveHeader();
    if (detailDialog_->property("geometryRestored").toBool())
        settings_->setValue("detail/dialog_geometry", detailDialog_->saveGeometry());
    if (optionsDialog_->property("geometryRestored").toBool())
        settings_->setValue("report_options/dialog_geometry", optionsDialog_->saveGeometry());
    settings_->sync();
}
bool MainWindow::eventFilter(QObject *object, QEvent *event) {
    if (object == outputLabel_ && event->type() == QEvent::Resize)
        outputLabel_->setText(outputLabel_->fontMetrics().elidedText(
            "输出目录: " + outputDir_, Qt::ElideMiddle, outputLabel_->width()));
    if (object == outputLabel_ && event->type() == QEvent::MouseButtonRelease && !busy_) {
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() == Qt::LeftButton &&
            outputLabel_->rect().contains(mouse->position().toPoint()))
            chooseOutputDir();
    }
    if (event->type() == QEvent::Hide) {
        if (object == detailDialog_ && detailDialog_->property("geometryRestored").toBool())
            settings_->setValue("detail/dialog_geometry", detailDialog_->saveGeometry());
        if (object == optionsDialog_ && optionsDialog_->property("geometryRestored").toBool())
            settings_->setValue("report_options/dialog_geometry", optionsDialog_->saveGeometry());
    }
    return QMainWindow::eventFilter(object, event);
}
void MainWindow::closeEvent(QCloseEvent *event) {
    savePreferences();
    if (busy_) {
        closing_ = true;
        summary_->setText("等待当前任务完成后关闭...");
        event->ignore();
        return;
    }
    optionsDialog_->close();
    detailDialog_->close();
    QMainWindow::closeEvent(event);
}
} // namespace xm::ui

#include <QFontDatabase>
#include <QMutex>
#include <QMutexLocker>
#include <QTextStream>
namespace xm::ui {
void configureHighDpi() {
    if (!qEnvironmentVariableIsSet("QT_ENABLE_HIGHDPI_SCALING"))
        qputenv("QT_ENABLE_HIGHDPI_SCALING", "1");
    if (!qEnvironmentVariableIsSet("QT_AUTO_SCREEN_SCALE_FACTOR"))
        qputenv("QT_AUTO_SCREEN_SCALE_FACTOR", "1");
    if (!qEnvironmentVariableIsSet("QT_SCALE_FACTOR_ROUNDING_POLICY"))
        qputenv("QT_SCALE_FACTOR_ROUNDING_POLICY", "RoundPreferFloor");
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::RoundPreferFloor);
}
void configureAppFont() {
    const auto available = QFontDatabase::families();
    QString selected = QApplication::font().family();
    for (const auto &preferred :
         {"Microsoft YaHei UI", "Segoe UI", "Noto Sans CJK SC", "Microsoft YaHei"})
        if (available.contains(preferred)) {
            selected = preferred;
            break;
        }
    QFont font(selected, 10);
    font.setHintingPreference(QFont::PreferFullHinting);
    font.setStyleStrategy(
        static_cast<QFont::StyleStrategy>(QFont::PreferAntialias | QFont::PreferQuality));
    QApplication::setFont(font);
}
void configureAppAppearance() {
    // The UI intentionally uses a light stylesheet. A native macOS dark palette
    // otherwise leaks into checkbox indicators, comboboxes and empty headers.
    QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Light);
    QApplication::setStyle(QStyleFactory::create("Fusion"));
    QPalette palette;
    palette.setColor(QPalette::Window, QColor("#f3f5f8"));
    palette.setColor(QPalette::WindowText, QColor("#111827"));
    palette.setColor(QPalette::Base, QColor("#fbfcfe"));
    palette.setColor(QPalette::AlternateBase, QColor("#f8fafc"));
    palette.setColor(QPalette::Text, QColor("#111827"));
    palette.setColor(QPalette::Button, QColor("#ffffff"));
    palette.setColor(QPalette::ButtonText, QColor("#111827"));
    palette.setColor(QPalette::ToolTipBase, QColor("#ffffff"));
    palette.setColor(QPalette::ToolTipText, QColor("#111827"));
    palette.setColor(QPalette::Highlight, QColor("#d9e8ff"));
    palette.setColor(QPalette::HighlightedText, QColor("#111827"));
    palette.setColor(QPalette::PlaceholderText, QColor("#6b7280"));
    palette.setColor(QPalette::Link, QColor("#0b5fff"));
    palette.setColor(QPalette::Light, QColor("#ffffff"));
    palette.setColor(QPalette::Midlight, QColor("#e6ecf3"));
    palette.setColor(QPalette::Mid, QColor("#c7d0db"));
    palette.setColor(QPalette::Dark, QColor("#728197"));
    palette.setColor(QPalette::Shadow, QColor("#4b5563"));
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        palette.setColor(QPalette::Disabled, role, QColor("#8f97a3"));
    QApplication::setPalette(palette);
}
bool shouldDisableMacAccessibility(const QString &platform, int majorVersion,
                                   const QByteArray &optIn) {
    const auto option = optIn.trimmed().toLower();
    return platform == "darwin" && majorVersion >= 26 && option != "1" && option != "true" &&
           option != "yes" && option != "on";
}
#ifndef Q_OS_MACOS
bool disableMacWindowAccessibility(QWidget *) {
    return false;
}
#endif
namespace {
QString logPath;
QMutex logMutex;
void logMessage(QtMsgType type, const QMessageLogContext &, const QString &message) {
    QMutexLocker lock(&logMutex);
    QFile log(logPath);
    if (log.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        QTextStream stream(&log);
        const char *level = type == QtDebugMsg      ? "DEBUG"
                            : type == QtInfoMsg     ? "INFO"
                            : type == QtWarningMsg  ? "WARNING"
                            : type == QtCriticalMsg ? "ERROR"
                                                    : "FATAL";
        stream << QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz") << " - " << level
               << " - " << message << '\n';
    }
    if (type == QtFatalMsg)
        std::abort();
}
} // namespace
void configureLogging() {
    logPath = QDir::current().filePath("debug.log");
#ifdef Q_OS_MACOS
    if (QCoreApplication::applicationDirPath().contains(".app/Contents/MacOS")) {
        const auto directory = QDir::home().filePath("Library/Logs/XmlMergeTool");
        if (QDir().mkpath(directory))
            logPath = QDir(directory).filePath("debug.log");
    }
#endif
    qInstallMessageHandler(logMessage);
}
} // namespace xm::ui
