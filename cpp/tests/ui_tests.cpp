#include "main_window.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QUrl>
#include <QXmlStreamReader>
#include <QtTest>
#include <atomic>
#include <chrono>
#include <thread>

using namespace xm;
using namespace xm::ui;
namespace {
std::vector<SuiteCandidate> samples() {
    std::vector<SuiteCandidate> result;
    for (int index = 0; index < 3; ++index) {
        SuiteCandidate candidate;
        candidate.candidate_id = "candidate-" + std::to_string(index);
        candidate.file_path = "/tmp/file" + std::to_string(index) + ".xml";
        candidate.file_name = "file" + std::to_string(index) + ".xml";
        candidate.suite_name = index < 2 ? "SuiteA" : "SuiteB";
        candidate.suite_path_parts = {"Root", index < 2 ? "ModuleA" : "ModuleB",
                                      candidate.suite_name};
        candidate.suite_path = rf::join(candidate.suite_path_parts, " / ");
        candidate.status = index == 0 ? "FAIL" : index == 1 ? "PASS" : "SKIP";
        candidate.suite_starttime = index == 0 ? "" : index == 1 ? "20260101 10:00:01.001" : "bad";
        candidate.suite_endtime = "20260101 10:00:02.001";
        candidate.test_names = {"Example test"};
        candidate.test_results.push_back({"Example test", candidate.status, "", 1.0});
        result.push_back(candidate);
    }
    return result;
}
void writeFile(const QString &path,
               const QByteArray &data =
                   "<robot><suite name=\"Root\" id=\"s1\"><test name=\"Case\" id=\"s1-t1\"><status "
                   "status=\"PASS\" starttime=\"20260101 10:00:00.000\" endtime=\"20260101 "
                   "10:00:01.000\"/></test><status status=\"PASS\" starttime=\"20260101 "
                   "10:00:00.000\" endtime=\"20260101 10:00:01.000\"/></suite></robot>") {
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(data), data.size());
}
} // namespace
class UrlCapture final : public QObject {
    Q_OBJECT
  public:
    UrlCapture() {
        QDesktopServices::setUrlHandler("file", this, "capture");
    }
    ~UrlCapture() override {
        QDesktopServices::unsetUrlHandler("file");
    }
    QUrl opened;
  public slots:
    void capture(const QUrl &url) {
        opened = url;
    }
};
class UiTests final : public QObject {
    Q_OBJECT
  private:
    QTemporaryDir temporary_;
    std::unique_ptr<MainWindow> window_;
    QString settings() const {
        return temporary_.filePath("settings.ini");
    }
    MainWindow &window() {
        return *window_;
    }
  private slots:
    void init() {
        QVERIFY(temporary_.isValid());
        QFile::remove(settings());
        window_ = std::make_unique<MainWindow>(nullptr, MainWindow::Loader{}, settings());
        window().setInteractive(false);
        window().populateCandidates(samples(), {{"candidate-1"}, {}});
        disableMacWindowAccessibility(&window());
        window().show();
        QTest::qWait(10);
    }
    void cleanup() {
        window_.reset();
    }
    void stableModelAndSelection() {
        auto *proxy = window().treeView->model();
        auto *selection = window().treeView->selectionModel();
        for (int index = 0; index < 30; ++index) {
            window().setViewMode(index % 2 ? "flat" : "tree");
            QCOMPARE(window().treeView->model(), proxy);
            QCOMPARE(window().treeView->selectionModel(), selection);
            QCOMPARE(window().filteredIds().size(), 3);
            QCOMPARE(window().selectedIds(), QStringList{"candidate-1"});
        }
        window().setViewMode("tree");
        QVERIFY(window().groupIndex("Root / ModuleA").isValid());
        QCOMPARE(
            window().candidateIndex("candidate-1").siblingAtColumn(StartTime).data().toString(),
            QString("2026-01-01 10:00:01"));
        QCOMPARE(
            window().candidateIndex("candidate-0").siblingAtColumn(StartTime).data().toString(),
            QString("—"));
        QCOMPARE(
            window().candidateIndex("candidate-2").siblingAtColumn(StartTime).data().toString(),
            QString("—"));
    }
    void treeParentTriState() {
        window().setViewMode("tree");
        auto root = window().groupIndex("Root");
        window().treeView->setCurrentIndex(root.siblingAtColumn(SuitePath));
        QCOMPARE(root.data(Qt::CheckStateRole).toInt(), int(Qt::PartiallyChecked));
        QVERIFY(window().proxy->setData(root, Qt::Checked, Qt::CheckStateRole));
        QTRY_COMPARE(window().selectedIds().size(), 3);
        QCOMPARE(window().treeView->currentIndex().siblingAtColumn(Check),
                 window().groupIndex("Root"));
        QCOMPARE(window().setChecked({"candidate-0"}, false), 1);
        QCOMPARE(window().groupIndex("Root / ModuleA").data(Qt::CheckStateRole).toInt(),
                 int(Qt::PartiallyChecked));
        QVERIFY(window().proxy->setData(window().groupIndex("Root"), Qt::Unchecked,
                                        Qt::CheckStateRole));
        QTRY_VERIFY(window().selectedIds().isEmpty());
    }
    void filteredSelectionRemainsIndependent() {
        window().applySelection({"candidate-0", "candidate-1", "candidate-2"});
        for (const auto &mode : {"flat", "tree"}) {
            window().setViewMode(mode);
            window().setFilters(false, true, "PASS", "");
            QCOMPARE(window().filteredIds(), QStringList{"candidate-1"});
            QCOMPARE(window().selectedIds().size(), 3);
            window().invertFiltered();
            QCOMPARE(window().selectedIds().size(), 2);
            QVERIFY(window().filteredIds().isEmpty());
            window().restoreDefaultSelection();
            QCOMPARE(window().selectedIds(), QStringList{"candidate-1"});
            window().applySelection({"candidate-0", "candidate-1", "candidate-2"});
        }
        window().setFilters(true, false, "ALL", "");
        QCOMPARE(window().filteredIds().size(), 2);
        window().setFilters(false, false, "ALL", "ModuleA");
        QCOMPARE(window().filteredIds().size(), 2);
        window().setFilters(false, false, "ALL", "Root");
        QCOMPARE(window().filteredIds().size(), 3);
        window().setFilters(false, false, "ALL", "file2");
        QCOMPARE(window().filteredIds(), QStringList{"candidate-2"});
    }
    void checkedOnlyEditIsDeferredAndSafe() {
        window().setViewMode("tree");
        window().applySelection({"candidate-0", "candidate-1"});
        window().setFilters(false, true, "ALL", "");
        auto index = window().candidateIndex("candidate-0");
        QVERIFY(window().proxy->setData(index, Qt::Unchecked, Qt::CheckStateRole));
        QTRY_COMPARE(window().filteredIds(), QStringList{"candidate-1"});
        for (int i = 0; i < 20; ++i) {
            window().setViewMode("flat");
            window().setViewMode("tree");
            QCOMPARE(window().filteredIds().size(), 1);
        }
    }
    void contextScopeMatchesClickedRow() {
        auto first = window().candidateIndex("candidate-0").siblingAtColumn(SuitePath);
        auto second = window().candidateIndex("candidate-1").siblingAtColumn(SuitePath);
        auto third = window().candidateIndex("candidate-2").siblingAtColumn(SuitePath);
        auto *selection = window().treeView->selectionModel();
        selection->select(first, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        selection->select(second, QItemSelectionModel::Select | QItemSelectionModel::Rows);
        QCOMPARE(window().contextSelection(window().treeView->visualRect(second).center()).size(),
                 2);
        QCOMPARE(window().contextSelection(window().treeView->visualRect(third).center()),
                 QStringList{"candidate-2"});
        QVERIFY(window()
                    .contextSelection(QPoint(5, window().treeView->viewport()->height() - 1))
                    .isEmpty());
        std::unique_ptr<QMenu> menu(
            window().buildContextMenu({"candidate-0", "candidate-1"}, "candidate-1"));
        QStringList actions;
        for (auto *action : menu->actions())
            actions.push_back(action->text());
        for (const auto &text : {"勾选", "反勾选", "全选当前过滤", "反选当前过滤", "查看详情",
                                 "打开文件所在目录", "导出该测试 XML",
                                 "合并选中结果到指定目录…", "打开输出目录"})
            QVERIFY(actions.contains(text));
        const auto screenshot = qEnvironmentVariable("XML_MERGE_CONTEXT_SCREENSHOT");
        if (!screenshot.isEmpty()) {
            menu->popup(window().treeView->viewport()->mapToGlobal(
                window().treeView->visualRect(second).center()));
            QTest::qWait(20);
            QVERIFY(menu->grab().save(screenshot));
            menu->close();
        }
        menu->actions()[0]->trigger();
        QCOMPARE(window().selectedIds().size(), 2);
        menu->actions()[1]->trigger();
        QVERIFY(window().selectedIds().isEmpty());
    }
    void contextOpensClickedSourceDirectory() {
        const auto directory = temporary_.filePath("源文件 空格 #");
        QVERIFY(QDir().mkpath(directory));
        auto items = samples();
        items[1].file_path = QDir(directory).filePath("output.xml").toUtf8().toStdString();
        window().populateCandidates(items, {{"candidate-0"}, {}});
        UrlCapture capture;
        std::unique_ptr<QMenu> menu(
            window().buildContextMenu({"candidate-0", "candidate-1"}, "candidate-1"));
        QAction *open = nullptr;
        for (auto *action : menu->actions())
            if (action->text() == "打开文件所在目录")
                open = action;
        QVERIFY(open && open->isEnabled());
        open->trigger();
        QCOMPARE(capture.opened.toLocalFile(), directory);
        // The source file can have been removed; its containing directory still opens.
        QVERIFY(QDir().rmdir(directory));
        QSignalSpy errors(&window(), &MainWindow::errorOccurred);
        open->trigger();
        QCOMPARE(errors.size(), 1);
        QVERIFY(errors.front().front().toString().contains(directory));
        std::unique_ptr<QMenu> emptyMenu(window().buildContextMenu({}, {}));
        for (auto *action : emptyMenu->actions())
            if (action->text() == "打开文件所在目录" ||
                action->text() == "合并选中结果到指定目录…")
                QVERIFY(!action->isEnabled());
    }
    void contextMergesHighlightedResultsToChosenDirectory() {
        const auto source = temporary_.filePath("context-first.xml");
        const auto other = temporary_.filePath("context-second.xml");
        const QByteArray sourceXml =
            "<robot schemaversion=\"4\"><suite name=\"First\" id=\"s1\">"
            "<test name=\"First case\"><kw name=\"Keep keyword\"><msg level=\"INFO\">"
            "Keep message</msg></kw><status status=\"PASS\"/></test>"
            "<suite name=\"Nested\" id=\"s1-s1\"><test name=\"Nested case\">"
            "<status status=\"SKIP\"/></test><status status=\"SKIP\"/></suite>"
            "<status status=\"PASS\"/></suite></robot>";
        const QByteArray otherXml =
            "<robot schemaversion=\"4\"><suite name=\"Root\" id=\"s1\">"
            "<suite name=\"Second\" id=\"s1-s1\"><test name=\"Second case\">"
            "<status status=\"FAIL\"/></test><status status=\"FAIL\"/></suite>"
            "<suite name=\"Excluded\" id=\"s1-s2\"><test name=\"Excluded case\">"
            "<status status=\"PASS\"/></test><status status=\"PASS\"/></suite>"
            "<status status=\"FAIL\"/></suite></robot>";
        writeFile(source, sourceXml);
        writeFile(other, otherXml);
        window().clearCandidates();
        QSignalSpy finished(&window(), &MainWindow::taskFinished);
        window().loadFiles({source, other});
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
        QCOMPARE(window().candidates().size(), std::size_t(4));
        QStringList highlighted, checked;
        for (const auto &item : window().candidates()) {
            const auto id = QString::fromStdString(item.candidate_id);
            if (item.suite_name == "First" || item.suite_name == "Second")
                highlighted.push_back(id);
            else if (item.suite_name == "Excluded")
                checked.push_back(id);
        }
        QCOMPARE(highlighted.size(), 2);
        QCOMPARE(checked.size(), 1);
        window().applySelection(checked);
        const auto originalOutputDir = window().outputDir();
        auto *selection = window().treeView->selectionModel();
        selection->clearSelection();
        for (const auto &id : highlighted)
            selection->select(window().candidateIndex(id),
                              QItemSelectionModel::Select | QItemSelectionModel::Rows);
        const auto point = window().treeView->visualRect(
            window().candidateIndex(highlighted.back()).siblingAtColumn(SuitePath)).center();
        const auto contextIds = window().contextSelection(point);
        QCOMPARE(contextIds, highlighted);
        std::unique_ptr<QMenu> menu(window().buildContextMenu(contextIds, highlighted.back()));
        QAction *exportXml = nullptr;
        for (auto *action : menu->actions())
            if (action->text() == "合并选中结果到指定目录…")
                exportXml = action;
        QVERIFY(exportXml && exportXml->isEnabled());
        QSignalSpy exported(&window(), &MainWindow::selectedXmlExported);
        // Cancel through the real menu/dialog path before selecting a directory.
        bool dialogSeen = false;
        QTimer::singleShot(0, &window(), [&] {
            if (auto *dialog = window().findChild<QFileDialog *>()) {
                dialogSeen = true;
                dialog->reject();
            }
        });
        exportXml->trigger();
        QVERIFY(dialogSeen);
        QVERIFY(!window().isBusy());
        QCOMPARE(finished.size(), 1);
        QVERIFY(exported.isEmpty());
        const auto directory = temporary_.filePath("指定导出 空格");
        QVERIFY(QDir().mkpath(directory));
        QTimer::singleShot(0, &window(), [&] {
            if (auto *dialog = window().findChild<QFileDialog *>()) {
                dialog->setDirectory(directory);
                QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
            }
        });
        exportXml->trigger();
        QVERIFY(window().isBusy());
        std::unique_ptr<QMenu> busyMenu(window().buildContextMenu(contextIds, highlighted.back()));
        for (auto *action : busyMenu->actions())
            if (!action->isSeparator())
                QVERIFY(!action->isEnabled());
        QTRY_COMPARE_WITH_TIMEOUT(exported.size(), 1, 10000);
        const auto output = exported.front().front().toString();
        QCOMPARE(QFileInfo(output).absolutePath(), directory);
        QVERIFY(QFileInfo(output).fileName().startsWith("merged-"));
        QCOMPARE(QDir(directory).entryList(QDir::Files), QStringList{QFileInfo(output).fileName()});
        QFile merged(output);
        QVERIFY(merged.open(QIODevice::ReadOnly));
        const auto mergedXml = merged.readAll();
        QXmlStreamReader reader(mergedXml);
        QStringList names;
        while (!reader.atEnd()) {
            reader.readNext();
            if (reader.isStartElement() && reader.name() == QLatin1String("test"))
                names.push_back(reader.attributes().value("name").toString());
        }
        QVERIFY(!reader.hasError());
        QCOMPARE(names, QStringList({"First case", "Nested case", "Second case"}));
        QVERIFY(mergedXml.contains("Keep keyword"));
        QVERIFY(mergedXml.contains("Keep message"));
        QCOMPARE(window().selectedIds(), checked);
        QCOMPARE(window().outputDir(), originalOutputDir);
        window().exportSelectedXml(contextIds, directory);
        QTRY_COMPARE_WITH_TIMEOUT(exported.size(), 2, 10000);
        QVERIFY(exported.back().front().toString() != output);
        merged.seek(0);
        QCOMPARE(merged.readAll(), mergedXml);
        QFile firstSource(source), secondSource(other);
        QVERIFY(firstSource.open(QIODevice::ReadOnly));
        QVERIFY(secondSource.open(QIODevice::ReadOnly));
        QCOMPARE(firstSource.readAll(), sourceXml);
        QCOMPARE(secondSource.readAll(), otherXml);
    }
    void selectedXmlExportFailureLeavesNoOutput() {
        const auto source = temporary_.filePath("failure-source.xml");
        writeFile(source);
        window().clearCandidates();
        QSignalSpy finished(&window(), &MainWindow::taskFinished);
        QSignalSpy exported(&window(), &MainWindow::selectedXmlExported);
        QSignalSpy errors(&window(), &MainWindow::errorOccurred);
        window().loadFiles({source});
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
        const auto ids = window().selectedIds();
        const auto directory = temporary_.filePath("failed-export");
        window().exportSelectedXml({ids.front(), "stale-id"}, directory);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 10000);
        QCOMPARE(finished.back().back().toBool(), false);
        QVERIFY(!QFileInfo::exists(directory));
        // A file cannot be used as the target directory, and must be preserved.
        window().exportSelectedXml(ids, source);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 3, 10000);
        QCOMPARE(finished.back().back().toBool(), false);
        QCOMPARE(errors.size(), 2);
        QVERIFY(exported.isEmpty());
        QVERIFY(!window().isBusy());
        QCOMPARE(MergeService{}.load_candidates({source.toUtf8().toStdString()}).candidates.size(),
                 std::size_t(1));
    }
    void rubberSelectionAcrossScrollAndTree() {
        auto items = samples();
        for (int i = 3; i < 100; ++i) {
            auto item = items.front();
            item.candidate_id = "candidate-" + std::to_string(i);
            item.suite_path += " " + std::to_string(i);
            items.push_back(item);
        }
        window().populateCandidates(items, {});
        window().resize(1100, 700);
        QTest::qWait(20);
        auto anchor = window().proxy->index(0, 0);
        auto target = window().proxy->index(5, SuitePath);
        window().treeView->selectRubberRange(anchor,
                                             window().treeView->visualRect(target).center());
        QCOMPARE(window().treeView->selectionModel()->selectedRows().size(), 6);
        window().treeView->scrollTo(window().proxy->index(45, SuitePath),
                                    QAbstractItemView::PositionAtTop);
        target = window().proxy->index(48, SuitePath);
        window().treeView->selectRubberRange(anchor,
                                             window().treeView->visualRect(target).center());
        QCOMPARE(window().treeView->selectionModel()->selectedRows().size(), 49);
        window().setViewMode("tree");
        auto treeAnchor = window().groupIndex("Root");
        auto leaf = window().candidateIndex("candidate-2");
        window().treeView->scrollTo(leaf);
        window().treeView->selectRubberRange(treeAnchor,
                                             window().treeView->visualRect(leaf).center());
        QVERIFY(window().treeView->selectionModel()->selectedRows().size() > 3);
    }
    void headerPreferencesPerMode() {
        QVERIFY(window().treeView->isColumnHidden(ConflictSize));
        window().setColumnVisible(File, false);
        window().setColumnVisible(ConflictSize, true);
        window().setViewMode("tree");
        QVERIFY(window().treeView->isColumnHidden(ConflictSize));
        QVERIFY(!window().treeView->isColumnHidden(File));
        window().setColumnVisible(Status, false);
        window().treeView->setColumnWidth(SuitePath, 333);
        window().close();
        window_.reset();
        window_ = std::make_unique<MainWindow>(nullptr, MainWindow::Loader{}, settings());
        window().setInteractive(false);
        QCOMPARE(window().viewMode(), QString("tree"));
        QVERIFY(window().treeView->isColumnHidden(Status));
        QCOMPARE(window().treeView->columnWidth(SuitePath), 333);
        window().setViewMode("flat");
        QVERIFY(window().treeView->isColumnHidden(File));
        QVERIFY(!window().treeView->isColumnHidden(ConflictSize));
        QVERIFY(!window().treeView->isColumnHidden(Status));
    }
    void optionsAndDetailDialog() {
        window().findChild<QComboBox *>("LogLevelCombo")->setCurrentText("DEBUG");
        window().findChild<QComboBox *>("VisibleLogLevelCombo")->setCurrentText("INFO");
        window().findChild<QComboBox *>("RemoveKeywordsCombo")->setCurrentIndex(1);
        window().findChild<QComboBox *>("FlattenKeywordsCombo")->setCurrentIndex(3);
        window().findChild<QCheckBox *>("SplitLogCheckBox")->setChecked(true);
        window().findChild<QSpinBox *>("SuiteStatLevelSpin")->setValue(4);
        window().findChild<QLineEdit *>("LogTitleEdit")->setText("合并日志");
        window().findChild<QLineEdit *>("ReportTitleEdit")->setText("合并报告");
        window().findChild<QLineEdit *>("ReportBackgroundEdit")->setText("green:red:yellow");
        const auto options = window().reportOptions();
        QCOMPARE(options.level, std::string("DEBUG"));
        QCOMPARE(options.visible, std::string("INFO"));
        QVERIFY(options.split_log);
        QCOMPARE(options.suite_stat_level, 4);
        QCOMPARE(options.remove_keywords, std::string("passed"));
        QCOMPARE(options.flatten_keywords, std::string("iteration"));
        QCOMPARE(options.logtitle, std::string("合并日志"));
        QCOMPARE(options.reporttitle, std::string("合并报告"));
        QCOMPARE(options.report_background, std::string("green:red:yellow"));
        auto *dialog = window().findChild<QDialog *>("ReportOptionsDialog");
        QVERIFY(!dialog->isVisible());
        window().findChild<QPushButton *>("ReportOptionsButton")->click();
        QVERIFY(dialog->isVisible());
        dialog->hide();
        window().showDetail("candidate-1");
        QVERIFY(window().findChild<QDialog *>("DetailDialog")->isVisible());
        window().setFilters(false, true, "ALL", "");
        QCOMPARE(window().filteredIds().size(), 1);
        // Observe double-click routing without generating a file in the fixture.
        disconnect(&window(), &MainWindow::candidateDoubleClicked, &window(),
                   &MainWindow::openSingleLog);
        QSignalSpy doubleClick(&window(), &MainWindow::candidateDoubleClicked);
        window().setViewMode("tree");
        window().handleDoubleClick(window().groupIndex("Root").siblingAtColumn(SuitePath));
        window().handleDoubleClick(window().candidateIndex("candidate-1"));
        QCOMPARE(doubleClick.size(), 0);
        window().handleDoubleClick(
            window().candidateIndex("candidate-1").siblingAtColumn(SuitePath));
        QCOMPARE(doubleClick.size(), 1);
    }
    void allReportOptionChoicesMatchPython() {
        auto *level = window().findChild<QComboBox *>("LogLevelCombo");
        auto *visible = window().findChild<QComboBox *>("VisibleLogLevelCombo");
        auto *remove = window().findChild<QComboBox *>("RemoveKeywordsCombo");
        auto *flatten = window().findChild<QComboBox *>("FlattenKeywordsCombo");
        const auto defaults = window().reportOptions();
        QCOMPARE(defaults.level, std::string("TRACE"));
        QCOMPARE(defaults.visible, std::string("TRACE"));
        QVERIFY(!defaults.split_log);
        QCOMPARE(defaults.suite_stat_level, -1);
        QVERIFY(defaults.remove_keywords.empty());
        QVERIFY(defaults.flatten_keywords.empty());
        QVERIFY(defaults.logtitle.empty());
        QVERIFY(defaults.reporttitle.empty());
        QVERIFY(defaults.report_background.empty());
        QCOMPARE(level->count(), 5);
        QCOMPARE(visible->count(), 5);
        QCOMPARE(remove->count(), 6);
        QCOMPARE(flatten->count(), 4);
        for (const auto &value : {"TRACE", "DEBUG", "INFO", "WARN", "NONE"}) {
            level->setCurrentText(value);
            for (int index = 0; index < visible->count(); ++index) {
                visible->setCurrentIndex(index);
                const auto expected = index == 0 || level->currentText() == "NONE"
                                          ? level->currentText()
                                          : visible->currentText();
                QCOMPARE(window().reportOptions().level, std::string(value));
                QCOMPARE(window().reportOptions().visible, expected.toUtf8().toStdString());
            }
        }
        int index = 0;
        for (const auto &value : {"", "passed", "wuks", "for", "while", "all"}) {
            remove->setCurrentIndex(index++);
            QCOMPARE(window().reportOptions().remove_keywords, std::string(value));
        }
        index = 0;
        for (const auto &value : {"", "for", "while", "iteration"}) {
            flatten->setCurrentIndex(index++);
            QCOMPARE(window().reportOptions().flatten_keywords, std::string(value));
        }
        for (const auto &name :
             {"ViewModeCombo", "LogLevelCombo", "VisibleLogLevelCombo", "SplitLogCheckBox",
              "SuiteStatLevelSpin", "RemoveKeywordsCombo", "FlattenKeywordsCombo", "LogTitleEdit",
              "ReportTitleEdit", "ReportBackgroundEdit"})
            QVERIFY2(!window().findChild<QWidget *>(name)->toolTip().isEmpty(), name);
        QVERIFY(!QApplication::windowIcon().isNull());
    }
    void appendReloadAndFailureRemoveStaleData() {
        const auto first = temporary_.filePath("first.xml"),
                   second = temporary_.filePath("second.xml"),
                   empty = temporary_.filePath("empty.xml");
        writeFile(first);
        writeFile(second);
        writeFile(empty, "<robot><suite name=\"Empty\"><status status=\"PASS\"/></suite></robot>");
        window().clearCandidates();
        QSignalSpy finished(&window(), &MainWindow::taskFinished);
        window().loadFiles({first});
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
        QCOMPARE(window().candidates().size(), std::size_t(1));
        window().loadFiles({second});
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 10000);
        QCOMPARE(window().candidates().size(), std::size_t(2));
        QCOMPARE(window().activeFilePaths(), QStringList({first, second}));
        window().loadFiles({empty});
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 3, 10000);
        QVERIFY(window().activeFilePaths().contains(empty));
        QCOMPARE(window().candidates().size(), std::size_t(2));
        writeFile(first, "invalid no xml");
        window().loadFiles({first});
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 4, 10000);
        QCOMPARE(window().candidates().size(), std::size_t(1));
        QVERIFY(!window().activeFilePaths().contains(first));
        QVERIFY(window().activeFilePaths().contains(empty));
        writeFile(second, "<robot><suite name=\"Updated\"><test name=\"Updated test\"><status "
                          "status=\"SKIP\"/></test><status status=\"SKIP\"/></suite></robot>");
        window().setViewMode("tree");
        window().loadFiles({second});
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 5, 10000);
        QCOMPARE(window().candidates().size(), std::size_t(1));
        QCOMPARE(window().candidates()[0].suite_name, std::string("Updated"));
        QCOMPARE(window().filteredIds().size(), 1);
    }
    void nativePathSeparatorsPreserveAppendAndReloadOrder() {
        const auto first = temporary_.filePath("native-first.xml");
        const auto second = temporary_.filePath("native-second.xml");
        writeFile(first);
        writeFile(second);
        auto received = std::make_shared<std::vector<std::string>>();
        window_.reset();
        window_ = std::make_unique<MainWindow>(
            nullptr,
            [received](const std::vector<std::string> &paths) {
                *received = paths;
                LoadBatchResult result;
                result.parsed_file_count = static_cast<int>(paths.size());
                // std::filesystem returns native backslashes on Windows;
                // QFileInfo and Qt dialogs use forward slash paths.
                for (const auto &path : paths)
                    result.successful_file_paths.push_back(
                        QDir::toNativeSeparators(QString::fromUtf8(path.data(), path.size()))
                            .toUtf8()
                            .toStdString());
                return result;
            },
            settings());
        window().setInteractive(false);
        QSignalSpy finished(&window(), &MainWindow::taskFinished);
        window().loadFiles({first});
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
        window().loadFiles({second});
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 10000);
        QCOMPARE(window().activeFilePaths(), QStringList({first, second}));
        // Reloading must replace the old path and put it last, preserving the
        // input-order tie-breaker; native separators must not create duplicates.
        window().loadFiles({QDir::toNativeSeparators(first)});
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 3, 10000);
        QCOMPARE(window().activeFilePaths(), QStringList({second, first}));
        QCOMPARE(received->size(), std::size_t(2));
        QCOMPARE((*received)[0], second.toUtf8().toStdString());
        QCOMPARE((*received)[1], first.toUtf8().toStdString());
    }
    void excelIncludesHiddenSelectionAndBusyState() {
        window().applySelection({"candidate-0", "candidate-1", "candidate-2"});
        window().setFilters(false, false, "PASS", "");
        const auto path = temporary_.filePath("selected.xlsx");
        QSignalSpy exported(&window(), &MainWindow::excelExported);
        window().exportExcel(path);
        QVERIFY(window().isBusy());
        QVERIFY(!window().findChild<QPushButton *>("ExportExcelButton")->isEnabled());
        QTRY_COMPARE_WITH_TIMEOUT(exported.size(), 1, 10000);
        QVERIFY(QFileInfo::exists(path));
        QVERIFY(!window().isBusy());
        QVERIFY(window().findChild<QPushButton *>("ExportExcelButton")->isEnabled());
        QCOMPARE(window().selectedIds().size(), 3);
        QCOMPARE(MainWindow::normalizeXlsxPath("a.csv"), QString("a.xlsx"));
        QCOMPARE(MainWindow::normalizeXlsxPath("a.XLSX"), QString("a.XLSX"));
        QCOMPARE(MainWindow::normalizeXlsxPath("a."), QString("a.xlsx"));
        QCOMPARE(MainWindow::normalizeXlsxPath(".xlsx"), QString(".xlsx.xlsx"));
        QCOMPARE(MainWindow::normalizeXlsxPath(".hidden.csv"), QString(".hidden.xlsx"));
        QCOMPARE(MainWindow::safeExportName("CON"), QString("_CON"));
        QCOMPARE(MainWindow::safeExportName("a/b:*"), QString("a_b__"));
        const auto emoji = QString::fromUtf8("😀");
        QCOMPARE(MainWindow::safeExportName(QString("A") + emoji.repeated(150)),
                 QString("A") + emoji.repeated(119));
    }
    void closeDuringLoadFinishesSafely() {
        const auto path = temporary_.filePath("closing.xml");
        writeFile(path);
        auto done = std::make_shared<std::atomic_bool>(false);
        window_.reset();
        window_ = std::make_unique<MainWindow>(
            nullptr,
            [done](const std::vector<std::string> &) {
                std::this_thread::sleep_for(std::chrono::milliseconds(120));
                done->store(true);
                return LoadBatchResult{};
            },
            settings());
        window().setInteractive(false);
        window().show();
        window().loadFiles({path});
        QVERIFY(window().isBusy());
        QVERIFY(!window().close());
        QVERIFY(window().isVisible());
        QTRY_VERIFY(done->load());
        QTRY_VERIFY(!window().isVisible());
        QVERIFY(!window().isBusy());
    }
    void accessibilityOptIn() {
        QVERIFY(shouldDisableMacAccessibility("darwin", 26, {}));
        QVERIFY(!shouldDisableMacAccessibility("darwin", 25, {}));
        QVERIFY(!shouldDisableMacAccessibility("linux", 26, {}));
        for (const auto &value : {"1", "true", " TRUE ", "yes", "on"})
            QVERIFY(!shouldDisableMacAccessibility("darwin", 26, value));
        QVERIFY(shouldDisableMacAccessibility("darwin", 26, "false"));
    }
    void lightThemeControlsAndFooter() {
        QCOMPARE(QApplication::palette().color(QPalette::Window), QColor("#f3f5f8"));
        QCOMPARE(QApplication::palette().color(QPalette::Text), QColor("#111827"));
        // Offscreen has no platform color-scheme API; its explicit palette is
        // still checked above. Native platforms must honor the light request.
        if (QGuiApplication::platformName() != "offscreen" &&
            QGuiApplication::platformName() != "minimal")
            QCOMPARE(QGuiApplication::styleHints()->colorScheme(), Qt::ColorScheme::Light);
        QVERIFY(!QPixmap(":/ui/check.png").isNull());
        QVERIFY(!QPixmap(":/ui/partial.png").isNull());
        QVERIFY(!QPixmap(":/ui/down.png").isNull());
        window().setOutputDir("/Users/example/a very long output path/" +
                              QString("directory/").repeated(30));
        window().setViewMode("tree");
        QTest::qWait(20);
        QCOMPARE(window().treeView->treePosition(), int(SuitePath));
        auto *selected = window().findChild<QCheckBox *>("SelectedOnlyCheckBox");
        auto *compact = window().findChild<QCheckBox *>("CompactCheckBox");
        auto *output = window().findChild<QLabel *>("OutputDirLabel");
        QVERIFY(output->geometry().left() - compact->geometry().right() >= 20);
        QVERIFY(output->text().contains(QChar(0x2026)));
        QCOMPARE(output->toolTip(), window().outputDir());
        QCOMPARE(window().width(), 1320);
        QVERIFY(selected->isVisible());
        const auto screenshot = qEnvironmentVariable("XML_MERGE_UI_SCREENSHOT");
        if (!screenshot.isEmpty())
            QVERIFY(window().grab().save(screenshot));
    }
};
int main(int argc, char **argv) {
    configureHighDpi();
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    configureAppAppearance();
    configureAppFont();
    app.setQuitOnLastWindowClosed(false);
    UiTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "ui_tests.moc"
