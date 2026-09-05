#include "MainWindowHeader.h"

#include "LinkManager.h"
#include "ParameterService.h"
#include "SerialLinkInterface.h"
#include "UASInterface.h"
#include "UASManager.h"
#include "VehicleEndpoint.h"
#include "VehicleTargetManager.h"
#include "core/parameters/ParameterStore.h"

#include <QAction>
#include <QApplication>
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
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

namespace {
constexpr int LinkIdRole = Qt::UserRole + 1;
constexpr int SystemIdRole = Qt::UserRole + 2;
constexpr int ComponentIdRole = Qt::UserRole + 3;

const char kHeaderStyle[] = R"(
MainWindowHeader {
    background: #121614;
    color: #ffffff;
    font-family: "Bitstream Vera Sans";
}
QScrollArea#mainNavigationScroll,
QScrollArea#mainNavigationScroll QWidget#qt_scrollarea_viewport,
QWidget#mainNavigationHost,
QWidget#connectionPanel {
    background: #121614;
    border: 0;
}
QToolButton[mpNav="true"] {
    background: transparent;
    border: 0;
    color: #e0e0e0;
    font-size: 11px;
    font-weight: bold;
    padding: 4px 10px;
    min-height: 64px;
    max-height: 64px;
}
QToolButton[mpNav="true"]:hover {
    background: #202623;
    color: #ffffff;
}
QToolButton[mpNav="true"]:checked {
    background: #34d399;
    color: #0d1210;
}
QToolButton#MenuFlightData,
QToolButton#MenuFlightPlanner {
    min-width: 30px;
    max-width: 30px;
}
QToolButton#MenuInitConfig {
    min-width: 40px;
    max-width: 40px;
}
QToolButton#MenuConfigTune {
    min-width: 44px;
    max-width: 44px;
}
QToolButton#MenuSimulation {
    min-width: 64px;
    max-width: 64px;
}
QToolButton#MenuHelp {
    min-width: 30px;
    max-width: 30px;
}
QToolButton#toolsButton {
    background: transparent;
    border: 0;
    color: #e0e0e0;
    font-size: 11px;
    font-weight: bold;
    padding: 4px 10px;
    min-height: 64px;
    max-height: 64px;
    min-width: 44px;
    max-width: 44px;
}
QToolButton#toolsButton:hover { background: #202623; color: #ffffff; }
QToolButton#toolsButton::menu-indicator { image: none; width: 0; }
QToolButton#ardupilotLink {
    background: transparent;
    border: 0;
    color: #f2c200;
    font-size: 26px;
    font-weight: bold;
    font-style: italic;
    padding-left: 16px;
    padding-right: 16px;
    min-height: 64px;
    max-height: 64px;
    min-width: 150px;
    max-width: 150px;
}
QToolButton#refreshLinksButton,
QToolButton#addConnectionButton {
    min-width: 28px;
    max-width: 28px;
    min-height: 28px;
    max-height: 28px;
    padding: 0;
    border: 0;
    border-radius: 3px;
    background: #34d399;
    color: #06251a;
}
QToolButton#refreshLinksButton:hover,
QToolButton#addConnectionButton:hover { background: #10b981; color: #06251a; }
QComboBox, QSpinBox {
    min-height: 28px;
    max-height: 28px;
    background: #161b18;
    color: #ffffff;
    border: 1px solid #2a322d;
    border-radius: 3px;
    padding: 0 6px;
}
QPushButton, QCheckBox {
    min-height: 28px;
    max-height: 28px;
    color: #ffffff;
}
QCheckBox#autoConnectCheckBox {
    background: transparent;
    color: #d8d8d8;
    spacing: 5px;
}
QCheckBox#autoConnectCheckBox::indicator {
    width: 18px;
    height: 18px;
    border: 1px solid #68716c;
    border-radius: 3px;
    background: #0d1210;
    image: none;
}
QCheckBox#autoConnectCheckBox::indicator:checked {
    border-color: #34d399;
    background: #34d399;
}
QPushButton#connectButton {
    min-width: 110px;
    border: 1px solid #34d399;
    border-radius: 3px;
    color: #06251a;
    font-weight: 600;
    background: #34d399;
}
QPushButton#connectButton:hover { background: #6ee7b7; }
QLabel#connectionStatus {
    color: #cfcfcf;
    background: transparent;
    border: 0;
    font-size: 10px;
}
QProgressBar {
    min-width: 120px;
    max-width: 120px;
    min-height: 4px;
    max-height: 4px;
    border: 0;
    background: #0d1210;
}
QProgressBar::chunk { background: #34d399; }
QScrollBar:horizontal {
    height: 3px;
    margin: 0;
    border: 0;
    background: transparent;
}
QScrollBar::handle:horizontal {
    min-width: 24px;
    border: 0;
    border-radius: 0;
    background: #34d399;
}
QScrollBar::add-line:horizontal,
QScrollBar::sub-line:horizontal { width: 0; border: 0; }
)";

QString navigationIcon(const QString &objectName)
{
    if (objectName == QStringLiteral("MenuFlightData")) {
        return QStringLiteral(":/files/images/missionplanner10/chart-line.svg");
    }
    if (objectName == QStringLiteral("MenuFlightPlanner")) {
        return QStringLiteral(":/files/images/missionplanner10/map-location-dot.svg");
    }
    if (objectName == QStringLiteral("MenuInitConfig")) {
        return QStringLiteral(":/files/images/missionplanner10/screwdriver-wrench.svg");
    }
    if (objectName == QStringLiteral("MenuConfigTune")) {
        return QStringLiteral(":/files/images/missionplanner10/sliders.svg");
    }
    if (objectName == QStringLiteral("MenuSimulation")) {
        return QStringLiteral(":/files/images/missionplanner10/gamepad.svg");
    }
    if (objectName == QStringLiteral("MenuHelp")) {
        return QStringLiteral(":/files/images/missionplanner10/circle-question.svg");
    }
    return QString();
}

int navigationButtonWidth(const QString &objectName)
{
    if (objectName == QStringLiteral("MenuFlightData")
        || objectName == QStringLiteral("MenuFlightPlanner")
        || objectName == QStringLiteral("MenuHelp")) {
        return 50;
    }
    if (objectName == QStringLiteral("MenuInitConfig")) {
        return 60;
    }
    if (objectName == QStringLiteral("MenuConfigTune")) {
        return 64;
    }
    if (objectName == QStringLiteral("MenuSimulation")) {
        return 84;
    }
    return 54;
}
}

MainWindowHeader::MainWindowHeader(QWidget *parent)
    : QWidget(parent),
      m_navigationGroup(new QButtonGroup(this))
{
    setObjectName(QStringLiteral("mainWindowHeader"));
    setAttribute(Qt::WA_StyledBackground, true);
    setFixedHeight(headerHeightFor(false, false));
    setStyleSheet(QString::fromLatin1(kHeaderStyle));
    m_navigationGroup->setExclusive(true);

    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto *scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("mainNavigationScroll"));
    scroll->setFrameShape(QFrame::NoFrame);
    // MP10 uses an automatic horizontal overflow strip. A local 3 px style
    // avoids the legacy application's 14 px scrollbar stealing header height.
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
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
    m_toolsButton->setFixedWidth(64);
    root->addWidget(m_toolsButton);

    auto *ardupilot = new QToolButton(this);
    ardupilot->setText(tr("ARDUPILOT"));
    ardupilot->setObjectName(QStringLiteral("ardupilotLink"));
    ardupilot->setFixedWidth(182);
    ardupilot->setCursor(Qt::PointingHandCursor);
    ardupilot->setToolTip(QStringLiteral("https://ardupilot.org"));
    connect(ardupilot, &QToolButton::clicked, this, []() {
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://ardupilot.org")));
    });
    root->addWidget(ardupilot);

    auto *connection = new QWidget(this);
    m_connectionPanel = connection;
    connection->setObjectName(QStringLiteral("connectionPanel"));
    auto *connectionRows = new QVBoxLayout(connection);
    connectionRows->setContentsMargins(8, 3, 12, 3);
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
    m_baudSpin->setRange(1, 10000000);
    m_baudSpin->setFixedWidth(140);
    top->addWidget(m_baudSpin);
    m_targetCombo = new QComboBox(connection);
    m_targetCombo->setObjectName(QStringLiteral("cmb_sysid"));
    m_targetCombo->setFixedWidth(170);
    m_targetCombo->setEnabled(false);
    m_autoConnectCheckBox = new QCheckBox(tr("Auto"), connection);
    m_autoConnectCheckBox->setObjectName(QStringLiteral("autoConnectCheckBox"));
    m_autoConnectCheckBox->setChecked(QSettings().value(
        QStringLiteral("autoconnect"), false).toBool());
    m_autoConnectCheckBox->setFixedWidth(54);
    top->addWidget(m_autoConnectCheckBox);
    auto *refresh = new QToolButton(connection);
    refresh->setObjectName(QStringLiteral("refreshLinksButton"));
    refresh->setText(QStringLiteral("⟳"));
    refresh->setFixedSize(28, 28);
    top->addWidget(refresh);
    m_addConnectionButton = new QToolButton(connection);
    m_addConnectionButton->setObjectName(QStringLiteral("addConnectionButton"));
    m_addConnectionButton->setText(QStringLiteral("+"));
    m_addConnectionButton->setToolTip(tr("Add Connection…"));
    m_addConnectionButton->setFixedSize(28, 28);
    m_addConnectionButton->setEnabled(false);
    top->addWidget(m_addConnectionButton);
    m_connectButton = new QPushButton(tr("CONNECT"), connection);
    m_connectButton->setObjectName(QStringLiteral("connectButton"));
    m_connectButton->setFixedWidth(110);
    top->addWidget(m_connectButton);
    connectionRows->addLayout(top);

    auto *bottom = new QHBoxLayout;
    bottom->setContentsMargins(0, 0, 0, 0);
    bottom->setSpacing(6);
    bottom->addWidget(m_targetCombo);
    m_connectionStatus = new QLabel(tr("No connection"), connection);
    m_connectionStatus->setObjectName(QStringLiteral("connectionStatus"));
    bottom->addWidget(m_connectionStatus, 1);
    m_connectionProgress = new QProgressBar(connection);
    m_connectionProgress->setObjectName(QStringLiteral("connectionProgress"));
    m_connectionProgress->setRange(0, 100);
    m_connectionProgress->setValue(0);
    m_connectionProgress->setTextVisible(false);
    bottom->addWidget(m_connectionProgress);
    connectionRows->addLayout(bottom);
    root->addWidget(connection);

    setContextMenuPolicy(Qt::CustomContextMenu);
    connect(this, &QWidget::customContextMenuRequested, this,
            [this](const QPoint &position) {
                QMenu menu(this);
                menu.addAction(m_autoHideAction);
                QAction *fullScreen = menu.addAction(tr("Full Screen"));
                connect(fullScreen, &QAction::triggered,
                        this, &MainWindowHeader::fullScreenRequested);
                if (m_connectionOptionsAction) {
                    menu.addSeparator();
                    menu.addAction(m_connectionOptionsAction);
                }
                if (m_addConnectionAction) {
                    menu.addAction(m_addConnectionAction);
                }
                menu.exec(mapToGlobal(position));
            });

    m_autoHideAction = new QAction(tr("Auto Hide Menu"), this);
    m_autoHideAction->setObjectName(QStringLiteral("actionMenuAutoHide"));
    m_autoHideAction->setCheckable(true);
    connect(m_autoHideAction, &QAction::toggled,
            this, &MainWindowHeader::setAutoHideEnabled);
    const bool savedAutoHide = QSettings().value(
        QStringLiteral("menu_autohide"), false).toBool();
    m_autoHideAction->setChecked(savedAutoHide);

    connect(refresh, &QToolButton::clicked, this, &MainWindowHeader::refreshLinks);
    connect(m_addConnectionButton, &QToolButton::clicked, this, [this]() {
        if (m_addConnectionAction) {
            m_addConnectionAction->trigger();
        }
    });
    connect(m_portCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindowHeader::updateCurrentLink);
    connect(m_baudSpin, &QSpinBox::editingFinished,
            this, &MainWindowHeader::applyBaudRate);
    connect(m_connectButton, &QPushButton::clicked,
            this, &MainWindowHeader::toggleConnection);
    connect(m_autoConnectCheckBox, &QCheckBox::toggled, this,
            [](bool enabled) {
                QSettings().setValue(QStringLiteral("autoconnect"), enabled);
            });
    connect(LinkManager::instance(), SIGNAL(newLink(int)), this, SLOT(refreshLinks()));
    connect(LinkManager::instance(), SIGNAL(linkChanged(int)), this, SLOT(updateCurrentLink()));
    connect(UASManager::instance(), SIGNAL(activeUASSet(UASInterface*)),
            this, SLOT(activeVehicleChanged(UASInterface*)));
    VehicleTargetManager *const targets =
        LinkManager::instance()->vehicleTargetManager();
    connect(targets, &VehicleTargetManager::endpointsChanged,
            this, &MainWindowHeader::rebuildTargetList);
    connect(targets, &VehicleTargetManager::currentTargetChanged,
            this, &MainWindowHeader::syncCurrentTarget);
    connect(m_targetCombo, QOverload<int>::of(&QComboBox::activated),
            this, &MainWindowHeader::selectTarget);

    refreshLinks();
    rebuildTargetList();
    if (m_autoConnectCheckBox->isChecked()) {
        QTimer::singleShot(0, this, [this]() {
            const int linkId = currentLinkId();
            if (linkId >= 0 && !LinkManager::instance()->getLinkConnected(linkId)) {
                LinkManager::instance()->connectLink(linkId);
                updateCurrentLink();
            }
        });
    }
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
    addNavigationButton(tr("DATA"), QStringLiteral("MenuFlightData"), data)->setChecked(true);
    addNavigationButton(tr("PLAN"), QStringLiteral("MenuFlightPlanner"), plan);
    addNavigationButton(tr("SETUP"), QStringLiteral("MenuInitConfig"), setup);
    addNavigationButton(tr("CONFIG"), QStringLiteral("MenuConfigTune"), config);
    addNavigationButton(tr("SIMULATION"), QStringLiteral("MenuSimulation"), simulation);
    addNavigationButton(tr("HELP"), QStringLiteral("MenuHelp"), help);
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
    button->setFixedWidth(navigationButtonWidth(objectName));
    if (action) {
        const QString iconPath = navigationIcon(objectName);
        button->setIcon(iconPath.isEmpty() ? action->icon() : QIcon(iconPath));
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

bool MainWindowHeader::autoHideEnabled() const
{
    return m_autoHideEnabled;
}

void MainWindowHeader::setAutoHideEnabled(bool enabled)
{
    if (m_autoHideEnabled == enabled) {
        updateHeaderHeight();
        return;
    }
    m_autoHideEnabled = enabled;
    if (m_autoHideAction && m_autoHideAction->isChecked() != enabled) {
        m_autoHideAction->setChecked(enabled);
    }
    QSettings().setValue(QStringLiteral("menu_autohide"), enabled);
    updateHeaderHeight();
    emit autoHideEnabledChanged(enabled);
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
void MainWindowHeader::enterEvent(QEnterEvent *event)
#else
void MainWindowHeader::enterEvent(QEvent *event)
#endif
{
    m_headerHovered = true;
    updateHeaderHeight();
    QWidget::enterEvent(event);
}

void MainWindowHeader::leaveEvent(QEvent *event)
{
    m_headerHovered = false;
    updateHeaderHeight();
    QWidget::leaveEvent(event);
}

void MainWindowHeader::updateHeaderHeight()
{
    setFixedHeight(headerHeightFor(m_autoHideEnabled, m_headerHovered));
}

void MainWindowHeader::setToolsMenu(QMenu *menu)
{
    m_toolsButton->setMenu(menu);
}

void MainWindowHeader::setConnectionOptionsAction(QAction *action)
{
    m_connectionOptionsAction = action;
}

void MainWindowHeader::setAddConnectionAction(QAction *action)
{
    m_addConnectionAction = action;
    m_addConnectionButton->setEnabled(action && action->isEnabled());
    if (!action) {
        return;
    }
    connect(action, &QAction::changed, m_addConnectionButton,
            [this, action]() {
                m_addConnectionButton->setEnabled(action->isEnabled());
            });
}

void MainWindowHeader::setDefaultBaudRate(int baud)
{
    if (currentLinkId() < 0 && baud > 0) {
        m_baudSpin->setValue(baud);
    }
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
        const int configuredBaud = QSettings().value(
            QStringLiteral("baudrate"), 115200).toInt();
        m_baudSpin->setValue(configuredBaud > 0
            ? configuredBaud
            : 115200);
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

void MainWindowHeader::applyBaudRate()
{
    const int linkId = currentLinkId();
    auto *serial = qobject_cast<SerialLinkInterface *>(
        LinkManager::instance()->getLink(linkId));
    if (!serial) {
        return;
    }
    if (serial->isConnected()) {
        m_baudSpin->setValue(serial->getBaudRate());
        return;
    }
    serial->setBaudRate(m_baudSpin->value());
    updateCurrentLink();
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

void MainWindowHeader::rebuildTargetList()
{
    const VehicleTargetLease current =
        LinkManager::instance()->vehicleTargetManager()->acquireTarget();
    const QSignalBlocker blocker(m_targetCombo);
    m_targetCombo->clear();
    const QList<VehicleEndpoint> endpoints =
        LinkManager::instance()->vehicleTargetManager()->endpoints();
    int selectedIndex = -1;
    for (const VehicleEndpoint &endpoint : endpoints) {
        if (endpoint.componentId == MAV_COMP_ID_MISSIONPLANNER) {
            continue;
        }
        const int index = m_targetCombo->count();
        m_targetCombo->addItem(endpoint.displayName());
        m_targetCombo->setItemData(index, endpoint.linkId, LinkIdRole);
        m_targetCombo->setItemData(index, endpoint.systemId, SystemIdRole);
        m_targetCombo->setItemData(
            index, endpoint.componentId, ComponentIdRole);
        if (current.isValid()
            && current.endpoint.sameIdentity(endpoint)) {
            selectedIndex = index;
        }
    }
    m_targetCombo->setCurrentIndex(selectedIndex);
    m_targetCombo->setEnabled(m_targetCombo->count() > 0);
    m_connectionPanel->setFixedWidth(556);
}

void MainWindowHeader::syncCurrentTarget()
{
    const VehicleTargetLease current =
        LinkManager::instance()->vehicleTargetManager()->acquireTarget();
    const QSignalBlocker blocker(m_targetCombo);
    int selectedIndex = -1;
    for (int index = 0; index < m_targetCombo->count(); ++index) {
        if (current.isValid()
            && m_targetCombo->itemData(index, LinkIdRole).toInt()
                == current.endpoint.linkId
            && m_targetCombo->itemData(index, SystemIdRole).toInt()
                == current.endpoint.systemId
            && m_targetCombo->itemData(index, ComponentIdRole).toInt()
                == current.endpoint.componentId) {
            selectedIndex = index;
            break;
        }
    }
    m_targetCombo->setCurrentIndex(selectedIndex);
    if (current.isValid()) {
        const int portIndex = m_portCombo->findData(current.endpoint.linkId);
        if (portIndex >= 0) {
            m_portCombo->setCurrentIndex(portIndex);
        }
    }
}

void MainWindowHeader::selectTarget(int index)
{
    if (index < 0 || index >= m_targetCombo->count()) {
        return;
    }
    VehicleTargetManager *const targets =
        LinkManager::instance()->vehicleTargetManager();
    const int linkId = m_targetCombo->itemData(index, LinkIdRole).toInt();
    const int systemId = m_targetCombo->itemData(index, SystemIdRole).toInt();
    const int componentId =
        m_targetCombo->itemData(index, ComponentIdRole).toInt();
    if (!targets->selectTarget(linkId, systemId, componentId)) {
        syncCurrentTarget();
        return;
    }
    const VehicleTargetLease selected = targets->acquireTarget();
    ParameterService *const parameters =
        LinkManager::instance()->parameterService();
    if (selected.isValid() && parameters
        && !parameters->store()->snapshot(selected.endpoint).isComplete()
        && !(QApplication::keyboardModifiers() & Qt::ControlModifier)) {
        parameters->requestCurrentParameterList();
    }
}

void MainWindowHeader::activeVehicleChanged(UASInterface *uas)
{
    if (uas) {
        m_connectionStatus->setText(tr("Vehicle %1 · %2")
                                    .arg(uas->getUASID()).arg(uas->getUASName()));
    }
}
