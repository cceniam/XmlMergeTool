#include "main_window.hpp"
#include <QApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QTimer>
#include <csignal>
#include <cstdio>

namespace {
volatile std::sig_atomic_t exitRequested = 0;
void handleSignal(int) {
    exitRequested = 1;
}
QByteArray hashFile(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    return hash.addData(&file) ? hash.result().toHex() : QByteArray();
}
} // namespace
int main(int argc, char **argv) {
    xm::ui::configureHighDpi();
    QApplication app(argc, argv);
    QApplication::setApplicationName("XmlMergeTool");
    QApplication::setOrganizationName("Robotframework-XMLMergeTool");
    xm::ui::configureLogging();
    xm::ui::configureAppAppearance();
    xm::ui::configureAppFont();
    const auto args = app.arguments();
    const int smoke = args.indexOf("--smoke-test");
    std::unique_ptr<QTemporaryDir> smokeSettings;
    if (smoke >= 0) {
        smokeSettings = std::make_unique<QTemporaryDir>();
        if (!smokeSettings->isValid()) {
            std::fprintf(stderr, "Unable to create isolated smoke-test settings directory\n");
            return 2;
        }
    }
    xm::ui::MainWindow window(nullptr, {},
                              smokeSettings ? smokeSettings->filePath("settings.ini") : QString());
    xm::ui::disableMacWindowAccessibility(&window);
    window.show();
    QTimer signalTimer;
    QObject::connect(&signalTimer, &QTimer::timeout, &window, [&window] {
        if (exitRequested)
            window.close();
    });
    signalTimer.start(200);
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
    if (smoke >= 0) {
        if (smoke + 2 >= args.size()) {
            std::fprintf(stderr, "Usage: XmlMergeTool --smoke-test INPUT_XML OUTPUT_DIR\n");
            return 2;
        }
        const QString input = QFileInfo(args[smoke + 1]).absoluteFilePath();
        const QString directory = QFileInfo(args[smoke + 2]).absoluteFilePath();
        if (!QDir().mkpath(directory))
            return 2;
        window.setInteractive(false);
        window.setOutputDir(directory);
        const QByteArray originalHash = hashFile(input);
        auto results = std::make_shared<QJsonObject>();
        results->insert("input", input);
        results->insert("runtime", "C++17 / Qt " + QString(qVersion()));
        results->insert("input_sha256", QString::fromLatin1(originalHash));
        auto finished = std::make_shared<bool>(false);
        auto finish = [&, results, finished, directory, input, originalHash](bool success,
                                                                             const QString &error) {
            if (*finished)
                return;
            *finished = true;
            const bool sourceUnchanged = !originalHash.isEmpty() && hashFile(input) == originalHash;
            success = success && sourceUnchanged;
            results->insert("success", success);
            results->insert("source_unchanged", sourceUnchanged);
            results->insert("error", error);
            QSaveFile file(QDir(directory).filePath("smoke-results.json"));
            bool written = file.open(QIODevice::WriteOnly);
            if (written) {
                const auto json = QJsonDocument(*results).toJson();
                written = file.write(json) == json.size() && file.commit();
            }
            std::fprintf(stdout, "%s\n",
                         QJsonDocument(*results).toJson(QJsonDocument::Compact).constData());
            QCoreApplication::exit(success && written ? 0 : 1);
        };
        QObject::connect(&window, &xm::ui::MainWindow::errorOccurred, &window,
                         [finish](const QString &error) { finish(false, error); });
        QObject::connect(&window, &xm::ui::MainWindow::merged, &window,
                         [results](const QString &xml, const QString &log, const QString &report,
                                   const QString &conflict) {
                             results->insert("merged_xml", xml);
                             results->insert("log", log);
                             results->insert("report", report);
                             results->insert("conflict_csv", conflict);
                         });
        QObject::connect(&window, &xm::ui::MainWindow::excelExported, &window,
                         [results](const QString &path) { results->insert("excel", path); });
        QObject::connect(&window, &xm::ui::MainWindow::singleXmlExported, &window,
                         [results](const QString &path) { results->insert("single_xml", path); });
        QObject::connect(&window, &xm::ui::MainWindow::singleLogReady, &window,
                         [results](const QString &path) { results->insert("single_log", path); });
        QObject::connect(
            &window, &xm::ui::MainWindow::taskFinished, &window,
            [&, results, finished, directory, finish](const QString &kind, bool success) {
                if (*finished)
                    return;
                if (!success) {
                    finish(false, "Task failed: " + kind);
                    return;
                }
                QTimer::singleShot(0, &window, [&, results, finished, directory, finish, kind] {
                    if (*finished)
                        return;
                    if (kind == "load") {
                        if (window.selectedIds().isEmpty()) {
                            finish(false, "No default-selected candidates");
                            return;
                        }
                        results->insert("candidate_count",
                                        static_cast<qint64>(window.candidates().size()));
                        results->insert("selected_count", window.selectedIds().size());
                        const auto chosen = window.selectedIds();
                        window.setViewMode("tree");
                        window.setFilters(false, true, "ALL", "");
                        window.setChecked({chosen.front()}, false);
                        window.setViewMode("flat");
                        window.setViewMode("tree");
                        window.setFilters(false, false, "ALL", "");
                        window.applySelection(chosen);
                        window.setViewMode("flat");
                        window.mergeSelected();
                    } else if (kind == "merge")
                        window.exportExcel(QDir(directory).filePath("selected-tests.xlsx"));
                    else if (kind == "excel")
                        window.exportSingleXml(window.selectedIds().front(),
                                               QDir(directory).filePath("single-suite.xml"));
                    else if (kind == "single-xml")
                        window.openSingleLog(window.selectedIds().front());
                    else if (kind == "single-log")
                        finish(true, {});
                });
            });
        QTimer::singleShot(180000, &window, [finish] { finish(false, "Smoke test timed out"); });
        QTimer::singleShot(0, &window, [&window, input] { window.loadFiles({input}); });
        return app.exec();
    }
    QStringList inputs;
    for (int index = 1; index < args.size(); ++index)
        if (args[index].endsWith(".xml", Qt::CaseInsensitive))
            inputs.push_back(args[index]);
    if (!inputs.isEmpty())
        QTimer::singleShot(0, &window, [&window, inputs] { window.loadFiles(inputs); });
    return app.exec();
}
