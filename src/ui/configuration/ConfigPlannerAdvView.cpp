#include "ConfigPlannerAdvView.h"

#include <QAbstractItemView>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QMetaType>
#include <QPushButton>
#include <QSettings>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <QtGlobal>

namespace {
QString displayValue(const QVariant &value)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const int typeId = value.typeId();
#else
    const int typeId = value.userType();
#endif
    if (typeId == QMetaType::QByteArray) {
        return QObject::tr("<binary data: %1 bytes>")
            .arg(value.toByteArray().size());
    }
    if (typeId == QMetaType::QStringList) {
        return value.toStringList().join(QStringLiteral(", "));
    }
    return value.toString();
}
}

ConfigPlannerAdvView::ConfigPlannerAdvView(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("ConfigPlannerAdvView"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(10);

    auto *header = new QHBoxLayout;
    auto *title = new QLabel(tr("Planner Advanced (config.xml)"), this);
    title->setObjectName(QStringLiteral("plannerAdvancedTitle"));
    QFont titleFont = title->font();
    titleFont.setPixelSize(18);
    titleFont.setBold(true);
    title->setFont(titleFont);
    auto *refreshButton = new QPushButton(tr("Refresh"), this);
    refreshButton->setObjectName(QStringLiteral("refreshButton"));
    header->addWidget(title);
    header->addStretch(1);
    header->addWidget(refreshButton);
    layout->addLayout(header);

    m_settingsTable = new QTableWidget(this);
    m_settingsTable->setObjectName(QStringLiteral("settingsTable"));
    m_settingsTable->setColumnCount(2);
    m_settingsTable->setHorizontalHeaderLabels(
        {tr("Name"), tr("Value")});
    m_settingsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_settingsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_settingsTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_settingsTable->setAlternatingRowColors(true);
    m_settingsTable->verticalHeader()->hide();
    m_settingsTable->horizontalHeader()->setStretchLastSection(true);
    m_settingsTable->horizontalHeader()->setSectionResizeMode(
        0, QHeaderView::ResizeToContents);
    layout->addWidget(m_settingsTable, 1);

    connect(refreshButton, &QPushButton::clicked,
            this, &ConfigPlannerAdvView::refresh);
    refresh();
}

void ConfigPlannerAdvView::refresh()
{
    QSettings settings;
    settings.sync();
    const QStringList keys = settings.allKeys();

    m_settingsTable->setSortingEnabled(false);
    m_settingsTable->setRowCount(keys.size());
    for (int row = 0; row < keys.size(); ++row) {
        const QString &key = keys.at(row);
        m_settingsTable->setItem(row, 0, new QTableWidgetItem(key));
        m_settingsTable->setItem(
            row, 1, new QTableWidgetItem(displayValue(settings.value(key))));
    }
    m_settingsTable->setSortingEnabled(true);
    m_settingsTable->sortItems(0, Qt::AscendingOrder);
}
