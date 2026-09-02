#include "ProximityWindow.h"

#include "ProximityRadarControl.h"
#include "Proximity.h"
#include "UASInterface.h"

#include <QVBoxLayout>

ProximityWindow::ProximityWindow(QWidget *parent)
    : QWidget(parent, Qt::Window),
      m_radar(new ProximityRadarControl(this))
{
    setObjectName(QStringLiteral("ProximityWindow"));
    setWindowTitle(tr("Proximity"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(620, 620);
    setMinimumSize(420, 420);
    setStyleSheet(QStringLiteral(
        "ProximityWindow { background-color: #151817; }"));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_radar);
}

ProximityWindow::~ProximityWindow()
{
    if (m_uas) {
        disconnect(m_uas, nullptr, this, nullptr);
    }
}

ProximityWindow *ProximityWindow::OpenWindow(QWidget *owner,
                                              UASInterface *uas)
{
    auto *window = new ProximityWindow(owner);
    window->setActiveUAS(uas);
    if (owner && owner->window()) {
        const QPoint center = owner->window()->frameGeometry().center();
        window->move(center - window->rect().center());
    }
    window->show();
    window->raise();
    window->activateWindow();
    return window;
}

ProximityRadarControl *ProximityWindow::Radar() const
{
    return m_radar;
}

Proximity *ProximityWindow::ProximityState() const
{
    return m_radar->ProximityState();
}

UASInterface *ProximityWindow::activeUAS() const
{
    return m_uas.data();
}

void ProximityWindow::setActiveUAS(UASInterface *uas)
{
    if (m_uas == uas) {
        return;
    }
    if (m_uas) {
        disconnect(m_uas, nullptr, this, nullptr);
    }
    m_uas = uas;
    m_radar->setProximity(m_uas ? m_uas->getProximity() : nullptr);
    if (!m_uas) {
        return;
    }
    connect(m_uas, &QObject::destroyed, this, [this]() {
        m_uas = nullptr;
        m_radar->setProximity(nullptr);
    });
}
