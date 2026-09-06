#include "ui/TranslationEditorWindow.h"

#include "ui/TranslationEditorModel.h"

#include <QtTest>

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableView>
#include <QTemporaryDir>

namespace
{
template<typename T>
T *control(QWidget *window, const char *name)
{
    T *result = window->findChild<T *>(QString::fromLatin1(name));
    Q_ASSERT(result);
    return result;
}

template<typename T>
T *visibleDialog(QWidget *window, const char *name)
{
    const auto dialogs = window->findChildren<T *>(
        QString::fromLatin1(name), Qt::FindChildrenRecursively);
    for (T *dialog : dialogs) {
        if (dialog->isVisible())
            return dialog;
    }
    return nullptr;
}

bool writeBytes(const QString &path, const QByteArray &bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(bytes) == bytes.size();
}

QByteArray resx(const QList<QPair<QString, QString>> &entries)
{
    QByteArray result("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<root>\n");
    for (const auto &entry : entries) {
        result += "  <data name=\"" + entry.first.toUtf8()
            + "\" xml:space=\"preserve\"><value>"
            + entry.second.toUtf8() + "</value></data>\n";
    }
    result += "</root>\n";
    return result;
}

void selectDirectory(QFileDialog *dialog, const QString &path)
{
    QVERIFY(dialog);
    dialog->setDirectory(path);
    if (QLineEdit *name = dialog->findChild<QLineEdit *>(QStringLiteral("fileNameEdit")))
        name->setText(path);
    dialog->selectFile(path);
    QVERIFY(QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection));
}

void selectFile(QFileDialog *dialog, const QString &path)
{
    QVERIFY(dialog);
    dialog->setDirectory(QFileInfo(path).absolutePath());
    if (QLineEdit *name = dialog->findChild<QLineEdit *>(QStringLiteral("fileNameEdit")))
        name->setText(path);
    dialog->selectFile(path);
    QVERIFY(QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection));
}

void chooseCulture(TranslationEditorWindow *window, const QString &culture)
{
    auto *combo = control<QComboBox>(window, "CultureCombo");
    const int index = combo->findData(culture);
    QVERIFY2(index >= 0, qPrintable(QStringLiteral("Culture missing: ") + culture));
    combo->setCurrentIndex(index);
}

void chooseSource(TranslationEditorWindow *window, const QString &path)
{
    control<QPushButton>(window, "BrowseSourceButton")->click();
    auto *dialog = visibleDialog<QFileDialog>(window, "TranslationEditorSourceDialog");
    QVERIFY(dialog);
    selectDirectory(dialog, path);
    QTRY_COMPARE(control<QLineEdit>(window, "SourceRoot")->text(),
                 QFileInfo(path).absoluteFilePath());
}

void loadProject(TranslationEditorWindow *window, const QString &path,
                 const QString &culture = QStringLiteral("ru-RU"))
{
    chooseSource(window, path);
    chooseCulture(window, culture);
    control<QPushButton>(window, "LoadButton")->click();
    QTRY_VERIFY_WITH_TIMEOUT(!window->busy(), 10000);
}

QModelIndex translationIndex(TranslationEditorWindow *window, int row)
{
    return window->editorModel()->index(
        row, TranslationEditorModel::TranslationColumn);
}

QPushButton *messageButton(QMessageBox *box, QMessageBox::StandardButton which)
{
    return qobject_cast<QPushButton *>(box ? box->button(which) : nullptr);
}
} // namespace

class TranslationEditorWindowTest final : public QObject
{
    Q_OBJECT

private slots:
    void matchesMissionPlannerSurface();
    void loadsEditsFiltersCopiesAndReverts();
    void translationEditorPreservesLongMultilineText();
    void exportIsExactDefaultCancelAndBacksUp();
    void failedLoadRetainsGridAndImportResumes();
    void closeResolvesUnsavedAndBusyWorkSafely();
};

void TranslationEditorWindowTest::matchesMissionPlannerSurface()
{
    QPointer<TranslationEditorWindow> window = new TranslationEditorWindow;
    QCOMPARE(window->objectName(), QStringLiteral("TranslationEditorWindow"));
    QCOMPARE(window->windowModality(), Qt::NonModal);
    QCOMPARE(window->size(), QSize(1400, 820));
    QCOMPARE(window->minimumSize(), QSize(940, 600));
    QVERIFY(window->testAttribute(Qt::WA_DeleteOnClose));
    QVERIFY(!window->busy());
    QVERIFY(!window->isClosing());

    QVERIFY(control<QLineEdit>(window, "SourceRoot")->isReadOnly());
    QVERIFY(control<QLineEdit>(window, "OutputRoot")->isReadOnly());
    auto *table = control<QTableView>(window, "TranslationGrid");
    QCOMPARE(table->model()->columnCount(), 5);
    const QStringList headers = {QStringLiteral("File"), QStringLiteral("Internal key"),
        QStringLiteral("English / neutral"), QStringLiteral("Other language"),
        QStringLiteral("Existing")};
    for (int column = 0; column < headers.size(); ++column)
        QCOMPARE(table->model()->headerData(column, Qt::Horizontal).toString(), headers.at(column));
    QVERIFY(!control<QPushButton>(window, "ExportButton")->isEnabled());
    QVERIFY(!control<QPushButton>(window, "CancelButton")->isEnabled());
    QVERIFY(control<QComboBox>(window, "CultureCombo")->count() > 10);

    delete window;
    QVERIFY(window.isNull());
}

void TranslationEditorWindowTest::translationEditorPreservesLongMultilineText()
{
    QTemporaryDir source;
    QVERIFY(source.isValid());
    const QString original = QStringLiteral("first line\n")
        + QString(40000, QLatin1Char('x')) + QStringLiteral("\nlast line");
    QVERIFY(writeBytes(source.filePath(QStringLiteral("Strings.resx")),
        resx({{QStringLiteral("Long.Text"), original}})));

    TranslationEditorWindow window;
    window.show();
    loadProject(&window, source.path());
    auto *table = control<QTableView>(&window, "TranslationGrid");
    const QModelIndex cell = table->model()->index(
        0, TranslationEditorModel::TranslationColumn);
    table->setCurrentIndex(cell);
    table->edit(cell);
    QPlainTextEdit *editor = nullptr;
    QTRY_VERIFY((editor = visibleDialog<QPlainTextEdit>(
        &window, "TranslationCellEditor")) != nullptr);
    QCOMPARE(editor->toPlainText(), original);

    const QString replacement = QStringLiteral("replacement\n")
        + QString(41000, QLatin1Char('z')) + QStringLiteral("\ntail");
    editor->setPlainText(replacement);
    QKeyEvent commit(QEvent::KeyPress, Qt::Key_Return, Qt::ControlModifier);
    QApplication::sendEvent(editor, &commit);
    QTRY_COMPARE(window.editorModel()->entryAt(0).translation, replacement);
}

void TranslationEditorWindowTest::loadsEditsFiltersCopiesAndReverts()
{
    QTemporaryDir source;
    QVERIFY(source.isValid());
    QVERIFY(writeBytes(source.filePath(QStringLiteral("Strings.resx")),
        resx({{QStringLiteral("Window.Text"), QStringLiteral("Window")},
              {QStringLiteral("Button.Text"), QStringLiteral("Launch")}})));
    QVERIFY(writeBytes(source.filePath(QStringLiteral("Strings.ru-RU.resx")),
        resx({{QStringLiteral("Window.Text"), QString::fromUtf8("Окно")}})));

    TranslationEditorWindow window;
    window.show();
    loadProject(&window, source.path());
    auto *model = window.editorModel();
    QCOMPARE(model->totalCount(), 2);
    QCOMPARE(model->resourceFileCount(), 1);
    QCOMPARE(model->translatedCount(), 1);
    QCOMPARE(model->missingCount(), 1);
    QVERIFY(window.statusText().contains(QStringLiteral("Loaded 2")));

    int buttonRow = -1;
    for (int row = 0; row < model->rowCount(); ++row) {
        if (model->entryAt(row).key == QStringLiteral("Button.Text"))
            buttonRow = row;
    }
    QVERIFY(buttonRow >= 0);
    QVERIFY(model->setData(translationIndex(&window, buttonRow),
                           QString::fromUtf8("Запуск"), Qt::EditRole));
    QCOMPARE(model->modifiedCount(), 1);
    QCOMPARE(model->missingCount(), 0);

    auto *table = control<QTableView>(&window, "TranslationGrid");
    control<QLineEdit>(&window, "SearchEdit")->setText(QStringLiteral("Button"));
    QTRY_COMPARE(table->model()->rowCount(), 1);
    control<QCheckBox>(&window, "ExportOnlyCheckBox")->setChecked(true);
    QTRY_COMPARE(table->model()->rowCount(), 1);

    control<QPushButton>(&window, "CopyCsvButton")->click();
    QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 10000);
    const QString csv = QApplication::clipboard()->text();
    QVERIFY(csv.contains(QStringLiteral("\"Button.Text\"")));
    QVERIFY(csv.contains(QString::fromUtf8("\"Запуск\"")));
    QVERIFY(csv.contains(QStringLiteral("\"Window.Text\"")));

    control<QPushButton>(&window, "RevertButton")->click();
    auto *prompt = visibleDialog<QMessageBox>(&window, "TranslationEditorRevertConfirmation");
    QVERIFY(prompt);
    QCOMPARE(prompt->defaultButton(), messageButton(prompt, QMessageBox::Cancel));
    messageButton(prompt, QMessageBox::Cancel)->click();
    QTRY_VERIFY(!window.busy());
    QCOMPARE(model->modifiedCount(), 1);

    control<QPushButton>(&window, "RevertButton")->click();
    prompt = visibleDialog<QMessageBox>(&window, "TranslationEditorRevertConfirmation");
    QVERIFY(prompt);
    messageButton(prompt, QMessageBox::Yes)->click();
    QTRY_COMPARE(model->modifiedCount(), 0);
    QCOMPARE(model->entryAt(buttonRow).translation, QStringLiteral("Launch"));
}

void TranslationEditorWindowTest::exportIsExactDefaultCancelAndBacksUp()
{
    QTemporaryDir source;
    QVERIFY(source.isValid());
    QVERIFY(writeBytes(source.filePath(QStringLiteral("Views/Strings.resx")),
        resx({{QStringLiteral("A.Text"), QStringLiteral("Alpha")},
              {QStringLiteral("B.Text"), QStringLiteral("Beta")}})));

    TranslationEditorWindow window;
    window.show();
    loadProject(&window, source.path());
    TranslationEditorModel *model = window.editorModel();
    QVERIFY(model->setData(translationIndex(&window, 0), QString::fromUtf8("Альфа")));
    const QString output = source.filePath(QStringLiteral("translation"));
    QVERIFY(QDir().mkpath(source.filePath(QStringLiteral("translation/Views"))));
    const QString localized = source.filePath(QStringLiteral("translation/Views/Strings.ru-RU.resx"));
    QVERIFY(writeBytes(localized, QByteArrayLiteral("old localized")));
    QVERIFY(writeBytes(source.filePath(QStringLiteral("translation/output.html")),
                       QByteArrayLiteral("old resume")));

    control<QPushButton>(&window, "ExportButton")->click();
    auto *prompt = visibleDialog<QDialog>(&window, "TranslationEditorExportConfirmation");
    QVERIFY(prompt);
    auto *plan = control<QPlainTextEdit>(prompt, "TranslationEditorExportPlan");
    QVERIFY(plan->toPlainText().contains(localized));
    QVERIFY(plan->toPlainText().contains(source.filePath(QStringLiteral("translation/output.html"))));
    auto *buttons = prompt->findChild<QDialogButtonBox *>();
    QVERIFY(buttons);
    QVERIFY(buttons->button(QDialogButtonBox::Cancel)->isDefault());
    buttons->button(QDialogButtonBox::Cancel)->click();
    QTRY_VERIFY(!window.busy());
    QCOMPARE(QFile(localized).open(QIODevice::ReadOnly), true);
    QFile old(localized); QVERIFY(old.open(QIODevice::ReadOnly));
    QCOMPARE(old.readAll(), QByteArrayLiteral("old localized"));

    control<QPushButton>(&window, "ExportButton")->click();
    prompt = visibleDialog<QDialog>(&window, "TranslationEditorExportConfirmation");
    QVERIFY(prompt);
    control<QPushButton>(prompt, "TranslationEditorExportConfirmButton")->click();
    QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 15000);
    QVERIFY(window.statusText().contains(QStringLiteral("Exported")));
    QVERIFY(!model->hasUnsavedChanges());
    QFile translated(localized);
    QVERIFY(translated.open(QIODevice::ReadOnly));
    const QByteArray outputBytes = translated.readAll();
    QVERIFY(outputBytes.contains(QString::fromUtf8("Альфа").toUtf8()));
    QVERIFY(!outputBytes.contains(QByteArrayLiteral("Beta")));
    QVERIFY(QFileInfo::exists(source.filePath(QStringLiteral("translation/output.html"))));
    QDir backup(source.filePath(QStringLiteral("translation/.backup")));
    QVERIFY(backup.exists());
    QVERIFY(!backup.entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
}

void TranslationEditorWindowTest::failedLoadRetainsGridAndImportResumes()
{
    QTemporaryDir source;
    QTemporaryDir empty;
    QVERIFY(source.isValid() && empty.isValid());
    QVERIFY(writeBytes(source.filePath(QStringLiteral("Strings.resx")),
        resx({{QStringLiteral("A.Text"), QStringLiteral("Alpha")}})));
    TranslationEditorWindow window;
    window.show();
    loadProject(&window, source.path());
    QCOMPARE(window.editorModel()->rowCount(), 1);
    QVERIFY(window.editorModel()->setData(translationIndex(&window, 0),
                                          QStringLiteral("Imported later")));

    control<QPushButton>(&window, "ExportButton")->click();
    auto *exportPrompt = visibleDialog<QDialog>(&window, "TranslationEditorExportConfirmation");
    QVERIFY(exportPrompt);
    control<QPushButton>(exportPrompt, "TranslationEditorExportConfirmButton")->click();
    QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 10000);
    const QString resume = source.filePath(QStringLiteral("translation/output.html"));
    QVERIFY(QFileInfo::exists(resume));
    QVERIFY(window.editorModel()->setData(translationIndex(&window, 0),
                                          QStringLiteral("Changed again")));

    control<QPushButton>(&window, "ImportButton")->click();
    auto *import = visibleDialog<QFileDialog>(&window, "TranslationEditorImportDialog");
    QVERIFY(import);
    selectFile(import, resume);
    QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 10000);
    QCOMPARE(window.editorModel()->entryAt(0).translation, QStringLiteral("Imported later"));

    QVERIFY(window.editorModel()->setData(translationIndex(&window, 0),
                                          QStringLiteral("Retained after failure")));

    chooseSource(&window, empty.path());
    control<QPushButton>(&window, "LoadButton")->click();
    auto *discard = visibleDialog<QMessageBox>(&window,
        "TranslationEditorDiscardLoadConfirmation");
    QVERIFY(discard);
    messageButton(discard, QMessageBox::Yes)->click();
    QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 10000);
    QCOMPARE(window.editorModel()->rowCount(), 1);
    QCOMPARE(window.editorModel()->entryAt(0).translation,
             QStringLiteral("Retained after failure"));
    QVERIFY(window.statusText().contains(QStringLiteral("previous table was retained")));
}

void TranslationEditorWindowTest::closeResolvesUnsavedAndBusyWorkSafely()
{
    QTemporaryDir source;
    QVERIFY(source.isValid());
    QList<QPair<QString, QString>> entries;
    for (int index = 0; index < 4000; ++index) {
        entries.append({QStringLiteral("K%1.Text").arg(index),
                        QStringLiteral("Value %1").arg(index)});
    }
    QVERIFY(writeBytes(source.filePath(QStringLiteral("Strings.resx")), resx(entries)));

    QPointer<TranslationEditorWindow> window = new TranslationEditorWindow;
    window->show();
    loadProject(window, source.path());
    QVERIFY(window->editorModel()->setData(translationIndex(window, 0),
                                           QStringLiteral("Edited")));
    QSignalSpy resolved(window, &TranslationEditorWindow::closeResolved);
    window->close();
    auto *closePrompt = visibleDialog<QMessageBox>(window,
        "TranslationEditorCloseConfirmation");
    QVERIFY(closePrompt);
    QCOMPARE(closePrompt->defaultButton(), messageButton(closePrompt, QMessageBox::Cancel));
    messageButton(closePrompt, QMessageBox::Cancel)->click();
    QTRY_VERIFY(!window->isClosing());
    QCOMPARE(resolved.size(), 1);
    QCOMPARE(resolved.at(0).at(0).toBool(), false);

    // A large CSV job gives close() a real worker boundary. The approved close
    // waits for cancellation instead of abandoning a destructive/export task.
    control<QPushButton>(window, "CopyCsvButton")->click();
    QVERIFY(window->busy());
    window->close();
    closePrompt = visibleDialog<QMessageBox>(window,
        "TranslationEditorCloseConfirmation");
    QVERIFY(closePrompt);
    messageButton(closePrompt, QMessageBox::Yes)->click();
    QTRY_VERIFY_WITH_TIMEOUT(window.isNull(), 15000);
    QVERIFY(resolved.size() >= 2);
    QCOMPARE(resolved.last().at(0).toBool(), true);
}

int main(int argc, char **argv)
{
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
    QApplication app(argc, argv);
    TranslationEditorWindowTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_translationeditorwindow.moc"
