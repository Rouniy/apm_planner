#include "MainWindowHeader.h"

#include "LinkManager.h"
#include "UASInterface.h"
#include "UASManager.h"

#include <QAction>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

namespace {
const char kHeaderStyle[] = R"(
MainWindowHeader {
    background: #262728;
    color: #ffffff;
}
QScrollArea#mainNavigationScroll,
QScrollArea#mainNavigationScroll QWidget#qt_scrollarea_viewport,
QWidget#mainNavigationHost,
QWidget#connectionPanel {
    background: #262728;
    border: 0;
}
QToolButton[mpNav="true"] {
    background: transparent;
    border: 0;
    color: #e0e0e0;
    font-size: 11px;
    font-weight: 600;
    padding: 4px 10px;
    min-width: 54px;
}
QToolButton[mpNav="true"]:hover {
    background: #3a3b3c;
    color: #ffffff;
}
QToolButton[mpNav="true"]:checked {
    background: #94c11f;
    color: #262728;
}
QToolButton#toolsButton {
    background: transparent;
    border: 0;
    color: #e0e0e0;
    font-size: 11px;
    font-weight: 600;
    padding: 4px 10px;
}
QToolButton#toolsButton:hover { background: #3a3b3c; color: #ffffff; }
QToolButton#ardupilotLink {
    background: transparent;
    border: 0;
    color: #f2c200;
    font-size: 26px;
    font-weight: bold;
    font-style: italic;
    padding-left: 16px;
    padding-right: 16px;
}
QComboBox, QSpinBox {
    min-height: 26px;
    max-height: 26px;
    background: #2d2d2d;
    color: #ffffff;
    border: 1px solid #1f1f20;
    border-radius: 3px;
    padding: 0 6px;
}
QPushButton, QCheckBox {
    min-height: 26px;
    max-height: 26px;
    color: #ffffff;
}
QPushButton#connectButton {
    min-width: 110px;
    border: 1px solid #94c11f;
    border-radius: 3px;
    color: #405704;
    font-weight: 600;
    background: #94c11f;
}
QPushButton#connectButton:hover { background: #a6d62b; }
QLabel#connectionStatus { color: #cfcfcf; font-size: 10px; }
QProgressBar {
    min-width: 120px;
    max-width: 120px;
    min-height: 4px;
    max-height: 4px;
    border: 0;
    background: #1a1a1b;
}
QProgressBar::chunk { background: #94c11f; }
)";
}

MainWindowHeader::MainWindowHeader(QWidget *parent)
    : QWidget(parent),
      m_navigationGroup(new QButtonGroup(this))
{
    setObjectName(QStringLiteral("mainWindowHeader"));
    setAttribute(Qt::WA_StyledBackground, true);
    setFixedHeight(64);
    setStyleSheet(QString::fromLatin1(kHeaderStyle));
    m_navigationGroup->setExclusive(true);

    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto *scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("mainNavigationScroll"));
    scroll->setFrameShape(QFrame::NoFrame);
    // A native scrollbar steals 16-20 px from the 64 px Mission Planner header
    // and clips the icon/text buttons. Navigation still compresses through its
    // scroll-area viewport; explicit overflow controls will be added with the
    // final profile-dependent navigation registry.
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidgetResizable(true);
    m_navigationHost = new QWidget(scroll);
    m_navigationHost->setObjectName(QStringLiteral("mainNavigationHost"));
    auto *navigationLayout = new QHBoxLayout(m_navigationHost);
    navigationLayout->setContentsMargins(0, 0, 0, 0);
    navigationLayout->setSpacing(0);
    navigationLayout->addStretch();
    scroll->setWidget(m_navigationHost);
    root->addWidget(scroll, 1);

    m_toolsButton = new QToolButton(this);
    m_toolsButton->setObjectName(QStringLiteral("toolsButton"));
    m_toolsButton->setText(tr("TOOLS"));
    m_toolsButton->setPopupMode(QToolButton::InstantPopup);
    m_toolsButton->setFixedHeight(64);
    root->addWidget(m_toolsButton);

    auto *ardupilot = new QToolButton(this);
    ardupilot->setText(tr("ARDUPILOT"));
    ardupilot->setObjectName(QStringLiteral("ardupilotLink"));
    ardupilot->setCursor(Qt::PointingHandCursor);
    ardupilot->setToolTip(QStringLiteral("https://ardupilot.org"));
    connect(ardupilot, &QToolButton::clicked, this, []() {
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://ardupilot.org")));
    });
    root->addWidget(ardupilot);

    auto *connection = new QWidget(this);
    connection->setObjectName(QStringLiteral("connectionPanel"));
    auto *connectionRows = new QVBoxLayout(connection);
    connectionRows->setContentsMargins(0, 3, 6, 3);
    connectionRows->setSpacing(2);
    auto *top = new QHBoxLayout;
    top->setContentsMargins(0, 0, 0, 0);
    top->setSpacing(6);

    m_portCombo = new QComboBox(connection);
    m_portCombo->setObjectName(QStringLiteral("portCombo"));
    m_portCombo->setFixedWidth(180);
    top->addWidget(m_portCombo);
    m_baudSpin = new QSpinBox(connection);
    m_baudSpin->setObjectName(QStringLiteral("baudSpin"));
    m_baudSpin->setRange(1200, 4000000);
    m_baudSpin->setFixedWidth(140);
    top->addWidget(m_baudSpin);
    m_vehicleCombo = new QComboBox(connection);
    m_vehicleCombo->setObjectName(QStringLiteral("vehicleCombo"));
    m_vehicleCombo->setFixedWidth(170);
    m_vehicleCombo->hide();
    top->addWidget(m_vehicleCombo);
    auto *automatic = new QCheckBox(tr("Auto"), connection);
    automatic->setChecked(true);
    top->addWidget(automatic);
    auto *refresh = new QToolButton(connection);
    refresh->setObjectName(QStringLiteral("refreshLinksButton"));
    refresh->setText(QStringLiteral("⟳"));
    refresh->setFixedSize(28, 28);
    top->addWidget(refresh);
    m_connectButton = new QPushButton(tr("CONNECT"), connection);
    m_connectButton->setObjectName(QStringLiteral("connectButton"));
    top->addWidget(m_connectButton);
    connectionRows->addLayout(top);

    auto *bottom = new QHBoxLayout;
    bottom->setContentsMargins(0, 0, 0, 0);
    bottom->setSpacing(6);
    m_connectionStatus = new QLabel(tr("No connection"), connection);
    m_connectionStatus->setObjectName(QStringLiteral("connectionStatus"));
    bottom->addWidget(m_connectionStatus, 1);
    m_connectionProgress = new QProgressBar(connection);
    m_connectionProgress->setRange(0, 100);
    m_connectionProgress->setValue(0);
    m_connectionProgress->setTextVisible(false);
    bottom->addWidget(m_connectionProgress);
    connectionRows->addLayout(bottom);
    root->addWidget(connection);

    connect(refresh, &QToolButton::clicked, this, &MainWindowHeader::refreshLinks);
    connect(m_portCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindowHeader::updateCurrentLink);
    connect(m_connectButton, &QPushButton::clicked,
            this, &MainWindowHeader::toggleConnection);
    connect(LinkManager::instance(), SIGNAL(newLink(int)), this, SLOT(refreshLinks()));
    connect(LinkManager::instance(), SIGNAL(linkChanged(int)), this, SLOT(updateCurrentLink()));
    connect(UASManager::instance(), SIGNAL(UASCreated(UASInterface*)), this, SLOT(rebuildVehicleList()));
    connect(UASManager::instance(), SIGNAL(UASDeleted(UASInterface*)), this, SLOT(rebuildVehicleList()));
    connect(UASManager::instance(), SIGNAL(activeUASSet(UASInterface*)),
            this, SLOT(activeVehicleChanged(UASInterface*)));
    connect(m_vehicleCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int index) {
                if (index < 0) {
                    return;
                }
                UASInterface *uas = UASManager::instance()->getUASForId(
                    m_vehicleCombo->itemData(index).toInt());
                if (uas) {
                    UASManager::instance()->setActiveUAS(uas);
                }
            });

    refreshLinks();
    rebuildVehicleList();
}

void MainWindowHeader::setNavigationActions(QAction *data,
                                                QAction *plan,
                                                QAction *setup,
                                                QAction *config,
                                                QAction *simulation,
                                                QAction *help)
{
    auto *layout = qobject_cast<QHBoxLayout *>(m_navigationHost->layout());
    while (layout->count() > 0) {
        QLayoutItem *item = layout->takeAt(0);
        if (item->widget()) {
            item->widget()->deleteLater();
        }
        delete item;
    }
    addNavigationButton(tr("DATA"), QStringLiteral("navData"), data)->setChecked(true);
    addNavigationButton(tr("PLAN"), QStringLiteral("navPlan"), plan);
    addNavigationButton(tr("SETUP"), QStringLiteral("navSetup"), setup);
    addNavigationButton(tr("CONFIG"), QStringLiteral("navConfig"), config);
    addNavigationButton(tr("SIMULATION"), QStringLiteral("navSimulation"), simulation);
    addNavigationButton(tr("HELP"), QStringLiteral("navHelp"), help);
    layout->addStretch();
}

QToolButton *MainWindowHeader::addNavigationButton(const QString &name,
                                                       const QString &objectName,
                                                       QAction *action)
{
    auto *button = new QToolButton(m_navigationHost);
    button->setObjectName(objectName);
    button->setProperty("mpNav", true);
    button->setText(name);
    button->setCheckable(true);
    button->setFixedHeight(64);
    if (action) {
        button->setIcon(action->icon());
        button->setIconSize(QSize(20, 20));
        button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        connect(button, &QToolButton::clicked, action, &QAction::trigger);
        connect(action, &QAction::triggered, button, [button]() { button->setChecked(true); });
        button->setVisible(action->isVisible());
        button->setEnabled(action->isEnabled());
        connect(action, &QAction::changed, button, [button, action]() {
            button->setVisible(action->isVisible());
            button->setEnabled(action->isEnabled());
        });
    }
    m_navigationGroup->addButton(button);
    qobject_cast<QHBoxLayout *>(m_navigationHost->layout())->addWidget(button);
    return button;
}

void MainWindowHeader::setToolsMenu(QMenu *menu)
{
    m_toolsButton->setMenu(menu);
}

void MainWindowHeader::disableConnectWidget(bool disable)
{
    if (!m_disableOverride) {
        m_portCombo->setDisabled(disable);
        m_baudSpin->setDisabled(disable);
        m_connectButton->setDisabled(disable);
    }
}

void MainWindowHeader::overrideDisableConnectWidget(bool disable)
{
    m_disableOverride = disable;
}

void MainWindowHeader::startAnimation()
{
    m_connectionProgress->setRange(0, 0);
}

void MainWindowHeader::stopAnimation()
{
    m_connectionProgress->setRange(0, 100);
    m_connectionProgress->setValue(0);
}

void MainWindowHeader::refreshLinks()
{
    const int selectedId = currentLinkId();
    m_portCombo->blockSignals(true);
    m_portCombo->clear();
    const QList<int> links = LinkManager::instance()->getLinks();
    for (int linkId : links) {
        const QString label = LinkManager::instance()->getLinkShortName(linkId)
            + QStringLiteral(" · ") + LinkManager::instance()->getLinkDetail(linkId);
        m_portCombo->addItem(label, linkId);
    }
    const int restoredIndex = m_portCombo->findData(selectedId);
    if (restoredIndex >= 0) {
        m_portCombo->setCurrentIndex(restoredIndex);
    }
    m_portCombo->blockSignals(false);
    updateCurrentLink();
}

int MainWindowHeader::currentLinkId() const
{
    return m_portCombo->currentIndex() >= 0 ? m_portCombo->currentData().toInt() : -1;
}

void MainWindowHeader::updateCurrentLink()
{
    const int linkId = currentLinkId();
    if (linkId < 0) {
        m_baudSpin->setValue(115200);
        m_connectButton->setText(tr("CONNECT"));
        m_connectButton->setEnabled(false);
        m_connectionStatus->setText(tr("No connection configured"));
        return;
    }

    const int baud = LinkManager::instance()->getSerialLinkBaud(linkId);
    if (baud > 0) {
        m_baudSpin->setValue(baud);
    }
    const bool connected = LinkManager::instance()->getLinkConnected(linkId);
    m_connectButton->setEnabled(true);
    m_connectButton->setText(connected ? tr("DISCONNECT") : tr("CONNECT"));
    m_connectionStatus->setText(connected
        ? tr("Connected · %1").arg(LinkManager::instance()->getLinkDetail(linkId))
        : tr("Disconnected · %1").arg(LinkManager::instance()->getLinkDetail(linkId)));
}

void MainWindowHeader::toggleConnection()
{
    const int linkId = currentLinkId();
    if (linkId < 0) {
        return;
    }
    if (LinkManager::instance()->getLinkConnected(linkId)) {
        LinkManager::instance()->disconnectLink(linkId);
    } else {
        LinkManager::instance()->connectLink(linkId);
    }
    updateCurrentLink();
}

void MainWindowHeader::rebuildVehicleList()
{
    const int activeId = UASManager::instance()->getActiveUAS()
        ? UASManager::instance()->getActiveUAS()->getUASID() : -1;
    m_vehicleCombo->blockSignals(true);
    m_vehicleCombo->clear();
    const QList<UASInterface *> vehicles = UASManager::instance()->getUASList();
    for (UASInterface *vehicle : vehicles) {
        m_vehicleCombo->addItem(
            tr("Vehicle %1 · %2").arg(vehicle->getUASID()).arg(vehicle->getUASName()),
            vehicle->getUASID());
    }
    const int activeIndex = m_vehicleCombo->findData(activeId);
    if (activeIndex >= 0) {
        m_vehicleCombo->setCurrentIndex(activeIndex);
    }
    m_vehicleCombo->setVisible(vehicles.size() > 1);
    m_vehicleCombo->blockSignals(false);
}

void MainWindowHeader::activeVehicleChanged(UASInterface *uas)
{
    rebuildVehicleList();
    if (uas) {
        m_connectionStatus->setText(tr("Vehicle %1 · %2")
                                    .arg(uas->getUASID()).arg(uas->getUASName()));
    }
}
