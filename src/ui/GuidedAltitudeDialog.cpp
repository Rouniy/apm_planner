#include "GuidedAltitudeDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

#include <cmath>
#include <limits>

GuidedAltitudeDialog::GuidedAltitudeDialog(
    double initialAltitudeMetres,
    MAV_FRAME initialFrame,
    double displayMultiplier,
    const QString &displayUnit,
    const QString &frozenTargetDescription,
    QWidget *parent)
    : QDialog(parent),
      m_displayMultiplier(
          std::isfinite(displayMultiplier) && displayMultiplier > 0.0
              ? displayMultiplier : 1.0)
{
    setObjectName(QStringLiteral("GuidedAltitudeDialog"));
    setWindowTitle(tr("Enter Guided Mode Alt"));
    setMinimumWidth(420);

    auto *layout = new QVBoxLayout(this);

    auto *target = new QLabel(this);
    target->setObjectName(QStringLiteral("GuidedAltitudeTarget"));
    target->setTextFormat(Qt::PlainText);
    target->setWordWrap(true);
    target->setText(tr("Exact target: %1").arg(frozenTargetDescription));
    layout->addWidget(target);

    auto *form = new QFormLayout;
    m_altitude = new QLineEdit(this);
    m_altitude->setObjectName(QStringLiteral("GuidedAltitudeValue"));
    if (std::isfinite(initialAltitudeMetres)) {
        const double initialDisplayAltitude =
            initialAltitudeMetres * m_displayMultiplier;
        if (std::isfinite(initialDisplayAltitude)) {
            m_altitude->setText(
                QLocale::c().toString(initialDisplayAltitude, 'g', 15));
        }
    }
    const QString unit = displayUnit.trimmed().isEmpty()
        ? tr("display units") : displayUnit.trimmed();
    form->addRow(tr("Altitude (%1)").arg(unit), m_altitude);

    m_frame = new QComboBox(this);
    m_frame->setObjectName(QStringLiteral("GuidedAltitudeFrame"));
    m_frame->addItem(tr("Relative"),
                     static_cast<int>(MAV_FRAME_GLOBAL_RELATIVE_ALT));
    m_frame->addItem(tr("Absolute"),
                     static_cast<int>(MAV_FRAME_GLOBAL));
    m_frame->addItem(tr("Terrain"),
                     static_cast<int>(MAV_FRAME_GLOBAL_TERRAIN_ALT));
    const MAV_FRAME selectedFrame = isSupportedFrame(initialFrame)
        ? initialFrame : MAV_FRAME_GLOBAL_RELATIVE_ALT;
    const int selectedIndex = m_frame->findData(
        static_cast<int>(selectedFrame));
    m_frame->setCurrentIndex(selectedIndex >= 0 ? selectedIndex : 0);
    form->addRow(tr("Frame"), m_frame);
    layout->addLayout(form);

    m_validation = new QLabel(this);
    m_validation->setObjectName(QStringLiteral("GuidedAltitudeValidation"));
    m_validation->setTextFormat(Qt::PlainText);
    m_validation->setWordWrap(true);
    m_validation->setVisible(false);
    layout->addWidget(m_validation);
    connect(m_altitude, &QLineEdit::textChanged, this, [this]() {
        m_validation->clear();
        m_validation->setVisible(false);
    });

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    auto *acceptButton = buttons->button(QDialogButtonBox::Ok);
    auto *cancelButton = buttons->button(QDialogButtonBox::Cancel);
    acceptButton->setObjectName(QStringLiteral("GuidedAltitudeAcceptButton"));
    cancelButton->setObjectName(QStringLiteral("GuidedAltitudeCancelButton"));
    acceptButton->setText(tr("Apply"));
    acceptButton->setAutoDefault(false);
    acceptButton->setDefault(false);
    cancelButton->setAutoDefault(true);
    cancelButton->setDefault(true);
    connect(buttons, &QDialogButtonBox::accepted,
            this, &GuidedAltitudeDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected,
            this, &GuidedAltitudeDialog::reject);
    layout->addWidget(buttons);
}

double GuidedAltitudeDialog::altitudeMetres() const
{
    return m_hasAcceptedValue
        ? m_acceptedAltitudeMetres
        : std::numeric_limits<double>::quiet_NaN();
}

MAV_FRAME GuidedAltitudeDialog::frame() const
{
    return m_acceptedFrame;
}

bool GuidedAltitudeDialog::hasAcceptedValue() const
{
    return m_hasAcceptedValue;
}

bool GuidedAltitudeDialog::isSupportedFrame(MAV_FRAME frame)
{
    return frame == MAV_FRAME_GLOBAL
        || frame == MAV_FRAME_GLOBAL_RELATIVE_ALT
        || frame == MAV_FRAME_GLOBAL_TERRAIN_ALT;
}

void GuidedAltitudeDialog::accept()
{
    const QPointer<GuidedAltitudeDialog> guard(this);
    const QPointer<QLineEdit> altitudeEdit(m_altitude);
    const QPointer<QLabel> validation(m_validation);
    const QString text = m_altitude->text().trimmed();
    bool ok = false;
    double displayedAltitude = QLocale::c().toDouble(text, &ok);
    if (!ok) {
        displayedAltitude = QLocale().toDouble(text, &ok);
    }
    const double altitude = displayedAltitude / m_displayMultiplier;
    const MAV_FRAME selectedFrame = static_cast<MAV_FRAME>(
        m_frame->currentData().toInt());
    if (!ok || !std::isfinite(displayedAltitude)
        || !std::isfinite(altitude)) {
        validation->setText(tr("Enter a finite altitude value."));
        if (!guard || !validation || !altitudeEdit) {
            return;
        }
        validation->setVisible(true);
        if (!guard || !altitudeEdit) {
            return;
        }
        altitudeEdit->setFocus(Qt::OtherFocusReason);
        if (guard && altitudeEdit) {
            altitudeEdit->selectAll();
        }
        return;
    }
    if (!isSupportedFrame(selectedFrame)) {
        validation->setText(tr("Select a supported altitude frame."));
        if (guard && validation) {
            validation->setVisible(true);
        }
        return;
    }

    m_acceptedAltitudeMetres = altitude;
    m_acceptedFrame = selectedFrame;
    m_hasAcceptedValue = true;
    QDialog::accept();
}

void GuidedAltitudeDialog::reject()
{
    m_hasAcceptedValue = false;
    QDialog::reject();
}
