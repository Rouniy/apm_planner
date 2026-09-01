#include "ConfigElevationSourcesView.h"

#include "ConfigElevationSourcesViewModel.h"
#include "ui/BackstageView.h"

#include <QAbstractItemView>
#include <QFileDialog>
#include <QFrame>
#include <QGridLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

namespace {
QTableWidgetItem *readOnlyItem(const QString &text)
{
    auto *item = new QTableWidgetItem(text);
    item->setToolTip(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
}
}

ConfigElevationSourcesView::ConfigElevationSourcesView(QWidget *parent)
    : ConfigElevationSourcesView(new ConfigElevationSourcesViewModel, parent)
{
    m_viewModel->setParent(this);
}

ConfigElevationSourcesView::ConfigElevationSourcesView(
    ConfigElevationSourcesViewModel *viewModel, QWidget *parent)
    : QWidget(parent),
      m_viewModel(viewModel ? viewModel
                            : new ConfigElevationSourcesViewModel(this))
{
    setObjectName(QStringLiteral("ConfigElevationSourcesView"));
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(QStringLiteral(
        "ConfigElevationSourcesView { background: #1A201D; color: #E6EDE9; }"
        "QLabel { color: #E6EDE9; }"
        "QLabel#elevationSourcesTitle { color: #E8E8E8; font-size: 16px;"
        " font-weight: bold; }"
        "QLabel[elevationSourceStatus=\"true\"] { color: #9CDCFE; }"
        "QLineEdit { background: #0D1210; color: #E6EDE9;"
        " border: 1px solid #666; padding: 4px; }"
        "QPushButton { background: #202623; color: #E6EDE9;"
        " border: 1px solid #666; padding: 4px 10px; min-height: 22px; }"
        "QPushButton:hover { background: #2E2E2F; }"
        "QPushButton:disabled { color: #777879; }"
        "QTabWidget::pane { border: 0; background: #0D1210; }"
        "QTabBar::tab { background: #202623; color: #E6EDE9;"
        " padding: 6px 10px; border: 1px solid #666; }"
        "QTabBar::tab:selected { background: #2E2E2F; }"
        "QTableWidget { background: #0D1210; color: #E6EDE9;"
        " gridline-color: #2A322D; border: 0; }"
        "QTableWidget::item { border-bottom: 1px solid #2A322D; }"
        "QHeaderView::section { background: #202623; color: #E6EDE9;"
        " border: 1px solid #2A322D; padding: 5px; }"
        "QProgressBar { border: 1px solid #666; background: #0D1210;"
        " color: #E6EDE9; text-align: center; }"
        "QProgressBar::chunk { background: #34D399; }"));
    buildUi();

    connect(m_directoryEdit, &QLineEdit::textChanged,
            m_viewModel, &ConfigElevationSourcesViewModel::SetDirectoryPath);
    connect(m_browseButton, &QPushButton::clicked,
            this, &ConfigElevationSourcesView::browse);
    connect(m_saveButton, &QPushButton::clicked,
            m_viewModel, &ConfigElevationSourcesViewModel::Rescan);
    connect(m_clearButton, &QPushButton::clicked,
            this, &ConfigElevationSourcesView::clearSaved);
    connect(m_cancelButton, &QPushButton::clicked,
            m_viewModel, &ConfigElevationSourcesViewModel::Cancel);
    connect(m_viewModel,
            &ConfigElevationSourcesViewModel::directoryPathChanged,
            this, &ConfigElevationSourcesView::syncDirectory);
    connect(m_viewModel, &ConfigElevationSourcesViewModel::statusChanged,
            this, &ConfigElevationSourcesView::syncStatus);
    connect(m_viewModel,
            &ConfigElevationSourcesViewModel::nativeGdalStatusChanged,
            this, &ConfigElevationSourcesView::syncNativeStatus);
    connect(m_viewModel, &ConfigElevationSourcesViewModel::progressChanged,
            this, &ConfigElevationSourcesView::syncProgress);
    connect(m_viewModel, &ConfigElevationSourcesViewModel::busyChanged,
            this, &ConfigElevationSourcesView::syncBusy);
    connect(m_viewModel, &ConfigElevationSourcesViewModel::filesChanged,
            this, &ConfigElevationSourcesView::syncElevationRows);
    connect(m_viewModel, &ConfigElevationSourcesViewModel::rasterFilesChanged,
            this, &ConfigElevationSourcesView::syncRasterRows);

    syncDirectory(m_viewModel->DirectoryPath());
    syncStatus(m_viewModel->Status());
    syncNativeStatus(m_viewModel->NativeGdalStatus());
    syncProgress(m_viewModel->Progress(), m_viewModel->ProgressMaximum());
    syncBusy(m_viewModel->IsBusy());
    syncElevationRows();
    syncRasterRows();
}

void ConfigElevationSourcesView::browse()
{
    if (m_viewModel->IsBusy()) {
        return;
    }
    const QString directory = QFileDialog::getExistingDirectory(
        this, tr("Select GeoTIFF / DTED elevation directory"),
        m_viewModel->DirectoryPath(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (!directory.isEmpty()) {
        m_viewModel->SelectAndScan(directory);
    }
}

void ConfigElevationSourcesView::clearSaved()
{
    if (m_viewModel->IsBusy()) {
        return;
    }
    const bool confirmed = QMessageBox::question(
        this,
        tr("Clear Elevation Directory"),
        tr("Stop loading this local GeoTIFF/DTED directory on future starts? Files already indexed remain active until Mission Planner is restarted."),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No) == QMessageBox::Yes;
    m_viewModel->ClearSaved(confirmed);
}

void ConfigElevationSourcesView::syncDirectory(const QString &path)
{
    if (m_directoryEdit->text() == path) {
        return;
    }
    const QSignalBlocker blocker(m_directoryEdit);
    m_directoryEdit->setText(path);
}

void ConfigElevationSourcesView::syncStatus(const QString &status)
{
    m_statusLabel->setText(status);
}

void ConfigElevationSourcesView::syncNativeStatus(const QString &status)
{
    m_nativeStatusLabel->setText(status);
}

void ConfigElevationSourcesView::syncProgress(int value, int maximum)
{
    m_progressBar->setMaximum(qMax(1, maximum));
    m_progressBar->setValue(qBound(0, value, m_progressBar->maximum()));
}

void ConfigElevationSourcesView::syncBusy(bool busy)
{
    m_browseButton->setEnabled(!busy);
    m_saveButton->setEnabled(!busy);
    m_clearButton->setEnabled(!busy);
    m_cancelButton->setEnabled(busy);
}

void ConfigElevationSourcesView::syncElevationRows()
{
    const QList<ElevationSourceFile> files = m_viewModel->Files();
    const bool sorting = m_elevationTable->isSortingEnabled();
    m_elevationTable->setSortingEnabled(false);
    m_elevationTable->setRowCount(files.size());
    for (int row = 0; row < files.size(); ++row) {
        const ElevationSourceFile &file = files.at(row);
        m_elevationTable->setItem(row, 0, readOnlyItem(file.Name()));
        m_elevationTable->setItem(row, 1, readOnlyItem(file.Format));
        m_elevationTable->setItem(row, 2, readOnlyItem(file.State()));
        m_elevationTable->setItem(row, 3, readOnlyItem(file.Coverage));
        m_elevationTable->setItem(row, 4, readOnlyItem(file.FullPath));
        m_elevationTable->setItem(row, 5, readOnlyItem(file.Error));
    }
    m_elevationTable->setSortingEnabled(sorting);
}

void ConfigElevationSourcesView::syncRasterRows()
{
    const QList<NativeGdalRasterFile> files = m_viewModel->RasterFiles();
    const bool sorting = m_rasterTable->isSortingEnabled();
    m_rasterTable->setSortingEnabled(false);
    m_rasterTable->setRowCount(files.size());
    for (int row = 0; row < files.size(); ++row) {
        const NativeGdalRasterFile &file = files.at(row);
        m_rasterTable->setItem(row, 0, readOnlyItem(file.Name()));
        m_rasterTable->setItem(row, 1, readOnlyItem(file.Driver));
        m_rasterTable->setItem(row, 2, readOnlyItem(file.State()));
        m_rasterTable->setItem(row, 3, readOnlyItem(file.Size));
        m_rasterTable->setItem(row, 4, readOnlyItem(file.Coverage));
        m_rasterTable->setItem(row, 5, readOnlyItem(file.FullPath));
        m_rasterTable->setItem(row, 6, readOnlyItem(file.Error));
    }
    m_rasterTable->setSortingEnabled(sorting);
}

void ConfigElevationSourcesView::buildUi()
{
    auto *root = new QGridLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setHorizontalSpacing(0);
    root->setVerticalSpacing(10);
    root->setRowStretch(3, 1);

    auto *heading = new QWidget(this);
    auto *headingLayout = new QVBoxLayout(heading);
    headingLayout->setContentsMargins(0, 0, 0, 0);
    headingLayout->setSpacing(4);
    auto *title = new QLabel(tr("Elevation & Local Raster Sources"), heading);
    title->setObjectName(QStringLiteral("elevationSourcesTitle"));
    headingLayout->addWidget(title);
    auto *description = new QLabel(
        tr("Load local GeoTIFF/DTED elevation models and the official GDAL Custom raster-map overlay. Local DEM files are checked before the downloaded SRTM cache; native GDAL remains optional."),
        heading);
    description->setObjectName(QStringLiteral("elevationSourcesDescription"));
    description->setWordWrap(true);
    description->setMaximumWidth(850);
    headingLayout->addWidget(description, 0, Qt::AlignLeft);
    root->addWidget(heading, 0, 0);

    auto *directoryRow = new QWidget(this);
    auto *directoryLayout = new QGridLayout(directoryRow);
    directoryLayout->setContentsMargins(0, 0, 0, 0);
    directoryLayout->setHorizontalSpacing(8);
    auto *directoryLabel = new QLabel(tr("Local data directory"), directoryRow);
    directoryLabel->setObjectName(QStringLiteral("localDataDirectoryLabel"));
    directoryLayout->addWidget(directoryLabel, 0, 0);
    m_directoryEdit = new QLineEdit(directoryRow);
    m_directoryEdit->setObjectName(QStringLiteral("directoryPathTextBox"));
    m_directoryEdit->setPlaceholderText(
        tr("Directory containing elevation or georeferenced raster files"));
    directoryLayout->addWidget(m_directoryEdit, 0, 1);
    directoryLayout->setColumnStretch(1, 1);
    m_browseButton = new QPushButton(tr("Browse…"), directoryRow);
    m_browseButton->setObjectName(QStringLiteral("BrowseButton"));
    directoryLayout->addWidget(m_browseButton, 0, 2);
    m_saveButton = new QPushButton(tr("Save & Scan"), directoryRow);
    m_saveButton->setObjectName(QStringLiteral("saveAndScanButton"));
    directoryLayout->addWidget(m_saveButton, 0, 3);
    m_clearButton = new QPushButton(tr("Clear Saved"), directoryRow);
    m_clearButton->setObjectName(QStringLiteral("clearSavedButton"));
    directoryLayout->addWidget(m_clearButton, 0, 4);
    root->addWidget(directoryRow, 1, 0);

    auto *help = new QLabel(
        tr("The selected directory is restored automatically at every start. Subdirectories are scanned; one unreadable file does not prevent other sources from loading. GDAL datasets stay on disk and are sampled per map tile."),
        this);
    help->setObjectName(QStringLiteral("elevationSourcesHelp"));
    help->setWordWrap(true);
    root->addWidget(help, 2, 0);

    auto *border = new QFrame(this);
    border->setObjectName(QStringLiteral("elevationSourcesBorder"));
    border->setFrameShape(QFrame::Box);
    border->setLineWidth(1);
    border->setStyleSheet(QStringLiteral(
        "QFrame#elevationSourcesBorder { border: 1px solid #666; }"));
    auto *borderLayout = new QVBoxLayout(border);
    borderLayout->setContentsMargins(0, 0, 0, 0);
    auto *tabs = new QTabWidget(border);
    tabs->setObjectName(QStringLiteral("elevationSourcesTabs"));

    m_elevationTable = new QTableWidget(tabs);
    m_elevationTable->setObjectName(QStringLiteral("elevationSourcesTable"));
    configureTable(m_elevationTable,
                   {tr("Name"), tr("Format"), tr("State"),
                    tr("Coverage / size"), tr("Path"), tr("Error")},
                   {200, 90, 90, 360, 300, 260});
    tabs->addTab(m_elevationTable, tr("Elevation (GeoTIFF / DTED)"));

    auto *rasterTab = new QWidget(tabs);
    auto *rasterLayout = new QVBoxLayout(rasterTab);
    rasterLayout->setContentsMargins(0, 0, 0, 0);
    rasterLayout->setSpacing(6);
    m_rasterTable = new QTableWidget(rasterTab);
    m_rasterTable->setObjectName(QStringLiteral("nativeGdalRasterTable"));
    configureTable(m_rasterTable,
                   {tr("Name"), tr("Driver"), tr("State"), tr("Size"),
                    tr("Coverage"), tr("Path"), tr("Error")},
                   {200, 100, 90, 180, 320, 300, 260});
    rasterLayout->addWidget(m_rasterTable, 1);
    m_nativeStatusLabel = new QLabel(rasterTab);
    m_nativeStatusLabel->setObjectName(QStringLiteral("nativeGdalStatus"));
    m_nativeStatusLabel->setProperty("elevationSourceStatus", true);
    m_nativeStatusLabel->setContentsMargins(6, 0, 6, 0);
    m_nativeStatusLabel->setWordWrap(true);
    rasterLayout->addWidget(m_nativeStatusLabel);
    tabs->addTab(rasterTab, tr("GDAL Custom map rasters"));
    borderLayout->addWidget(tabs);
    root->addWidget(border, 3, 0);

    auto *footer = new QWidget(this);
    auto *footerLayout = new QGridLayout(footer);
    footerLayout->setContentsMargins(0, 0, 0, 0);
    footerLayout->setHorizontalSpacing(8);
    footerLayout->setVerticalSpacing(5);
    m_progressBar = new QProgressBar(footer);
    m_progressBar->setObjectName(QStringLiteral("elevationSourcesProgress"));
    m_progressBar->setRange(0, 1);
    m_progressBar->setFixedHeight(15);
    m_progressBar->setTextVisible(false);
    footerLayout->addWidget(m_progressBar, 0, 0);
    m_cancelButton = new QPushButton(tr("Cancel"), footer);
    m_cancelButton->setObjectName(QStringLiteral("cancelButton"));
    footerLayout->addWidget(m_cancelButton, 0, 1);
    m_statusLabel = new QLabel(footer);
    m_statusLabel->setObjectName(QStringLiteral("elevationSourcesStatus"));
    m_statusLabel->setProperty("elevationSourceStatus", true);
    m_statusLabel->setWordWrap(true);
    footerLayout->addWidget(m_statusLabel, 1, 0, 1, 2);
    footerLayout->setColumnStretch(0, 1);
    root->addWidget(footer, 4, 0);
}

void ConfigElevationSourcesView::configureTable(
    QTableWidget *table, const QStringList &headers,
    const QList<int> &widths)
{
    table->setColumnCount(headers.size());
    table->setHorizontalHeaderLabels(headers);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setShowGrid(false);
    table->setSortingEnabled(true);
    table->verticalHeader()->hide();
    table->horizontalHeader()->setSectionsClickable(true);
    table->horizontalHeader()->setSectionsMovable(false);
    for (int column = 0; column < widths.size(); ++column) {
        table->horizontalHeader()->setSectionResizeMode(
            column, QHeaderView::Interactive);
        table->setColumnWidth(column, widths.at(column));
    }
}

BackstagePage configElevationSourcesBackstagePage()
{
    BackstagePage page;
    page.id = QStringLiteral("ConfigElevationSourcesView");
    page.header = ConfigElevationSourcesView::tr("Elevation Sources");
    page.isSub = true;
    page.isAdvanced = true;
    page.requiresConnection = false;
    page.allowsPartialParameters = false;
    page.factory = [](QWidget *parent) {
        return new ConfigElevationSourcesView(parent);
    };
    return page;
}
