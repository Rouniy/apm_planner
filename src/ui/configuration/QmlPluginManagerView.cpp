#include "QmlPluginManagerView.h"

#include "qml/QmlPluginManager.h"

#include <QAbstractItemView>
#include <QDesktopServices>
#include <QDir>
#include <QFont>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QUrl>
#include <QVariantMap>
#include <QVBoxLayout>

namespace
{
constexpr int PluginIdRole = Qt::UserRole;
}

QmlPluginManagerView::QmlPluginManagerView(QmlPluginManager *manager,
                                           QWidget *parent)
    : QWidget(parent)
    , m_manager(manager)
{
    setObjectName(QStringLiteral("QmlPluginManagerView"));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(10);

    auto *title = new QLabel(tr("QML Plugins"), this);
    title->setObjectName(QStringLiteral("Title"));
    QFont titleFont = title->font();
    titleFont.setPointSize(titleFont.pointSize() + 5);
    titleFont.setBold(true);
    title->setFont(titleFont);
    layout->addWidget(title);

    auto *instructions = new QLabel(
        tr("Trusted in-process QML extensions are discovered from the "
           "application and user plugin folders. Reload closes open plugin "
           "windows and rebuilds the Tools menu."), this);
    instructions->setObjectName(QStringLiteral("Instructions"));
    instructions->setWordWrap(true);
    layout->addWidget(instructions);

    m_directory = new QLabel(this);
    m_directory->setObjectName(QStringLiteral("UserPluginDirectory"));
    m_directory->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_directory);

    m_plugins = new QTableWidget(this);
    m_plugins->setObjectName(QStringLiteral("Plugins"));
    m_plugins->setColumnCount(5);
    m_plugins->setHorizontalHeaderLabels({tr("Name"), tr("ID"),
                                           tr("Version"), tr("Author"),
                                           tr("Directory")});
    m_plugins->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_plugins->setSelectionMode(QAbstractItemView::SingleSelection);
    m_plugins->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_plugins->setAlternatingRowColors(true);
    m_plugins->verticalHeader()->hide();
    m_plugins->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(m_plugins, 2);

    auto *actions = new QHBoxLayout;
    actions->setSpacing(8);
    m_reload = new QPushButton(tr("Reload"), this);
    m_reload->setObjectName(QStringLiteral("ReloadButton"));
    m_open = new QPushButton(tr("Open"), this);
    m_open->setObjectName(QStringLiteral("OpenButton"));
    m_openFolder = new QPushButton(tr("Open User Folder"), this);
    m_openFolder->setObjectName(QStringLiteral("OpenFolderButton"));
    actions->addWidget(m_reload);
    actions->addWidget(m_open);
    actions->addWidget(m_openFolder);
    actions->addStretch(1);
    layout->addLayout(actions);

    auto *diagnosticsLabel = new QLabel(tr("Diagnostics"), this);
    diagnosticsLabel->setObjectName(QStringLiteral("DiagnosticsLabel"));
    layout->addWidget(diagnosticsLabel);
    m_diagnostics = new QPlainTextEdit(this);
    m_diagnostics->setObjectName(QStringLiteral("Diagnostics"));
    m_diagnostics->setReadOnly(true);
    m_diagnostics->setMaximumHeight(150);
    layout->addWidget(m_diagnostics, 1);

    connect(m_reload, &QPushButton::clicked,
            this, &QmlPluginManagerView::Reload);
    connect(m_open, &QPushButton::clicked,
            this, &QmlPluginManagerView::OpenSelected);
    connect(m_openFolder, &QPushButton::clicked,
            this, &QmlPluginManagerView::OpenUserFolder);
    connect(m_plugins, &QTableWidget::itemSelectionChanged,
            this, [this]() {
        m_open->setEnabled(m_manager && !selectedPluginId().isEmpty());
    });
    connect(m_plugins, &QTableWidget::itemDoubleClicked,
            this, [this](QTableWidgetItem *) { OpenSelected(); });

    if (m_manager) {
        connect(m_manager, &QmlPluginManager::pluginsChanged,
                this, &QmlPluginManagerView::Refresh);
        connect(m_manager, &QObject::destroyed, this, [this]() {
            m_manager = nullptr;
            Refresh();
        });
    }
    Refresh();
}

void QmlPluginManagerView::Refresh()
{
    const bool available = !m_manager.isNull();
    m_reload->setEnabled(available);
    m_openFolder->setEnabled(available);
    m_open->setEnabled(available && !selectedPluginId().isEmpty());

    m_plugins->setRowCount(0);
    if (!available) {
        m_directory->setText(tr("QML plugin manager is unavailable."));
        m_diagnostics->setPlainText(
            tr("The application-owned plugin service is unavailable."));
        return;
    }

    m_directory->setText(tr("User folder: %1")
                             .arg(m_manager->writablePluginDirectory()));
    const QVariantList plugins = m_manager->plugins();
    m_plugins->setRowCount(plugins.size());
    for (int row = 0; row < plugins.size(); ++row) {
        const QVariantMap plugin = plugins.at(row).toMap();
        const QString id = plugin.value(QStringLiteral("id")).toString();
        const QStringList values = {
            plugin.value(QStringLiteral("name")).toString(), id,
            plugin.value(QStringLiteral("version")).toString(),
            plugin.value(QStringLiteral("author")).toString(),
            plugin.value(QStringLiteral("directory")).toString()
        };
        for (int column = 0; column < values.size(); ++column) {
            auto *item = new QTableWidgetItem(values.at(column));
            item->setData(PluginIdRole, id);
            if (column == 0) {
                item->setToolTip(
                    plugin.value(QStringLiteral("description")).toString());
            }
            m_plugins->setItem(row, column, item);
        }
    }
    m_plugins->resizeColumnsToContents();

    const QStringList errors = m_manager->errors();
    m_diagnostics->setPlainText(
        errors.isEmpty() ? tr("No plugin diagnostics.")
                         : errors.join(QLatin1Char('\n')));
    m_open->setEnabled(!selectedPluginId().isEmpty());
}

void QmlPluginManagerView::Reload()
{
    if (m_manager) {
        m_manager->reload();
    }
}

void QmlPluginManagerView::OpenSelected()
{
    if (!m_manager) {
        return;
    }
    const QString id = selectedPluginId();
    if (!id.isEmpty()) {
        m_manager->openPlugin(id);
    }
}

void QmlPluginManagerView::OpenUserFolder()
{
    if (!m_manager) {
        return;
    }
    const QString directory = m_manager->writablePluginDirectory();
    QDir().mkpath(directory);
    QDesktopServices::openUrl(QUrl::fromLocalFile(directory));
}

QString QmlPluginManagerView::selectedPluginId() const
{
    const QList<QTableWidgetItem *> selection = m_plugins->selectedItems();
    return selection.isEmpty()
        ? QString() : selection.constFirst()->data(PluginIdRole).toString();
}
