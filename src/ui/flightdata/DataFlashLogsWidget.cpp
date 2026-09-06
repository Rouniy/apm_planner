#include "DataFlashLogsWidget.h"

#include <QGridLayout>
#include <QLabel>
#include <QPushButton>

namespace
{
QPushButton *makeButton(QWidget *parent, const char *name,
                        const QString &text)
{
    auto *button = new QPushButton(text, parent);
    button->setObjectName(QString::fromLatin1(name));
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    button->setMinimumWidth(0);
    return button;
}
} // namespace

DataFlashLogsWidget::DataFlashLogsWidget(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("DataFlashLogsWidget"));

    auto *layout = new QGridLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setHorizontalSpacing(4);
    layout->setVerticalSpacing(4);
    layout->setColumnStretch(0, 1);
    layout->setColumnStretch(1, 1);
    layout->setColumnStretch(2, 1);

    m_download = makeButton(this, "DataFlashDownloadButton",
                            tr("Download DataFlash Log\nVia Mavlink"));
    m_review = makeButton(this, "DataFlashReviewButton", tr("Review a Log"));
    m_analysis = makeButton(this, "DataFlashAutoAnalysisButton", tr("Auto Analysis"));
    m_kmlGpx = makeButton(this, "DataFlashKmlGpxButton", tr("Create KML + gpx"));
    m_binToLog = makeButton(this, "DataFlashBinToLogButton", tr("Convert Bin to Log"));
    m_matlab = makeButton(this, "DataFlashMatlabButton", tr("Create Matlab File"));
    m_geoReference = makeButton(this, "DataFlashGeoReferenceButton",
                                tr("Geo Reference Images"));
    m_organize = makeButton(this, "DataFlashOrganizeButton",
                            tr("Organize tlog/rlog/\nbin/log"));

    const QString unavailable = tr("This Mission Planner workflow is not yet ported.");
    m_matlab->setEnabled(false);
    m_matlab->setToolTip(unavailable);
    m_geoReference->setEnabled(false);
    m_geoReference->setToolTip(unavailable);

    layout->addWidget(m_download, 0, 0);
    layout->addWidget(m_review, 0, 1);
    layout->addWidget(m_analysis, 0, 2);
    layout->addWidget(m_kmlGpx, 1, 0);
    layout->addWidget(m_binToLog, 1, 1);
    layout->addWidget(m_matlab, 1, 2);
    layout->addWidget(m_geoReference, 2, 0);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("DataFlashLogStatus"));
    m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(true);
    m_status->setStyleSheet(QStringLiteral("color: #c8a03c;"));
    layout->addWidget(m_status, 2, 1, 1, 2);

    layout->addWidget(m_organize, 3, 0);
    auto *organizeDescription = new QLabel(
        tr("Sort local logs by vehicle type, system id and board serial number."), this);
    organizeDescription->setObjectName(QStringLiteral("DataFlashOrganizeDescription"));
    organizeDescription->setTextFormat(Qt::PlainText);
    organizeDescription->setWordWrap(true);
    organizeDescription->setStyleSheet(QStringLiteral("color: #999999;"));
    layout->addWidget(organizeDescription, 3, 1, 1, 2);
    layout->setRowStretch(4, 1);

    connect(m_download, &QPushButton::clicked,
            this, &DataFlashLogsWidget::downloadRequested);
    connect(m_review, &QPushButton::clicked,
            this, &DataFlashLogsWidget::reviewRequested);
    connect(m_analysis, &QPushButton::clicked,
            this, &DataFlashLogsWidget::autoAnalysisRequested);
    connect(m_kmlGpx, &QPushButton::clicked,
            this, &DataFlashLogsWidget::kmlGpxRequested);
    connect(m_binToLog, &QPushButton::clicked,
            this, &DataFlashLogsWidget::binToLogRequested);
    connect(m_organize, &QPushButton::clicked,
            this, &DataFlashLogsWidget::organizeRequested);
}

void DataFlashLogsWidget::setOperationBusy(bool busy)
{
    m_download->setEnabled(!busy);
    m_review->setEnabled(!busy);
    m_analysis->setEnabled(!busy);
    m_kmlGpx->setEnabled(!busy);
    m_binToLog->setEnabled(!busy);
    m_organize->setEnabled(!busy);
    // These two are intentionally unavailable even while idle.
    m_matlab->setEnabled(false);
    m_geoReference->setEnabled(false);
}

void DataFlashLogsWidget::setStatusText(const QString &text)
{
    m_status->setText(text);
}

QString DataFlashLogsWidget::statusText() const
{
    return m_status->text();
}
