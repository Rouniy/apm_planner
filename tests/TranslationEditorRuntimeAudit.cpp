#include "TranslationEditorRuntimeAudit.h"
#include "ui/MainWindow.h"
#include "ui/TranslationEditorWindow.h"
#include "ui/TranslationEditorModel.h"
#include "ui/configuration/ConfigDeveloperToolsView.h"
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPixmap>
#include <QPushButton>
#include <QTableView>
#include <QTemporaryDir>
#include <QThread>
#include <QDebug>
#include <functional>

namespace {
template<class T> T *control(QObject *owner, const char *name) {
    return owner ? owner->findChild<T *>(QString::fromLatin1(name)) : nullptr;
}
template<class T> T *visible(QObject *owner, const char *name) {
    if (owner) for (auto *item : owner->findChildren<T *>(QString::fromLatin1(name)))
        if (item->isVisible()) return item;
    return nullptr;
}
bool waitFor(const std::function<bool()> &ready, int timeout = 5000) {
    QElapsedTimer timer; timer.start();
    while (!ready() && timer.elapsed() < timeout) {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return ready();
}
QByteArray read(const QString &path) {
    QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
bool write(const QString &path, const QByteArray &bytes) {
    QFile file(path); return file.open(QIODevice::WriteOnly | QIODevice::NewOnly)
        && file.write(bytes) == bytes.size();
}
}

int RunTranslationEditorRuntimeAudit()
{
    int failures = 0;
    const auto expect = [&](bool value, const char *reason) {
        if (!value) { ++failures; qCritical() << "Translation editor runtime:" << reason; }
    };
    QTemporaryDir directory;
    if (!directory.isValid()) return 1;
    const QString source = directory.filePath(QStringLiteral("source tree"));
    const QString output = QDir(source).filePath(QStringLiteral("translation"));
    expect(QDir().mkpath(output), "fixture directories could not be created");
    const QString neutral = QDir(source).filePath(QStringLiteral("View.resx"));
    const QString localized = QDir(output).filePath(QStringLiteral("View.ru-RU.resx"));
    const QString html = QDir(output).filePath(QStringLiteral("output.html"));
    const QByteArray original = "<root><data name=\"one.Text\"><value>One</value></data>"
        "<data name=\"two.Text\"><value>Two</value></data></root>";
    const QByteArray previous = "<root><data name=\"old.Text\"><value>old localized fixture</value></data></root>";
    expect(write(neutral, original) && write(localized, previous) && write(html, "old resume fixture"),
           "fixture files could not be created");
    auto *main = MainWindow::instance();
    auto *route = control<QAction>(main, "actionDeveloperTools");
    auto *shared = control<QAction>(main, "actionTranslationResxEditor");
    expect(route && shared, "production actions missing");
    if (!route || !shared) return 1;
    route->trigger(); QApplication::processEvents();
    auto *page = main->findChild<ConfigDeveloperToolsView *>();
    auto *button = control<QPushButton>(page, "TranslationResxEditorButton");
    expect(page && page->ActionCount() == 32 && page->ImplementedActionCount() == 32
               && button && button->isEnabled(), "Developer action unavailable offline");
    if (!button) return 1;
    button->click(); QApplication::processEvents();
    QPointer<TranslationEditorWindow> editor(main->findChild<TranslationEditorWindow *>());
    expect(editor && editor->isVisible() && editor->isWindow()
               && editor->windowModality() == Qt::NonModal, "action did not open a modeless editor");
    if (!editor) return 1;
    shared->trigger();
    expect(main->findChildren<TranslationEditorWindow *>().size() == 1, "shared route duplicated editor");
    auto *browse = control<QPushButton>(editor, "BrowseSourceButton");
    auto *culture = control<QComboBox>(editor, "CultureCombo");
    auto *load = control<QPushButton>(editor, "LoadButton");
    auto *grid = control<QTableView>(editor, "TranslationGrid");
    auto *exportButton = control<QPushButton>(editor, "ExportButton");
    auto *search = control<QLineEdit>(editor, "SearchEdit");
    auto *copy = control<QPushButton>(editor, "CopyCsvButton");
    expect(browse && culture && load && grid && exportButton && search && copy, "editor controls missing");
    if (!browse || !culture || !load || !grid || !exportButton || !search || !copy) return 1;
    browse->click();
    auto *picker = visible<QFileDialog>(editor, "TranslationEditorSourceDialog");
    expect(picker, "source picker missing");
    if (!picker) return 1;
    picker->reject(); QApplication::processEvents();
    expect(grid->model()->rowCount() == 0, "source Cancel populated grid");
    browse->click();
    picker = visible<QFileDialog>(editor, "TranslationEditorSourceDialog");
    if (!picker) return 1;
    picker->setDirectory(directory.path());
    picker->selectFile(source);
    expect(QMetaObject::invokeMethod(picker, "accept", Qt::DirectConnection), "source selection failed");
    const int russian = culture->findData(QStringLiteral("ru-RU"));
    expect(russian >= 0 && culture->count() == ResxTranslationService::cultures().size()
               && culture->count() >= 851 && culture->findData(QStringLiteral("zh-TW")) >= 0,
           "exact .NET culture catalogue or supported Chinese aliases missing");
    culture->setCurrentIndex(russian);
    load->click();
    expect(waitFor([&] { return !editor->busy() && grid->model()->rowCount() == 2; }), "resource load did not complete");
    expect(grid->model()->columnCount() == 5, "reference grid columns differ");
    const QString translated = QString::fromUtf8("Один & <текст>");
    expect(grid->model()->setData(grid->model()->index(0, 3), translated), "translation is not editable");
    search->setText(QStringLiteral("one"));
    expect(waitFor([&] { return grid->model()->rowCount() == 1; }), "search does not filter grid");
    copy->click();
    expect(waitFor([&] { return !editor->busy() && QApplication::clipboard()->text().contains(QStringLiteral("two.Text")); }),
           "Copy CSV omitted filtered-out entries");
    culture->setCurrentIndex(culture->findData(QStringLiteral("de-DE")));
    exportButton->click();
    auto *confirmation = visible<QDialog>(editor, "TranslationEditorExportConfirmation");
    expect(confirmation, "export confirmation missing");
    if (!confirmation) return 1;
    confirmation->reject(); QApplication::processEvents();
    expect(read(localized) == previous && read(html) == "old resume fixture", "export Cancel changed files");
    exportButton->click();
    confirmation = visible<QDialog>(editor, "TranslationEditorExportConfirmation");
    auto *yes = control<QPushButton>(confirmation, "TranslationEditorExportConfirmButton");
    expect(confirmation && yes, "export confirmation unavailable on retry");
    if (!confirmation || !yes) return 1;
    const QString screenshot = qEnvironmentVariable("APM_RESX_AUDIT_SCREENSHOT");
    if (!screenshot.isEmpty()) expect(confirmation->grab().save(screenshot), "export consent screenshot failed");
    yes->click();
    expect(waitFor([&] { return !editor->busy() && read(localized) != previous; }), "RESX export did not complete");
    expect(read(localized).contains(translated.toHtmlEscaped().toUtf8())
               && !read(localized).contains("two.Text"), "localized output is not sparse or escaped correctly");
    expect(!QFile::exists(QDir(output).filePath(QStringLiteral("View.de-DE.resx"))), "selected combo retargeted loaded culture export");
    expect(read(html).contains("two.Text") && read(neutral) == original, "resume omitted a row or source changed");
    const QDir backupRoot(QDir(output).filePath(QStringLiteral(".backup")));
    const auto backups = backupRoot.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    expect(backups.size() == 1, "one complete backup generation was not created");
    if (backups.size() == 1) {
        const QDir backup(backupRoot.filePath(backups.first()));
        expect(read(backup.filePath(QStringLiteral("View.ru-RU.resx"))) == previous
                   && read(backup.filePath(QStringLiteral("output.html"))) == "old resume fixture", "backup bytes differ");
    }
    search->clear();
    if (!screenshot.isEmpty()) expect(editor->grab().save(screenshot + QStringLiteral(".window.png")), "editor screenshot failed");
    expect(grid->model()->setData(grid->model()->index(0, 3), QStringLiteral("unsaved fixture edit")), "post-export editing failed");
    main->close();
    auto *closePrompt = visible<QMessageBox>(editor, "TranslationEditorCloseConfirmation");
    expect(closePrompt && main->isVisible(), "application close bypassed unsaved editor");
    if (closePrompt) closePrompt->reject();
    QApplication::processEvents();
    expect(editor && editor->isVisible() && main->isVisible(), "Cancel closed editor or application");
    editor->close();
    closePrompt = visible<QMessageBox>(editor, "TranslationEditorCloseConfirmation");
    expect(closePrompt, "editor close did not prompt for unsaved changes");
    if (closePrompt) closePrompt->done(QMessageBox::Yes);
    expect(waitFor([&] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); return !editor; }), "approved editor close did not release window");
    expect(main->isVisible(), "later editor close reused a cancelled application-close request");
    shared->trigger(); QApplication::processEvents();
    editor = main->findChild<TranslationEditorWindow *>();
    expect(editor && editor->isVisible(), "editor could not reopen");
    if (editor) editor->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    main->close();
    qInfo() << "Translation editor runtime audit failures:" << failures;
    return failures ? 1 : 0;
}
