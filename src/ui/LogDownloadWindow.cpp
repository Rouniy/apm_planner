#include "LogDownloadWindow.h"

#include "LogDownloadViewModel.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QLayout>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <algorithm>

namespace {

// Compact Qt-example-style flow layout: the six MP10 toolbar controls wrap
// instead of disappearing beyond the 420 px minimum window width.
class FlowLayout final : public QLayout
{
public:
    explicit FlowLayout(QWidget *parent, int spacing = 8)
        : QLayout(parent), m_spacing(spacing)
    {
        setContentsMargins(0, 0, 0, 0);
    }

    ~FlowLayout() override
    {
        while (QLayoutItem *item = takeAt(0)) {
            delete item;
        }
    }

    void addItem(QLayoutItem *item) override { m_items.append(item); }
    int count() const override { return m_items.size(); }
    QLayoutItem *itemAt(int index) const override
    {
        return index >= 0 && index < m_items.size()
            ? m_items.at(index) : nullptr;
    }
    QLayoutItem *takeAt(int index) override
    {
        return index >= 0 && index < m_items.size()
            ? m_items.takeAt(index) : nullptr;
    }
    Qt::Orientations expandingDirections() const override { return {}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override
    {
        return arrange(QRect(0, 0, width, 0), true);
    }
    QSize sizeHint() const override { return minimumSize(); }
    QSize minimumSize() const override
    {
        QSize size;
        for (QLayoutItem *item : m_items) {
            size = size.expandedTo(item->minimumSize());
        }
        const QMargins margins = contentsMargins();
        return size + QSize(margins.left() + margins.right(),
                            margins.top() + margins.bottom());
    }
    void setGeometry(const QRect &rect) override
    {
        QLayout::setGeometry(rect);
        arrange(rect, false);
    }

private:
    int arrange(const QRect &rect, bool testOnly) const
    {
        int x = rect.x();
        int y = rect.y();
        int lineHeight = 0;
        for (QLayoutItem *item : m_items) {
            const QSize hint = item->sizeHint();
            const int nextX = x + hint.width() + m_spacing;
            if (nextX - m_spacing > rect.right() + 1
                && lineHeight > 0) {
                x = rect.x();
                y += lineHeight + m_spacing;
                lineHeight = 0;
            }
            if (!testOnly) {
                item->setGeometry(QRect(QPoint(x, y), hint));
            }
            x += hint.width() + m_spacing;
            lineHeight = std::max(lineHeight, hint.height());
        }
        return y + lineHeight - rect.y();
    }

    QList<QLayoutItem *> m_items;
    int m_spacing = 8;
};

QString pickerStartPath(const QString &fileName = QString())
{
    QString folder = QStandardPaths::writableLocation(
        QStandardPaths::DocumentsLocation);
    if (folder.isEmpty()) {
        folder = QDir::homePath();
    }
    return fileName.isEmpty() ? folder : QDir(folder).filePath(fileName);
}

} // namespace

LogDownloadWindow::LogDownloadWindow(
    LogDownloadViewModel *viewModel, QWidget *owner)
    : QWidget(owner, Qt::Window)
    , m_viewModel(viewModel)
{
    setObjectName(QStringLiteral("LogDownloadWindow"));
    setWindowTitle(tr("Download Logs (MAVLink)"));
    setWindowModality(Qt::NonModal);
    setAttribute(Qt::WA_DeleteOnClose, true);
    resize(540, 440);
    setMinimumSize(420, 320);
    if (owner) {
        move(owner->frameGeometry().center() - rect().center());
    }
    if (m_viewModel && !m_viewModel->parent()) {
        m_viewModel->setParent(this);
    }

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(8);

    auto *toolbar = new QWidget(this);
    toolbar->setObjectName(QStringLiteral("LogDownloadToolbar"));
    auto *flow = new FlowLayout(toolbar);
    m_refreshButton = new QPushButton(tr("Refresh List"), toolbar);
    m_refreshButton->setObjectName(QStringLiteral("RefreshListButton"));
    m_downloadSelectedButton = new QPushButton(
        tr("Download Selected"), toolbar);
    m_downloadSelectedButton->setObjectName(
        QStringLiteral("DownloadSelectedButton"));
    m_downloadAllButton = new QPushButton(tr("Download All…"), toolbar);
    m_downloadAllButton->setObjectName(QStringLiteral("DownloadAllButton"));
    m_createKmlCheckBox = new QCheckBox(tr("Create KML"), toolbar);
    m_createKmlCheckBox->setObjectName(QStringLiteral("CreateKmlCheckBox"));
    m_eraseAllButton = new QPushButton(tr("Erase All…"), toolbar);
    m_eraseAllButton->setObjectName(QStringLiteral("EraseAllButton"));
    m_eraseAllButton->setStyleSheet(QStringLiteral("color: #ff7777;"));
    m_cancelButton = new QPushButton(tr("Cancel"), toolbar);
    m_cancelButton->setObjectName(QStringLiteral("CancelDownloadButton"));
    flow->addWidget(m_refreshButton);
    flow->addWidget(m_downloadSelectedButton);
    flow->addWidget(m_downloadAllButton);
    flow->addWidget(m_createKmlCheckBox);
    flow->addWidget(m_eraseAllButton);
    flow->addWidget(m_cancelButton);
    root->addWidget(toolbar);

    m_table = new QTableWidget(this);
    m_table->setObjectName(QStringLiteral("LogDownloadGrid"));
    m_table->setColumnCount(3);
    m_table->setHorizontalHeaderLabels(
        {tr("Id"), tr("Date"), tr("Size")});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setShowGrid(false);
    m_table->verticalHeader()->hide();
    m_table->horizontalHeader()->setSectionResizeMode(
        0, QHeaderView::Fixed);
    m_table->setColumnWidth(0, 60);
    m_table->horizontalHeader()->setSectionResizeMode(
        1, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(
        2, QHeaderView::ResizeToContents);
    root->addWidget(m_table, 1);

    m_progress = new QProgressBar(this);
    m_progress->setObjectName(QStringLiteral("LogDownloadProgress"));
    m_progress->setRange(0, 100);
    m_progress->setFixedHeight(16);
    root->addWidget(m_progress);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("LogDownloadStatus"));
    m_status->setWordWrap(true);
    root->addWidget(m_status);

    connect(m_refreshButton, &QPushButton::clicked,
            this, [this]() {
        if (m_viewModel) {
            m_viewModel->refresh();
        }
    });
    connect(m_downloadSelectedButton, &QPushButton::clicked,
            this, &LogDownloadWindow::chooseSelectedDestination);
    connect(m_downloadAllButton, &QPushButton::clicked,
            this, [this]() {
        if (m_viewModel) {
            m_viewModel->requestDownloadAll();
        }
    });
    connect(m_createKmlCheckBox, &QCheckBox::toggled,
            this, [this](bool checked) {
        if (m_viewModel) {
            m_viewModel->setCreateKmlAfterDownload(checked);
        }
    });
    connect(m_eraseAllButton, &QPushButton::clicked,
            this, &LogDownloadWindow::confirmErase);
    connect(m_cancelButton, &QPushButton::clicked,
            this, [this]() {
        if (m_viewModel) {
            m_viewModel->cancelOwnDownload();
        }
    });
    connect(m_table, &QTableWidget::itemSelectionChanged,
            this, [this]() {
        if (!m_viewModel) {
            return;
        }
        const int row = m_table->currentRow();
        QTableWidgetItem *const item = row >= 0
            ? m_table->item(row, 0) : nullptr;
        m_viewModel->setSelectedLogId(
            item ? item->data(Qt::UserRole).toInt() : -1);
    });

    if (m_viewModel) {
        connect(m_viewModel, &LogDownloadViewModel::stateChanged,
                this, &LogDownloadWindow::syncState);
        connect(m_viewModel, &LogDownloadViewModel::logsChanged,
                this, &LogDownloadWindow::rebuildRows);
        connect(m_viewModel,
                &LogDownloadViewModel::downloadAllDestinationRequested,
                this, &LogDownloadWindow::chooseAllDestination);
        connect(m_viewModel, &QObject::destroyed, this, [this]() {
            m_viewModel = nullptr;
            syncState();
        });
    }
    rebuildRows();
    syncState();
}

LogDownloadWindow::~LogDownloadWindow()
{
    if (m_table) disconnect(m_table, nullptr, this, nullptr);
    // QWidget destroys child QObjects from its base destructor, after this
    // class's QPointer and widget fields have already been destroyed. Prevent
    // the model's destroyed/state signals from re-entering those fields then.
    if (m_viewModel) {
        disconnect(m_viewModel.data(), nullptr, this, nullptr);
    }
}

void LogDownloadWindow::closeEvent(QCloseEvent *event)
{
    if (m_viewModel) {
        m_viewModel->shutdown();
    }
    QWidget::closeEvent(event);
}

void LogDownloadWindow::rebuildRows()
{
    const QSignalBlocker blocker(m_table);
    m_table->setRowCount(0);
    if (!m_viewModel) {
        return;
    }
    const QVector<LogDownloadRow> rows = m_viewModel->logs();
    m_table->setRowCount(rows.size());
    for (int index = 0; index < rows.size(); ++index) {
        const LogDownloadRow &row = rows.at(index);
        auto *id = new QTableWidgetItem(QString::number(row.id));
        id->setData(Qt::UserRole, row.id);
        auto *date = new QTableWidgetItem(row.timeText());
        auto *size = new QTableWidgetItem(row.sizeText());
        size->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_table->setItem(index, 0, id);
        m_table->setItem(index, 1, date);
        m_table->setItem(index, 2, size);
        if (m_viewModel->selectedLogId() == row.id) {
            m_table->selectRow(index);
        }
    }
}

void LogDownloadWindow::syncState()
{
    const bool available = bool(m_viewModel);
    const bool busy = available && m_viewModel->isBusy();
    m_refreshButton->setEnabled(available && !busy);
    m_downloadSelectedButton->setEnabled(available && !busy);
    m_downloadAllButton->setEnabled(available && !busy);
    m_eraseAllButton->setEnabled(available && !busy);
    m_cancelButton->setEnabled(
        available && m_viewModel->isDownloading());
    if (!available) {
        m_progress->setValue(0);
        m_status->setText(tr("Log transfer service unavailable."));
        return;
    }
    m_createKmlCheckBox->setChecked(
        m_viewModel->createKmlAfterDownload());
    m_progress->setValue(qRound(m_viewModel->progress()));
    QString statusText = m_viewModel->status();
    const QString source = m_viewModel->targetSource();
    if (!source.isEmpty()) {
        statusText = tr("Source: %1").arg(source)
            + (statusText.isEmpty() ? QString()
                                    : QStringLiteral("\n") + statusText);
    }
    m_status->setText(statusText);
}

void LogDownloadWindow::chooseSelectedDestination()
{
    QPointer<LogDownloadWindow> guardedThis(this);
    QPointer<LogDownloadViewModel> guardedModel(m_viewModel);
    if (!guardedModel) {
        return;
    }
    QString suggested;
    const quint64 preparation =
        guardedModel->prepareSelectedDownload(&suggested);
    if (preparation == 0) {
        return;
    }

    auto *picker = new QFileDialog(
        this, tr("Save dataflash log"), pickerStartPath(suggested),
        tr("Dataflash log (*.bin)"));
    picker->setObjectName(QStringLiteral("LogDownloadSavePicker"));
    picker->setAcceptMode(QFileDialog::AcceptSave);
    picker->setFileMode(QFileDialog::AnyFile);
    picker->setDefaultSuffix(QStringLiteral("bin"));
    QPointer<QFileDialog> guardedPicker(picker);
    const int pickerResult = picker->exec();
    if (!guardedThis || !guardedModel || !guardedPicker) {
        if (guardedPicker) {
            delete guardedPicker.data();
        }
        return;
    }
    const QString destination = pickerResult == QDialog::Accepted
        && !guardedPicker->selectedFiles().isEmpty()
        ? guardedPicker->selectedFiles().constFirst() : QString();
    delete guardedPicker.data();
    if (!guardedThis || !guardedModel) {
        return;
    }
    if (destination.isEmpty()) {
        guardedModel->abandonPreparation(preparation);
        return;
    }
    bool overwriteKml = false;
    const QString kmlDestination =
        LogDownloadViewModel::kmlFileName(destination);
    if (guardedModel->createKmlAfterDownload()
        && QFileInfo::exists(kmlDestination)) {
        auto *confirmation = new QMessageBox(
            QMessageBox::Warning, tr("Overwrite KML track?"),
            tr("The KML destination already exists:\n%1\n\nReplace it?")
                .arg(QDir::toNativeSeparators(kmlDestination)),
            QMessageBox::Yes | QMessageBox::Cancel, this);
        confirmation->setObjectName(
            QStringLiteral("LogDownloadKmlOverwriteConfirmation"));
        confirmation->setDefaultButton(QMessageBox::Cancel);
        confirmation->setEscapeButton(QMessageBox::Cancel);
        QPointer<QMessageBox> guardedConfirmation(confirmation);
        const int confirmationResult = confirmation->exec();
        if (!guardedThis || !guardedModel || !guardedConfirmation) {
            if (guardedConfirmation) {
                delete guardedConfirmation.data();
            }
            return;
        }
        overwriteKml = confirmationResult == QMessageBox::Yes;
        delete guardedConfirmation.data();
        if (!guardedThis || !guardedModel) {
            return;
        }
    }
    guardedModel->startPreparedSelectedDownload(
        preparation, destination, overwriteKml);
}

void LogDownloadWindow::chooseAllDestination()
{
    QPointer<LogDownloadWindow> guardedThis(this);
    QPointer<LogDownloadViewModel> guardedModel(m_viewModel);
    if (!guardedModel) {
        return;
    }
    const quint64 preparation = guardedModel->prepareDownloadAll();
    if (preparation == 0) {
        return;
    }

    auto *picker = new QFileDialog(
        this, tr("Select folder for all DataFlash logs"), pickerStartPath());
    picker->setObjectName(QStringLiteral("LogDownloadFolderPicker"));
    picker->setAcceptMode(QFileDialog::AcceptOpen);
    picker->setFileMode(QFileDialog::Directory);
    picker->setOption(QFileDialog::ShowDirsOnly, true);
    QPointer<QFileDialog> guardedPicker(picker);
    const int pickerResult = picker->exec();
    if (!guardedThis || !guardedModel || !guardedPicker) {
        if (guardedPicker) {
            delete guardedPicker.data();
        }
        return;
    }
    const QString directory = pickerResult == QDialog::Accepted
        && !guardedPicker->selectedFiles().isEmpty()
        ? guardedPicker->selectedFiles().constFirst() : QString();
    delete guardedPicker.data();
    if (!guardedThis || !guardedModel) {
        return;
    }
    if (directory.isEmpty()) {
        guardedModel->abandonPreparation(preparation);
        return;
    }

    const QStringList collisions =
        guardedModel->existingDownloadAllFiles(preparation, directory);
    bool overwrite = collisions.isEmpty();
    if (!overwrite) {
        auto *confirmation = new QMessageBox(
            QMessageBox::Warning, tr("Overwrite downloaded logs?"),
            tr("%1 destination file(s) already exist. Replace all of them?")
                .arg(collisions.size()),
            QMessageBox::Yes | QMessageBox::Cancel, this);
        confirmation->setObjectName(
            QStringLiteral("LogDownloadOverwriteConfirmation"));
        confirmation->setDefaultButton(QMessageBox::Cancel);
        confirmation->setEscapeButton(QMessageBox::Cancel);
        QPointer<QMessageBox> guardedConfirmation(confirmation);
        const int confirmationResult = confirmation->exec();
        if (!guardedThis || !guardedModel || !guardedConfirmation) {
            if (guardedConfirmation) {
                delete guardedConfirmation.data();
            }
            return;
        }
        overwrite = confirmationResult == QMessageBox::Yes;
        delete guardedConfirmation.data();
        if (!guardedThis || !guardedModel) {
            return;
        }
    }
    guardedModel->startPreparedDownloadAll(
        preparation, directory, overwrite);
}

void LogDownloadWindow::confirmErase()
{
    QPointer<LogDownloadWindow> guardedThis(this);
    QPointer<LogDownloadViewModel> guardedModel(m_viewModel);
    if (!guardedModel) {
        return;
    }
    const quint64 preparation = guardedModel->prepareErase();
    if (preparation == 0) {
        return;
    }
    auto *confirmation = new QMessageBox(
        QMessageBox::Warning, tr("Erase onboard logs"),
        tr("Erase every DataFlash log stored on the vehicle?\n\n"
           "Download anything you need first. This operation cannot be "
           "undone."),
        QMessageBox::Yes | QMessageBox::Cancel, this);
    confirmation->setObjectName(
        QStringLiteral("LogDownloadEraseConfirmation"));
    confirmation->setDefaultButton(QMessageBox::Cancel);
    confirmation->setEscapeButton(QMessageBox::Cancel);
    QPointer<QMessageBox> guardedConfirmation(confirmation);
    const int confirmationResult = confirmation->exec();
    if (!guardedThis || !guardedModel || !guardedConfirmation) {
        if (guardedConfirmation) {
            delete guardedConfirmation.data();
        }
        return;
    }
    delete guardedConfirmation.data();
    if (!guardedThis || !guardedModel) {
        return;
    }
    guardedModel->completePreparedErase(
        preparation, confirmationResult == QMessageBox::Yes);
}
