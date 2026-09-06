#include "TranslationEditorWindow.h"

#include "TranslationEditorModel.h"

#include <QtConcurrentRun>

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QKeyEvent>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSet>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>

struct TranslationEditorWindow::JobState
{
    enum class Kind { Load, Export, Import, Csv };
    explicit JobState(Kind value) : kind(value) {}
    Kind kind;
    std::atomic_bool cancelled{false};
    std::atomic<qint64> completed{0};
    std::atomic<qint64> total{0};
};

namespace
{
QString pathKey(QString path)
{
    path = QDir::cleanPath(path);
#ifdef Q_OS_WIN
    return path.toCaseFolded();
#else
    return path;
#endif
}

QString boundedList(const QStringList &values, int maximum = 6)
{
    if (values.isEmpty())
        return {};
    const int shown = qMin(maximum, values.size());
    QString result = values.mid(0, shown).join(QStringLiteral(", "));
    if (shown < values.size())
        result += TranslationEditorWindow::tr(" (+%1 more)").arg(values.size() - shown);
    return result;
}

class TranslationCellDelegate final : public QStyledItemDelegate
{
public:
    explicit TranslationCellDelegate(QObject *parent)
        : QStyledItemDelegate(parent)
    {
    }

    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &,
                          const QModelIndex &) const override
    {
        auto *editor = new QPlainTextEdit(parent);
        editor->setObjectName(QStringLiteral("TranslationCellEditor"));
        editor->setToolTip(TranslationEditorWindow::tr("Enter inserts a new line; Ctrl+Enter or Tab commits the translation."));
        editor->setTabChangesFocus(false);
        return editor;
    }

    void setEditorData(QWidget *editor, const QModelIndex &index) const override
    {
        if (auto *text = qobject_cast<QPlainTextEdit *>(editor))
            text->setPlainText(index.data(Qt::EditRole).toString());
    }

    void setModelData(QWidget *editor, QAbstractItemModel *model,
                      const QModelIndex &index) const override
    {
        if (auto *text = qobject_cast<QPlainTextEdit *>(editor))
            model->setData(index, text->toPlainText(), Qt::EditRole);
    }

    void updateEditorGeometry(QWidget *editor,
                              const QStyleOptionViewItem &option,
                              const QModelIndex &) const override
    {
        QRect geometry = option.rect;
        geometry.setHeight(qMax(geometry.height(), 110));
        editor->setGeometry(geometry);
    }

    bool eventFilter(QObject *object, QEvent *event) override
    {
        auto *editor = qobject_cast<QPlainTextEdit *>(object);
        auto *key = event->type() == QEvent::KeyPress
            ? static_cast<QKeyEvent *>(event) : nullptr;
        if (!editor || !key)
            return QStyledItemDelegate::eventFilter(object, event);
        if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)
            && key->modifiers().testFlag(Qt::ControlModifier)) {
            emit commitData(editor);
            emit closeEditor(editor, QAbstractItemDelegate::NoHint);
            return true;
        }
        if (key->key() == Qt::Key_Tab
            && key->modifiers() == Qt::NoModifier) {
            emit commitData(editor);
            emit closeEditor(editor, QAbstractItemDelegate::EditNextItem);
            return true;
        }
        if (key->key() == Qt::Key_Backtab) {
            emit commitData(editor);
            emit closeEditor(editor, QAbstractItemDelegate::EditPreviousItem);
            return true;
        }
        return QStyledItemDelegate::eventFilter(object, event);
    }
};

} // namespace

TranslationEditorWindow::TranslationEditorWindow(QWidget *parent)
    : QWidget(parent, Qt::Window)
{
    setObjectName(QStringLiteral("TranslationEditorWindow"));
    setWindowTitle(tr("Translation / RESX Editor"));
    setWindowModality(Qt::NonModal);
    setAttribute(Qt::WA_DeleteOnClose, true);
    resize(1400, 820);
    setMinimumSize(940, 600);
    buildUi();
    populateCultures();

    m_progressTimer = new QTimer(this);
    m_progressTimer->setInterval(75);
    connect(m_progressTimer, &QTimer::timeout,
            this, &TranslationEditorWindow::updateProgress);

    connect(m_browseSource, &QPushButton::clicked,
            this, &TranslationEditorWindow::browseSource);
    connect(m_chooseOutput, &QPushButton::clicked,
            this, &TranslationEditorWindow::chooseOutput);
    connect(m_load, &QPushButton::clicked,
            this, &TranslationEditorWindow::requestLoad);
    connect(m_import, &QPushButton::clicked,
            this, &TranslationEditorWindow::chooseImport);
    connect(m_copyCsv, &QPushButton::clicked,
            this, &TranslationEditorWindow::copyCsv);
    connect(m_export, &QPushButton::clicked,
            this, &TranslationEditorWindow::requestExport);
    connect(m_revert, &QPushButton::clicked,
            this, &TranslationEditorWindow::requestRevert);
    connect(m_cancel, &QPushButton::clicked,
            this, &TranslationEditorWindow::cancel);
    connect(m_close, &QPushButton::clicked, this, &QWidget::close);

    connect(m_search, &QLineEdit::textChanged,
            m_filter, &TranslationEditorFilterModel::setSearchText);
    connect(m_missingOnly, &QCheckBox::toggled,
            m_filter, &TranslationEditorFilterModel::setMissingOnly);
    connect(m_exportOnly, &QCheckBox::toggled,
            m_filter, &TranslationEditorFilterModel::setExportOnly);
    connect(m_filter, &TranslationEditorFilterModel::visibleCountChanged,
            this, &TranslationEditorWindow::updateCounts);
    connect(m_model, &TranslationEditorModel::countsChanged, this, [this]() {
        ++m_modelRevision;
        updateCounts();
        refreshControls();
    });
    connect(m_culture, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) {
        if (m_model->hasProject() && selectedCulture() != m_loadedCulture) {
            setStatus(tr("The table still contains %1. Choose Load to replace it with %2.")
                .arg(m_loadedCulture, selectedCulture()));
        }
    });

    updateCounts();
    refreshControls();
}

TranslationEditorWindow::~TranslationEditorWindow()
{
    shutdown();
}

bool TranslationEditorWindow::busy() const noexcept
{
    return bool(m_job) || !m_sourceDialog.isNull() || !m_outputDialog.isNull()
        || !m_importDialog.isNull() || !m_prompt.isNull()
        || !m_closePrompt.isNull();
}

bool TranslationEditorWindow::isClosing() const noexcept
{
    return m_closing || m_shutdown;
}

QString TranslationEditorWindow::statusText() const
{
    return m_status ? m_status->text() : QString();
}

void TranslationEditorWindow::buildUi()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(8);

    auto *title = new QLabel(tr("Mission Planner RESX Translation Editor"), this);
    QFont titleFont = title->font();
    titleFont.setBold(true);
    titleFont.setPointSize(titleFont.pointSize() + 3);
    title->setFont(titleFont);
    layout->addWidget(title);

    auto *notice = new QLabel(
        tr("Edits Mission Planner-compatible .resx resources. Qt application "
           "translations (.ts/.qm) are separate; this tool does not change the "
           "running application language and performs no network access."), this);
    notice->setTextFormat(Qt::PlainText);
    notice->setWordWrap(true);
    layout->addWidget(notice);

    auto *sourceRow = new QHBoxLayout;
    sourceRow->addWidget(new QLabel(tr("Source project"), this));
    m_sourceRoot = new QLineEdit(this);
    m_sourceRoot->setObjectName(QStringLiteral("SourceRoot"));
    m_sourceRoot->setReadOnly(true);
    sourceRow->addWidget(m_sourceRoot, 1);
    m_browseSource = new QPushButton(tr("Browse…"), this);
    m_browseSource->setObjectName(QStringLiteral("BrowseSourceButton"));
    sourceRow->addWidget(m_browseSource);
    sourceRow->addWidget(new QLabel(tr("Culture"), this));
    m_culture = new QComboBox(this);
    m_culture->setObjectName(QStringLiteral("CultureCombo"));
    m_culture->setMinimumWidth(220);
    sourceRow->addWidget(m_culture);
    m_load = new QPushButton(tr("Load"), this);
    m_load->setObjectName(QStringLiteral("LoadButton"));
    sourceRow->addWidget(m_load);
    layout->addLayout(sourceRow);

    auto *filterRow = new QHBoxLayout;
    filterRow->addWidget(new QLabel(tr("Search"), this));
    m_search = new QLineEdit(this);
    m_search->setObjectName(QStringLiteral("SearchEdit"));
    m_search->setClearButtonEnabled(true);
    m_search->setPlaceholderText(tr("File, key, source or translation"));
    filterRow->addWidget(m_search, 1);
    m_missingOnly = new QCheckBox(tr("Missing only"), this);
    m_missingOnly->setObjectName(QStringLiteral("MissingOnlyCheckBox"));
    filterRow->addWidget(m_missingOnly);
    m_exportOnly = new QCheckBox(tr("Will export only"), this);
    m_exportOnly->setObjectName(QStringLiteral("ExportOnlyCheckBox"));
    filterRow->addWidget(m_exportOnly);
    m_revert = new QPushButton(tr("Revert edits…"), this);
    m_revert->setObjectName(QStringLiteral("RevertButton"));
    filterRow->addWidget(m_revert);
    layout->addLayout(filterRow);

    auto *counts = new QHBoxLayout;
    m_resourceCount = new QLabel(this);
    m_totalCount = new QLabel(this);
    m_missingCount = new QLabel(this);
    m_translatedCount = new QLabel(this);
    m_visibleCount = new QLabel(this);
    for (QLabel *label : {m_resourceCount, m_totalCount, m_missingCount,
                          m_translatedCount, m_visibleCount}) {
        label->setTextFormat(Qt::PlainText);
        counts->addWidget(label);
    }
    counts->addStretch(1);
    layout->addLayout(counts);

    m_model = new TranslationEditorModel(this);
    m_filter = new TranslationEditorFilterModel(this);
    m_filter->setSourceModel(m_model);
    m_table = new QTableView(this);
    m_table->setObjectName(QStringLiteral("TranslationGrid"));
    m_table->setModel(m_filter);
    m_table->setItemDelegateForColumn(
        TranslationEditorModel::TranslationColumn,
        new TranslationCellDelegate(m_table));
    m_table->setAlternatingRowColors(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setSortingEnabled(true);
    m_table->sortByColumn(TranslationEditorModel::FileColumn, Qt::AscendingOrder);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setSectionResizeMode(
        TranslationEditorModel::FileColumn, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(
        TranslationEditorModel::InternalKeyColumn, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(
        TranslationEditorModel::SourceTextColumn, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(
        TranslationEditorModel::TranslationColumn, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(
        TranslationEditorModel::ExistingColumn, QHeaderView::ResizeToContents);
    layout->addWidget(m_table, 1);

    auto *outputRow = new QHBoxLayout;
    outputRow->addWidget(new QLabel(tr("Output root"), this));
    m_outputRoot = new QLineEdit(this);
    m_outputRoot->setObjectName(QStringLiteral("OutputRoot"));
    m_outputRoot->setReadOnly(true);
    outputRow->addWidget(m_outputRoot, 1);
    m_chooseOutput = new QPushButton(tr("Choose…"), this);
    m_chooseOutput->setObjectName(QStringLiteral("ChooseOutputButton"));
    outputRow->addWidget(m_chooseOutput);
    layout->addLayout(outputRow);

    m_progress = new QProgressBar(this);
    m_progress->setObjectName(QStringLiteral("ProgressBar"));
    m_progress->setRange(0, 1000);
    m_progress->setValue(0);
    layout->addWidget(m_progress);

    m_status = new QLabel(
        tr("Choose a Mission Planner source directory and culture, then Load."), this);
    m_status->setObjectName(QStringLiteral("StatusLabel"));
    m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);

    auto *buttons = new QHBoxLayout;
    m_import = new QPushButton(tr("Import resume HTML…"), this);
    m_import->setObjectName(QStringLiteral("ImportButton"));
    buttons->addWidget(m_import);
    m_copyCsv = new QPushButton(tr("Copy CSV"), this);
    m_copyCsv->setObjectName(QStringLiteral("CopyCsvButton"));
    buttons->addWidget(m_copyCsv);
    buttons->addStretch(1);
    m_cancel = new QPushButton(tr("Cancel"), this);
    m_cancel->setObjectName(QStringLiteral("CancelButton"));
    buttons->addWidget(m_cancel);
    m_export = new QPushButton(tr("Export…"), this);
    m_export->setObjectName(QStringLiteral("ExportButton"));
    buttons->addWidget(m_export);
    m_close = new QPushButton(tr("Close"), this);
    m_close->setObjectName(QStringLiteral("CloseButton"));
    buttons->addWidget(m_close);
    layout->addLayout(buttons);
}

void TranslationEditorWindow::populateCultures()
{
    QString error;
    m_cultures = ResxTranslationService::cultures(&error);
    for (const TranslationCulture &culture : m_cultures)
        m_culture->addItem(culture.label(), culture.name);
    int selected = m_culture->findData(QLocale().name().replace('_', '-'));
    if (selected < 0)
        selected = m_culture->findData(QStringLiteral("ru-RU"));
    if (selected < 0 && !m_cultures.isEmpty())
        selected = 0;
    if (selected >= 0)
        m_culture->setCurrentIndex(selected);
    if (!error.isEmpty())
        setStatus(tr("Culture catalogue unavailable: %1").arg(error));
}

QString TranslationEditorWindow::selectedCulture() const
{
    return m_culture->currentData().toString();
}

void TranslationEditorWindow::browseSource()
{
    if (busy() || m_closing || m_shutdown)
        return;
    const quint64 flow = ++m_flow;
    auto *dialog = new QFileDialog(this, tr("Choose Mission Planner source directory"));
    dialog->setObjectName(QStringLiteral("TranslationEditorSourceDialog"));
    dialog->setFileMode(QFileDialog::Directory);
    dialog->setOption(QFileDialog::ShowDirsOnly, true);
    dialog->setAcceptMode(QFileDialog::AcceptOpen);
    dialog->setAttribute(Qt::WA_DeleteOnClose, true);
    if (!m_sourceRoot->text().isEmpty())
        dialog->setDirectory(m_sourceRoot->text());
    m_sourceDialog = dialog;
    connect(dialog, &QFileDialog::finished, this, [this, dialog, flow](int result) {
        if (m_sourceDialog != dialog || flow != m_flow)
            return;
        const QStringList files = dialog->selectedFiles();
        m_sourceDialog = nullptr;
        if (result == QDialog::Accepted && !files.isEmpty()) {
            const QString root = QDir::cleanPath(QFileInfo(files.first()).absoluteFilePath());
            QPointer<TranslationEditorWindow> guard(this);
            m_sourceRoot->setText(root);
            if (!guard || flow != m_flow)
                return;
            m_outputRoot->setText(QDir(root).absoluteFilePath(QStringLiteral("translation")));
            if (!guard || flow != m_flow)
                return;
            setStatus(tr("Source selected. Choose the target culture and Load."));
        }
        refreshControls();
    });
    QPointer<TranslationEditorWindow> guard(this);
    dialog->open();
    if (!guard)
        return;
    refreshControls();
}

void TranslationEditorWindow::chooseOutput()
{
    if (busy() || m_closing || m_shutdown)
        return;
    const quint64 flow = ++m_flow;
    auto *dialog = new QFileDialog(this, tr("Choose translation output directory"));
    dialog->setObjectName(QStringLiteral("TranslationEditorOutputDialog"));
    dialog->setFileMode(QFileDialog::Directory);
    dialog->setOption(QFileDialog::ShowDirsOnly, true);
    dialog->setAcceptMode(QFileDialog::AcceptOpen);
    dialog->setAttribute(Qt::WA_DeleteOnClose, true);
    if (!m_outputRoot->text().isEmpty())
        dialog->setDirectory(m_outputRoot->text());
    m_outputDialog = dialog;
    connect(dialog, &QFileDialog::finished, this, [this, dialog, flow](int result) {
        if (m_outputDialog != dialog || flow != m_flow)
            return;
        const QStringList files = dialog->selectedFiles();
        m_outputDialog = nullptr;
        if (result == QDialog::Accepted && !files.isEmpty()) {
            QPointer<TranslationEditorWindow> guard(this);
            m_outputRoot->setText(QDir::cleanPath(QFileInfo(files.first()).absoluteFilePath()));
            if (!guard || flow != m_flow)
                return;
        }
        refreshControls();
    });
    QPointer<TranslationEditorWindow> guard(this);
    dialog->open();
    if (!guard)
        return;
    refreshControls();
}

void TranslationEditorWindow::chooseImport()
{
    if (busy() || !m_model->hasProject() || m_closing || m_shutdown)
        return;
    const quint64 flow = ++m_flow;
    auto *dialog = new QFileDialog(this, tr("Import translation resume HTML"));
    dialog->setObjectName(QStringLiteral("TranslationEditorImportDialog"));
    dialog->setFileMode(QFileDialog::ExistingFile);
    dialog->setNameFilter(tr("HTML files (*.html *.htm);;All files (*)"));
    dialog->setAcceptMode(QFileDialog::AcceptOpen);
    dialog->setAttribute(Qt::WA_DeleteOnClose, true);
    m_importDialog = dialog;
    connect(dialog, &QFileDialog::finished, this, [this, dialog, flow](int result) {
        if (m_importDialog != dialog || flow != m_flow)
            return;
        const QStringList files = dialog->selectedFiles();
        m_importDialog = nullptr;
        if (result == QDialog::Accepted && !files.isEmpty())
            startImport(files.first(), flow);
        else
            refreshControls();
    });
    QPointer<TranslationEditorWindow> guard(this);
    dialog->open();
    if (!guard)
        return;
    refreshControls();
}

void TranslationEditorWindow::requestLoad()
{
    if (busy() || m_closing || m_shutdown)
        return;
    const QString root = m_sourceRoot->text().trimmed();
    const QString culture = selectedCulture();
    if (root.isEmpty() || culture.isEmpty()) {
        setStatus(tr("Choose a source directory and culture first."));
        return;
    }
    const quint64 flow = ++m_flow;
    if (!m_model->hasUnsavedChanges()) {
        startLoad(root, culture, flow);
        return;
    }
    auto *box = new QMessageBox(QMessageBox::Warning,
        tr("Discard unsaved translations?"),
        tr("Loading a new source or culture replaces the current table and discards all unsaved edits."),
        QMessageBox::Yes | QMessageBox::Cancel, this);
    box->setObjectName(QStringLiteral("TranslationEditorDiscardLoadConfirmation"));
    box->setDefaultButton(QMessageBox::Cancel);
    box->setEscapeButton(QMessageBox::Cancel);
    box->setAttribute(Qt::WA_DeleteOnClose, true);
    if (QPushButton *yes = qobject_cast<QPushButton *>(box->button(QMessageBox::Yes))) {
        yes->setText(tr("Discard and load"));
        yes->setAutoDefault(false);
    }
    m_prompt = box;
    connect(box, &QMessageBox::finished, this, [this, box, root, culture, flow](int result) {
        if (m_prompt != box || flow != m_flow)
            return;
        m_prompt = nullptr;
        if (result == QMessageBox::Yes)
            startLoad(root, culture, flow);
        else
            refreshControls();
    });
    QPointer<TranslationEditorWindow> guard(this);
    box->open();
    if (!guard)
        return;
    refreshControls();
}

void TranslationEditorWindow::startLoad(QString root, QString culture, quint64 flow)
{
    if (m_job || flow != m_flow || m_closing || m_shutdown)
        return;
    auto state = std::make_shared<JobState>(JobState::Kind::Load);
    auto *watcher = new QFutureWatcher<ResxTranslationService::LoadResult>(this);
    m_job = state;
    m_watcher = watcher;
    m_progressTimer->start();
    setStatus(tr("Loading source RESX resources…"));
    refreshControls();
    QPointer<TranslationEditorWindow> guard(this);
    connect(watcher, &QFutureWatcherBase::finished, this,
            [guard, watcher, state, flow]() {
        const auto result = watcher->result();
        watcher->deleteLater();
        if (guard)
            guard->finishLoad(flow, state, result);
    });
    watcher->setFuture(QtConcurrent::run([root, culture, state]() {
        return ResxTranslationService::load(root, culture,
            [state]() { return state->cancelled.load(std::memory_order_relaxed); },
            [state](qint64 done, qint64 total) {
                state->completed.store(done, std::memory_order_relaxed);
                state->total.store(total, std::memory_order_relaxed);
            });
    }));
}

void TranslationEditorWindow::finishLoad(
    quint64 flow, const std::shared_ptr<JobState> &state,
    const ResxTranslationService::LoadResult &result)
{
    if (m_job != state || flow != m_flow)
        return;
    if (result.cancelled || state->cancelled.load(std::memory_order_relaxed)) {
        setStatus(tr("Load cancelled; the previous table was retained."));
        completeJob(state);
        return;
    }
    if (!result.success) {
        setStatus(tr("Load failed; the previous table was retained: %1").arg(result.error));
        completeJob(state);
        return;
    }
    QPointer<TranslationEditorWindow> guard(this);
    m_model->loadProject(result.project);
    if (!guard || m_job != state || flow != m_flow)
        return;
    m_loadedCulture = result.project.culture;
    guard = this;
    m_sourceRoot->setText(result.project.sourceRoot);
    if (!guard || m_job != state || flow != m_flow)
        return;
    if (m_outputRoot->text().isEmpty())
        m_outputRoot->setText(QDir(result.project.sourceRoot).absoluteFilePath(QStringLiteral("translation")));
    if (!guard || m_job != state || flow != m_flow)
        return;
    QString summary = tr("Loaded %1 entries from %2 resource file(s) for %3.")
        .arg(result.project.entries.size()).arg(result.project.resourceFiles)
        .arg(result.project.culture);
    if (!result.project.warnings.isEmpty())
        summary += tr(" Warnings: %1").arg(boundedList(result.project.warnings));
    setStatus(summary);
    completeJob(state);
}

QStringList TranslationEditorWindow::exportPaths(
    const QVector<ResxTranslationEntry> &snapshot, const QString &outputRoot,
    const QString &culture, QString *error) const
{
    QStringList result;
    QSet<QString> seen;
    for (const ResxTranslationEntry &entry : snapshot) {
        QString pathError;
        const QString relative = ResxTranslationService::localizedRelativePath(
            entry.relativePath, culture, &pathError);
        if (relative.isEmpty()) {
            if (error)
                *error = pathError.isEmpty() ? tr("An output path could not be derived.") : pathError;
            return {};
        }
        const QString absolute = QDir(outputRoot).absoluteFilePath(relative);
        const QString key = pathKey(absolute);
        if (!seen.contains(key)) {
            seen.insert(key);
            result.append(QDir::cleanPath(absolute));
        }
    }
    const QString resume = QDir(outputRoot).absoluteFilePath(QStringLiteral("output.html"));
    if (!seen.contains(pathKey(resume)))
        result.append(QDir::cleanPath(resume));
    std::sort(result.begin(), result.end(), [](const QString &a, const QString &b) {
        return a.compare(b, Qt::CaseInsensitive) < 0;
    });
    if (error)
        error->clear();
    return result;
}

void TranslationEditorWindow::requestExport()
{
    if (busy() || m_closing || m_shutdown)
        return;
    if (!m_model->hasProject()) {
        setStatus(tr("Load a project before exporting."));
        return;
    }
    const QString output = m_outputRoot->text().trimmed();
    if (output.isEmpty()) {
        setStatus(tr("Choose an output directory before exporting."));
        return;
    }
    const QVector<ResxTranslationEntry> snapshot = m_model->snapshot();
    const QString culture = m_model->culture();
    QString error;
    const QStringList outputs = exportPaths(snapshot, output, culture, &error);
    if (!error.isEmpty()) {
        setStatus(tr("Cannot prepare export: %1").arg(error));
        return;
    }
    const quint64 flow = ++m_flow;
    showExportConfirmation(snapshot, output, culture, outputs,
                           m_modelRevision, flow);
}

void TranslationEditorWindow::showExportConfirmation(
    QVector<ResxTranslationEntry> snapshot, QString outputRoot,
    QString culture, QStringList outputs, quint64 modelRevision, quint64 flow)
{
    auto *dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("TranslationEditorExportConfirmation"));
    dialog->setWindowTitle(tr("Export RESX translations"));
    dialog->setModal(false);
    dialog->setAttribute(Qt::WA_DeleteOnClose, true);
    dialog->resize(760, 520);
    auto *layout = new QVBoxLayout(dialog);
    auto *summary = new QLabel(
        tr("Export ALL %1 loaded rows for culture %2 to:\n%3\n\n"
           "This writes sparse localized RESX files and output.html. Existing "
           "selected outputs are replaced only after exact backups are created. "
           "A resource with zero translated entries is still published empty so "
           "stale translations are removed. Localized files are regenerated only "
           "from the loaded string keys: non-string resources and keys outside "
           "this table are not carried forward (their old bytes remain in the "
           "backup). This does not change the running Qt "
           "application language (.ts/.qm is separate).")
            .arg(snapshot.size()).arg(culture, outputRoot), dialog);
    summary->setTextFormat(Qt::PlainText);
    summary->setWordWrap(true);
    layout->addWidget(summary);
    auto *plan = new QPlainTextEdit(dialog);
    plan->setObjectName(QStringLiteral("TranslationEditorExportPlan"));
    plan->setReadOnly(true);
    plan->setPlainText(outputs.join(QLatin1Char('\n')));
    layout->addWidget(plan, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Yes | QDialogButtonBox::Cancel,
                                         dialog);
    QPushButton *confirm = buttons->button(QDialogButtonBox::Yes);
    confirm->setObjectName(QStringLiteral("TranslationEditorExportConfirmButton"));
    confirm->setText(tr("Export and replace"));
    confirm->setAutoDefault(false);
    QPushButton *cancelButton = buttons->button(QDialogButtonBox::Cancel);
    cancelButton->setDefault(true);
    cancelButton->setAutoDefault(true);
    connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    layout->addWidget(buttons);
    m_prompt = dialog;
    connect(dialog, &QDialog::finished, this,
            [this, dialog, snapshot, outputRoot, culture,
             modelRevision, flow](int result) {
        if (m_prompt != dialog || flow != m_flow)
            return;
        m_prompt = nullptr;
        if (result != QDialog::Accepted) {
            setStatus(tr("Export cancelled before any files were changed."));
            refreshControls();
            return;
        }
        if (modelRevision != m_modelRevision || !m_model->hasProject()
            || m_model->culture() != culture) {
            setStatus(tr("Translations changed while confirmation was open; export was not started."));
            refreshControls();
            return;
        }
        startExport(snapshot, outputRoot, culture, modelRevision, flow);
    });
    QPointer<TranslationEditorWindow> guard(this);
    dialog->show();
    if (!guard)
        return;
    refreshControls();
}

void TranslationEditorWindow::startExport(
    QVector<ResxTranslationEntry> snapshot, QString outputRoot,
    QString culture, quint64 modelRevision, quint64 flow)
{
    if (m_job || flow != m_flow || m_closing || m_shutdown)
        return;
    auto state = std::make_shared<JobState>(JobState::Kind::Export);
    auto *watcher = new QFutureWatcher<ResxTranslationService::ExportResult>(this);
    m_job = state;
    m_watcher = watcher;
    m_progressTimer->start();
    setStatus(tr("Exporting all loaded translations…"));
    refreshControls();
    QPointer<TranslationEditorWindow> guard(this);
    connect(watcher, &QFutureWatcherBase::finished, this,
            [guard, watcher, state, flow, modelRevision]() {
        const auto result = watcher->result();
        watcher->deleteLater();
        if (guard)
            guard->finishExport(flow, state, modelRevision, result);
    });
    watcher->setFuture(QtConcurrent::run(
        [snapshot, outputRoot, culture, state]() {
        return ResxTranslationService::exportTranslations(
            outputRoot, culture, snapshot,
            [state]() { return state->cancelled.load(std::memory_order_relaxed); },
            [state](qint64 done, qint64 total) {
                state->completed.store(done, std::memory_order_relaxed);
                state->total.store(total, std::memory_order_relaxed);
            });
    }));
}

void TranslationEditorWindow::finishExport(
    quint64 flow, const std::shared_ptr<JobState> &state,
    quint64 modelRevision,
    const ResxTranslationService::ExportResult &result)
{
    if (m_job != state || flow != m_flow)
        return;
    QString receipts;
    if (!result.publishedPaths.isEmpty())
        receipts += tr(" Published %1 path(s): %2.")
            .arg(result.publishedPaths.size()).arg(boundedList(result.publishedPaths));
    if (!result.backupPaths.isEmpty())
        receipts += tr(" Backed up %1 path(s): %2.")
            .arg(result.backupPaths.size()).arg(boundedList(result.backupPaths));
    if (!result.backupDirectory.isEmpty())
        receipts += tr(" Backup directory: %1.").arg(result.backupDirectory);
    if (!result.warnings.isEmpty())
        receipts += tr(" Warnings: %1").arg(boundedList(result.warnings));

    if (result.success) {
        setStatus(tr("Exported %1 translated entries in %2 resource file(s).%3")
            .arg(result.translatedEntries).arg(result.resourceFiles).arg(receipts));
        QPointer<TranslationEditorWindow> guard(this);
        if (modelRevision == m_modelRevision)
            m_model->acceptChanges();
        if (!guard)
            return;
    } else if (result.cancelled) {
        setStatus(tr("Export cancelled. Completed publications and backups are retained.%1")
            .arg(receipts));
    } else {
        setStatus(tr("Export failed: %1.%2").arg(result.error, receipts));
    }
    completeJob(state);
}

void TranslationEditorWindow::startImport(QString path, quint64 flow)
{
    if (m_job || flow != m_flow || m_closing || m_shutdown)
        return;
    auto state = std::make_shared<JobState>(JobState::Kind::Import);
    auto *watcher = new QFutureWatcher<ResxTranslationService::ImportResult>(this);
    m_job = state;
    m_watcher = watcher;
    m_progressTimer->start();
    setStatus(tr("Importing resume HTML…"));
    refreshControls();
    QPointer<TranslationEditorWindow> guard(this);
    connect(watcher, &QFutureWatcherBase::finished, this,
            [guard, watcher, state, flow, path]() {
        const auto result = watcher->result();
        watcher->deleteLater();
        if (guard)
            guard->finishImport(flow, state, path, result);
    });
    watcher->setFuture(QtConcurrent::run([path, state]() {
        return ResxTranslationService::importResumeHtml(path,
            [state]() { return state->cancelled.load(std::memory_order_relaxed); },
            [state](qint64 done, qint64 total) {
                state->completed.store(done, std::memory_order_relaxed);
                state->total.store(total, std::memory_order_relaxed);
            });
    }));
}

void TranslationEditorWindow::finishImport(
    quint64 flow, const std::shared_ptr<JobState> &state, const QString &path,
    const ResxTranslationService::ImportResult &result)
{
    if (m_job != state || flow != m_flow)
        return;
    if (result.cancelled || state->cancelled.load(std::memory_order_relaxed)) {
        setStatus(tr("Import cancelled; existing edits were retained."));
    } else if (!result.success) {
        setStatus(tr("Import failed: %1").arg(result.error));
    } else {
        QPointer<TranslationEditorWindow> guard(this);
        const int applied = m_model->importTranslations(result);
        if (!guard)
            return;
        setStatus(tr("Imported %1 matching translation(s) from %2.")
            .arg(applied).arg(path));
    }
    completeJob(state);
}

void TranslationEditorWindow::copyCsv()
{
    if (busy() || !m_model->hasProject() || m_closing || m_shutdown)
        return;
    const QVector<ResxTranslationEntry> snapshot = m_model->snapshot();
    const int rows = snapshot.size();
    const quint64 flow = ++m_flow;
    auto state = std::make_shared<JobState>(JobState::Kind::Csv);
    auto *watcher = new QFutureWatcher<ResxTranslationService::TextResult>(this);
    m_job = state;
    m_watcher = watcher;
    m_progressTimer->start();
    setStatus(tr("Building CSV for all loaded rows…"));
    refreshControls();
    QPointer<TranslationEditorWindow> guard(this);
    connect(watcher, &QFutureWatcherBase::finished, this,
            [guard, watcher, state, flow, rows]() {
        const auto result = watcher->result();
        watcher->deleteLater();
        if (guard)
            guard->finishCsv(flow, state, rows, result);
    });
    watcher->setFuture(QtConcurrent::run([snapshot, state]() {
        return ResxTranslationService::buildCsv(snapshot,
            [state]() { return state->cancelled.load(std::memory_order_relaxed); },
            [state](qint64 done, qint64 total) {
                state->completed.store(done, std::memory_order_relaxed);
                state->total.store(total, std::memory_order_relaxed);
            });
    }));
}

void TranslationEditorWindow::finishCsv(
    quint64 flow, const std::shared_ptr<JobState> &state, int rows,
    const ResxTranslationService::TextResult &result)
{
    if (m_job != state || flow != m_flow)
        return;
    if (result.cancelled || state->cancelled.load(std::memory_order_relaxed)) {
        setStatus(tr("CSV creation cancelled; the clipboard was not changed."));
    } else if (!result.success) {
        setStatus(tr("CSV creation failed: %1").arg(result.error));
    } else {
        QPointer<TranslationEditorWindow> guard(this);
        if (QClipboard *clipboard = QApplication::clipboard())
            clipboard->setText(result.text);
        if (!guard || m_job != state || flow != m_flow)
            return;
        setStatus(tr("Copied CSV for all %1 loaded row(s) to the clipboard.").arg(rows));
    }
    completeJob(state);
}

void TranslationEditorWindow::requestRevert()
{
    if (busy() || !m_model->hasUnsavedChanges() || m_closing || m_shutdown)
        return;
    const quint64 flow = ++m_flow;
    auto *box = new QMessageBox(QMessageBox::Warning,
        tr("Revert unsaved translations?"),
        tr("This restores every edited translation to the last loaded or successfully exported value."),
        QMessageBox::Yes | QMessageBox::Cancel, this);
    box->setObjectName(QStringLiteral("TranslationEditorRevertConfirmation"));
    box->setDefaultButton(QMessageBox::Cancel);
    box->setEscapeButton(QMessageBox::Cancel);
    box->setAttribute(Qt::WA_DeleteOnClose, true);
    if (QPushButton *yes = qobject_cast<QPushButton *>(box->button(QMessageBox::Yes))) {
        yes->setText(tr("Revert all"));
        yes->setAutoDefault(false);
    }
    m_prompt = box;
    connect(box, &QMessageBox::finished, this, [this, box, flow](int result) {
        if (m_prompt != box || flow != m_flow)
            return;
        m_prompt = nullptr;
        if (result == QMessageBox::Yes) {
            QPointer<TranslationEditorWindow> guard(this);
            m_model->revertAll();
            if (!guard || flow != m_flow)
                return;
            setStatus(tr("Unsaved translation edits were reverted."));
        }
        refreshControls();
    });
    QPointer<TranslationEditorWindow> guard(this);
    box->open();
    if (!guard)
        return;
    refreshControls();
}

void TranslationEditorWindow::cancel()
{
    const std::shared_ptr<JobState> state = m_job;
    if (!state || state->cancelled.exchange(true, std::memory_order_relaxed))
        return;
    QString operation;
    switch (state->kind) {
    case JobState::Kind::Load: operation = tr("loading RESX resources"); break;
    case JobState::Kind::Export: operation = tr("exporting translations"); break;
    case JobState::Kind::Import: operation = tr("importing resume HTML"); break;
    case JobState::Kind::Csv: operation = tr("building CSV"); break;
    }
    setStatus(tr("Cancelling %1; waiting for the worker to stop safely…")
        .arg(operation));
    refreshControls();
}

void TranslationEditorWindow::updateCounts()
{
    m_resourceCount->setText(tr("Files: %1").arg(m_model->resourceFileCount()));
    m_totalCount->setText(tr("Rows: %1").arg(m_model->totalCount()));
    m_missingCount->setText(tr("Missing: %1").arg(m_model->missingCount()));
    m_translatedCount->setText(tr("Translated: %1").arg(m_model->translatedCount()));
    m_visibleCount->setText(tr("Visible: %1").arg(m_filter->visibleCount()));
}

void TranslationEditorWindow::updateProgress()
{
    const std::shared_ptr<JobState> state = m_job;
    if (!state || m_shutdown)
        return;
    const qint64 done = qMax<qint64>(0, state->completed.load(std::memory_order_relaxed));
    const qint64 total = qMax<qint64>(0, state->total.load(std::memory_order_relaxed));
    const bool progressSignals = m_progress->blockSignals(true);
    m_progress->setRange(0, total > 0 ? 1000 : 0);
    if (total > 0)
        m_progress->setValue(int(qMin<qint64>(1000, done * 1000 / total)));
    m_progress->blockSignals(progressSignals);
}

void TranslationEditorWindow::refreshControls()
{
    if (m_shutdown)
        return;
    const bool blocked = busy() || m_closing;
    const bool project = m_model->hasProject();
    m_browseSource->setEnabled(!blocked);
    m_culture->setEnabled(!blocked);
    m_load->setEnabled(!blocked && !m_sourceRoot->text().trimmed().isEmpty()
                       && !selectedCulture().isEmpty());
    m_search->setEnabled(!blocked && project);
    m_missingOnly->setEnabled(!blocked && project);
    m_exportOnly->setEnabled(!blocked && project);
    m_revert->setEnabled(!blocked && m_model->hasUnsavedChanges());
    m_table->setEditTriggers(!blocked && project
        ? QAbstractItemView::EditTriggers(QAbstractItemView::DoubleClicked
                                          | QAbstractItemView::EditKeyPressed)
        : QAbstractItemView::EditTriggers(QAbstractItemView::NoEditTriggers));
    m_chooseOutput->setEnabled(!blocked && project);
    m_import->setEnabled(!blocked && project);
    m_copyCsv->setEnabled(!blocked && project);
    m_export->setEnabled(!blocked && project && !m_outputRoot->text().trimmed().isEmpty());
    m_cancel->setEnabled(bool(m_job)
                         && !m_job->cancelled.load(std::memory_order_relaxed));
    m_close->setEnabled(!m_closing || !m_closeWhenIdle);
}

void TranslationEditorWindow::completeJob(const std::shared_ptr<JobState> &state)
{
    if (m_job != state)
        return;
    m_job.reset();
    m_watcher = nullptr;
    m_progressTimer->stop();
    const bool progressSignals = m_progress->blockSignals(true);
    m_progress->setRange(0, 1000);
    m_progress->setValue(0);
    m_progress->blockSignals(progressSignals);
    refreshControls();
    resolveDeferredClose();
}

void TranslationEditorWindow::setStatus(const QString &text)
{
    if (m_status && !m_shutdown)
        m_status->setText(text);
}

void TranslationEditorWindow::dismissDialog(QPointer<QDialog> &dialog)
{
    QPointer<QDialog> current = dialog;
    dialog = nullptr;
    if (!current)
        return;
    const bool blocked = current->blockSignals(true);
    current->reject();
    if (!current)
        return;
    current->blockSignals(blocked);
    current->deleteLater();
}

void TranslationEditorWindow::dismissPickers()
{
    QPointer<QDialog> source = m_sourceDialog.data();
    m_sourceDialog = nullptr;
    if (source) {
        source->blockSignals(true);
        source->reject();
        if (source)
            source->deleteLater();
    }
    QPointer<QDialog> output = m_outputDialog.data();
    m_outputDialog = nullptr;
    if (output) {
        output->blockSignals(true);
        output->reject();
        if (output)
            output->deleteLater();
    }
    QPointer<QDialog> import = m_importDialog.data();
    m_importDialog = nullptr;
    if (import) {
        import->blockSignals(true);
        import->reject();
        if (import)
            import->deleteLater();
    }
}

void TranslationEditorWindow::closeEvent(QCloseEvent *event)
{
    if (m_shutdown || m_allowClose) {
        m_closing = true;
        event->accept();
        if (!m_shutdown && !m_closeResolutionEmitted) {
            m_closeResolutionEmitted = true;
            emit closeResolved(true);
        }
        return;
    }
    event->ignore();
    if (m_closePrompt) {
        m_closePrompt->raise();
        m_closePrompt->activateWindow();
        return;
    }

    m_closing = true;
    // A running worker is keyed by both its immutable state and m_flow. Keep
    // that revision until its terminal callback clears m_job; otherwise an
    // approved cancel-and-close would orphan the job and wedge the window.
    if (!m_job)
        ++m_flow;
    QPointer<TranslationEditorWindow> guard(this);
    dismissPickers();
    if (!guard)
        return;
    dismissDialog(m_prompt);
    if (!guard)
        return;
    if (!m_job && !m_model->hasUnsavedChanges()) {
        m_allowClose = true;
        event->accept();
        if (!m_closeResolutionEmitted) {
            m_closeResolutionEmitted = true;
            emit closeResolved(true);
        }
        return;
    }

    const bool active = bool(m_job);
    auto *box = new QMessageBox(QMessageBox::Warning,
        tr("Close Translation Editor?"),
        active
            ? tr("A background operation is still running. Cancel it and close only after the worker has stopped? Unsaved table edits will be discarded.")
            : tr("There are unsaved translation edits. Discard them and close?"),
        QMessageBox::Yes | QMessageBox::Cancel, this);
    box->setObjectName(QStringLiteral("TranslationEditorCloseConfirmation"));
    box->setDefaultButton(QMessageBox::Cancel);
    box->setEscapeButton(QMessageBox::Cancel);
    box->setAttribute(Qt::WA_DeleteOnClose, true);
    if (QPushButton *yes = qobject_cast<QPushButton *>(box->button(QMessageBox::Yes))) {
        yes->setText(active ? tr("Cancel work and close") : tr("Discard and close"));
        yes->setAutoDefault(false);
    }
    m_closePrompt = box;
    connect(box, &QMessageBox::finished, this, [this, box](int result) {
        if (m_closePrompt != box)
            return;
        m_closePrompt = nullptr;
        if (result != QMessageBox::Yes) {
            m_closing = false;
            m_closeWhenIdle = false;
            refreshControls();
            emit closeResolved(false);
            return;
        }
        if (m_job) {
            m_closeWhenIdle = true;
            m_job->cancelled.store(true, std::memory_order_relaxed);
            setStatus(tr("Cancelling the background operation; this window will close after the worker stops…"));
            refreshControls();
        } else {
            m_allowClose = true;
            QTimer::singleShot(0, this, &QWidget::close);
        }
    });
    guard = this;
    box->open();
    if (!guard)
        return;
    refreshControls();
}

void TranslationEditorWindow::resolveDeferredClose()
{
    if (!m_closeWhenIdle || m_job || m_shutdown)
        return;
    m_closeWhenIdle = false;
    m_allowClose = true;
    QTimer::singleShot(0, this, &QWidget::close);
}

void TranslationEditorWindow::shutdown()
{
    if (m_shutdown)
        return;
    m_shutdown = true;
    m_closing = true;
    ++m_flow;
    if (m_job)
        m_job->cancelled.store(true, std::memory_order_relaxed);
    if (m_watcher)
        disconnect(m_watcher, nullptr, this, nullptr);
    if (m_progressTimer)
        m_progressTimer->stop();
    if (QCoreApplication::closingDown()) {
        m_sourceDialog = nullptr;
        m_outputDialog = nullptr;
        m_importDialog = nullptr;
        m_prompt = nullptr;
        m_closePrompt = nullptr;
        return;
    }
    QPointer<TranslationEditorWindow> guard(this);
    dismissPickers();
    if (!guard)
        return;
    dismissDialog(m_prompt);
    if (!guard)
        return;
    dismissDialog(m_closePrompt);
}
