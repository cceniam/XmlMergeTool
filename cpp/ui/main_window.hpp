#pragma once

#include "candidate_view.hpp"
#include "merge_service.hpp"
#include <QFutureWatcher>
#include <QMainWindow>
#include <QSet>
#include <QSettings>
#include <QStandardItemModel>
#include <functional>
#include <memory>

class QCheckBox;
class QComboBox;
class QDialog;
class QLabel;
class QLineEdit;
class QMenu;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;

namespace xm::ui {

class MainWindow final : public QMainWindow {
    Q_OBJECT
  public:
    using Loader = std::function<LoadBatchResult(const std::vector<std::string> &)>;
    explicit MainWindow(QWidget *parent = nullptr, Loader loader = {},
                        const QString &settingsFile = {});
    ~MainWindow() override;
    void setInteractive(bool enabled) {
        interactive_ = enabled;
    }
    bool isBusy() const {
        return busy_;
    }
    void loadFiles(const QStringList &paths);
    void clearCandidates();
    void populateCandidates(std::vector<SuiteCandidate> candidates,
                            const SelectionSuggestion &suggestion);
    const std::vector<SuiteCandidate> &candidates() const {
        return candidates_;
    }
    QStringList activeFilePaths() const {
        return activePaths_;
    }
    QStringList selectedIds() const;
    QStringList filteredIds() const;
    void applySelection(const QStringList &ids);
    int setChecked(const QStringList &ids, bool checked);
    void selectAllFiltered();
    void invertFiltered();
    void restoreDefaultSelection();
    void setViewMode(const QString &mode);
    QString viewMode() const {
        return mode_;
    }
    void setFilters(bool conflicts, bool selected, const QString &status, const QString &search);
    QModelIndex candidateIndex(const QString &id) const;
    QModelIndex groupIndex(const QString &path) const;
    QString currentCandidateId() const;
    QStringList contextSelection(const QPoint &position);
    QMenu *buildContextMenu(const QStringList &ids, const QString &clickedId);
    void handleDoubleClick(const QModelIndex &index);
    void showDetail(const QString &id);
    void setOutputDir(const QString &path);
    QString outputDir() const {
        return outputDir_;
    }
    rf::Options reportOptions() const;
    void mergeSelected();
    void exportExcel(const QString &outputPath);
    void exportSingleXml(const QString &id, const QString &path);
    void exportSelectedXml(const QStringList &ids, const QString &directory);
    void openSingleLog(const QString &id);
    void setColumnVisible(int column, bool visible);
    static QString normalizeXlsxPath(const QString &path);
    static QString safeExportName(QString name);

    CandidateView *treeView = nullptr;
    QStandardItemModel *model = nullptr;
    CandidateFilter *proxy = nullptr;
  signals:
    void selectionChanged();
    void candidatesLoaded();
    void candidateDoubleClicked(const QString &id);
    void errorOccurred(const QString &message);
    void taskFinished(const QString &kind, bool success);
    void merged(const QString &xml, const QString &log, const QString &report,
                const QString &conflict);
    void excelExported(const QString &path);
    void singleXmlExported(const QString &path);
    void selectedXmlExported(const QString &path);
    void singleLogReady(const QString &path);

  protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *object, QEvent *event) override;

  private:
    struct JobResult {
        QString kind, error, path;
        int selectedCount = 0;
        LoadBatchResult loaded;
        SelectionSuggestion suggestion;
        MergeResult merged;
        ExcelExportResult excel;
    };
    void buildUi();
    void buildDialogs();
    void rebuildModel();
    void refreshFilter();
    void queueSelectionRefresh();
    void refreshSummary();
    void refreshDetail();
    void syncCheckStates();
    std::pair<int, int> syncGroup(QStandardItem *item);
    QStringList descendantIds(const QModelIndex &sourceIndex) const;
    void itemChanged(QStandardItem *item);
    void ensureCurrent(const QString &preferred = {});
    void setBusy(bool busy, const QString &message = {});
    bool startJob(const QString &kind, const QString &message, std::function<JobResult()> job);
    void jobFinished();
    void showError(const QString &message);
    void showInfo(const QString &title, const QString &message);
    void chooseOutputDir();
    void chooseExcelPath();
    void chooseSingleXml(const QString &id);
    void chooseSelectedXml(const QStringList &ids);
    void openPath(const QString &path);
    void saveHeader();
    void loadHeader();
    void savePreferences();
    void showDialog(QDialog *dialog, const QString &key, int offset);
    const SuiteCandidate *candidate(const QString &id) const;

    std::unique_ptr<QSettings> settings_;
    MergeService service_;
    Loader loader_;
    QFutureWatcher<JobResult> watcher_;
    std::vector<SuiteCandidate> candidates_;
    SelectionSuggestion suggestion_;
    QHash<QString, QStandardItem *> checks_;
    QHash<QString, int> candidatePositions_, groupSizes_;
    QSet<QString> selected_, defaults_;
    QStringList activePaths_;
    QString mode_ = "flat", outputDir_;
    int parsedCount_ = 0, failedCount_ = 0;
    bool busy_ = false, closing_ = false, interactive_ = true, syncing_ = false;
    bool rebuilding_ = false, restoringHeader_ = false, refreshQueued_ = false;
    QVector<QWidget *> actionWidgets_;
    QCheckBox *conflictOnly_, *selectedOnly_, *compact_, *splitLog_;
    QComboBox *viewMode_, *statusFilter_, *logLevel_, *visibleLevel_, *removeKeywords_,
        *flattenKeywords_;
    QLineEdit *search_, *logTitle_, *reportTitle_, *reportBackground_;
    QSpinBox *suiteStatLevel_;
    QLabel *summary_, *visibleCount_, *outputLabel_, *detailStatus_;
    QLabel *detailSelected_, *detailDefault_, *detailPath_, *detailFile_, *detailFilePath_,
        *detailStart_;
    QPlainTextEdit *detailTests_, *detailFailures_;
    QProgressBar *progress_;
    QDialog *detailDialog_, *optionsDialog_;
};

void configureHighDpi();
void configureAppAppearance();
void configureAppFont();
void configureLogging();
bool shouldDisableMacAccessibility(const QString &platform, int majorVersion,
                                   const QByteArray &optIn);
bool disableMacWindowAccessibility(QWidget *window);

} // namespace xm::ui
