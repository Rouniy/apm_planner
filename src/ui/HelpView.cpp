#include "HelpView.h"

#include "configuration.h"

#include <QCoreApplication>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVariant>
#include <QVBoxLayout>

#define APM_STRINGIFY_DETAIL(value) #value
#define APM_STRINGIFY(value) APM_STRINGIFY_DETAIL(value)

namespace {
QLabel *makeTextLabel(const QString &text, QWidget *parent,
                      const QString &objectName = QString())
{
    auto *label = new QLabel(text, parent);
    label->setObjectName(objectName);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

QLabel *makeLinkLabel(const QString &name, const QString &url, QWidget *parent)
{
    auto *label = new QLabel(
        QStringLiteral("<a style=\"color:#7fb3e0;text-decoration:none\" "
                       "href=\"%1\">%2: %1</a>").arg(url, name), parent);
    label->setProperty("helpLink", true);
    label->setOpenExternalLinks(true);
    label->setTextInteractionFlags(Qt::LinksAccessibleByMouse
                                   | Qt::LinksAccessibleByKeyboard
                                   | Qt::TextSelectableByMouse);
    return label;
}
}

HelpView::HelpView(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("HelpView"));
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(QStringLiteral(
        "HelpView, QScrollArea#helpScroll,"
        "QScrollArea#helpScroll QWidget#qt_scrollarea_viewport,"
        "QWidget#helpViewportHost, QWidget#helpContent { "
        "background: #1a201d; color: #e6ede9; "
        "font-family: sans-serif; }"
        "QLabel { color: #e6ede9; background: transparent; }"
        "QLabel#helpVersionLabel { color: #34d399; font-weight: 600; }"
        "QLabel[helpSection=\"true\"] { font-weight: bold; }"
        "QLabel[helpLink=\"true\"] { color: #7fb3e0; }"
        "QLabel[helpLink=\"true\"] a { color: #7fb3e0; }"
        "QLabel#helpLibrariesLabel, QLabel#helpUpdateStatus { color: #bbbbbb; }"
        "QPushButton { min-height: 28px; padding: 4px 8px; color: #06251a; "
        "font-weight: 600; border: 0; border-radius: 3px; "
        "background: qlineargradient(x1:0, y1:0, x2:0, y2:1, "
        "stop:0 #34d399, stop:1 #10b981); }"
        "QPushButton:hover { background: #10b981; }"
        "QPushButton:pressed { background: #059669; }"
        "QPushButton:disabled { color: #737a76; background: #161b18; }"
        "QScrollBar:vertical { width: 12px; margin: 0; border: 0; "
        "background: #1a201d; }"
        "QScrollBar::handle:vertical { min-height: 24px; border: 0; "
        "border-radius: 3px; background: #2a322d; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { "
        "height: 0; border: 0; background: #1a201d; }"));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto *scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("helpScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    root->addWidget(scroll);

    auto *viewportHost = new QWidget(scroll);
    viewportHost->setObjectName(QStringLiteral("helpViewportHost"));
    viewportHost->setAttribute(Qt::WA_StyledBackground, true);
    auto *viewportLayout = new QHBoxLayout(viewportHost);
    // Avalonia's StackPanel has an external 40 DIP margin in addition to its
    // 900 DIP MaxWidth. Keep the same geometry instead of consuming 80 pixels
    // from the useful text width.
    viewportLayout->setContentsMargins(40, 40, 40, 40);
    viewportLayout->setSpacing(0);

    auto *content = new QWidget(viewportHost);
    content->setObjectName(QStringLiteral("helpContent"));
    content->setAttribute(Qt::WA_StyledBackground, true);
    content->setFixedWidth(900);
    auto *contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(8);

    auto *welcome = makeTextLabel(
        tr("<b>Welcome to APM Planner 3.0</b>, mission planning for "
           "Unmanned Aerial Vehicles (UAV)."),
        content, QStringLiteral("helpWelcomeLabel"));
    welcome->setTextFormat(Qt::RichText);
    contentLayout->addWidget(welcome);

    QString version = QCoreApplication::applicationVersion();
    if (version.isEmpty()) {
        version = QStringLiteral(QGC_APPLICATION_VERSION);
    }
    const QString commit = QString::fromLatin1(APM_STRINGIFY(GIT_COMMIT));
    auto *versionLabel = makeTextLabel(
        commit.isEmpty() || commit == QStringLiteral("GIT_COMMIT")
            ? tr("Version %1").arg(version)
            : tr("Version %1 (%2)").arg(version, commit),
        content, QStringLiteral("helpVersionLabel"));
    contentLayout->addWidget(versionLabel);

    auto *helpTitle = makeTextLabel(tr("Help:"), content,
                                    QStringLiteral("helpSectionLabel"));
    helpTitle->setProperty("helpSection", true);
    helpTitle->setContentsMargins(0, 10, 0, 0);
    contentLayout->addWidget(helpTitle);
    contentLayout->addWidget(makeLinkLabel(QStringLiteral("ArduPlane"),
                                            QStringLiteral("https://ardupilot.org/plane"),
                                            content));
    contentLayout->addWidget(makeLinkLabel(QStringLiteral("ArduCopter"),
                                            QStringLiteral("https://ardupilot.org/copter"),
                                            content));
    contentLayout->addWidget(makeLinkLabel(QStringLiteral("ArduRover"),
                                            QStringLiteral("https://ardupilot.org/rover"),
                                            content));

    auto *libraries = makeTextLabel(
        tr("Libraries: Qt, MAVLink, QuaZip, SDL, alglib, "
           "QCustomPlot, OPMapControl"),
        content, QStringLiteral("helpLibrariesLabel"));
    libraries->setContentsMargins(0, 10, 0, 0);
    contentLayout->addWidget(libraries);

    auto *shortcutsTitle = makeTextLabel(tr("ShortCuts"), content,
                                         QStringLiteral("helpShortcutsLabel"));
    shortcutsTitle->setProperty("helpSection", true);
    shortcutsTitle->setContentsMargins(0, 16, 0, 0);
    contentLayout->addWidget(shortcutsTitle);

    const QStringList shortcuts = {
        tr("F2 - FlightData"),
        tr("F3 - FlightPlanner"),
        tr("F4 - Tuning"),
        tr("F5 - Refresh full param list"),
        tr("F12 - Connect / Disconnect"),
        tr("Ctrl+F - Developer Tools"),
        tr("Ctrl+P - Plugin Manager"),
        tr("Ctrl+I - MAVLink Inspector"),
        tr("Ctrl+G - NMEA Output"),
        tr("Ctrl+L - DataFlash Spectrogram"),
        tr("Ctrl+X - Map Tile Cache"),
        tr("Ctrl+J - MAVLink Device Operations"),
        tr("Ctrl+Y - Save parameters to EEPROM")
    };
    for (int i = 0; i < shortcuts.size(); ++i) {
        contentLayout->addWidget(makeTextLabel(
            shortcuts.at(i), content,
            QStringLiteral("helpShortcut%1").arg(i + 1)));
    }

    auto *buttonRow = new QHBoxLayout;
    buttonRow->setContentsMargins(0, 16, 0, 0);
    buttonRow->setSpacing(10);
    m_stableUpdateButton = new QPushButton(tr("Check for Updates"), content);
    m_stableUpdateButton->setObjectName(QStringLiteral("helpStableUpdateButton"));
    buttonRow->addWidget(m_stableUpdateButton);
    m_betaUpdateButton = new QPushButton(tr("Check for Beta Updates"), content);
    m_betaUpdateButton->setObjectName(QStringLiteral("helpBetaUpdateButton"));
    buttonRow->addWidget(m_betaUpdateButton);
    buttonRow->addStretch();
    contentLayout->addLayout(buttonRow);

    m_updateStatusLabel = makeTextLabel(
        QString(), content, QStringLiteral("helpUpdateStatus"));
    m_updateStatusLabel->setContentsMargins(0, 6, 0, 0);
    m_updateStatusLabel->hide();
    contentLayout->addWidget(m_updateStatusLabel);
    contentLayout->addStretch();

    viewportLayout->addWidget(content, 0, Qt::AlignLeft | Qt::AlignTop);
    viewportLayout->addStretch();
    scroll->setWidget(viewportHost);

    connect(m_stableUpdateButton, &QPushButton::clicked, this, [this]() {
        setUpdateCheckInProgress(true, tr("Checking for updates…"));
        emit checkForUpdatesRequested();
    });
    connect(m_betaUpdateButton, &QPushButton::clicked, this, [this]() {
        setUpdateCheckInProgress(true, tr("Checking for beta updates…"));
        emit checkForBetaUpdatesRequested();
    });
}

bool HelpView::updateCheckInProgress() const
{
    return m_updateCheckInProgress;
}

QString HelpView::updateStatus() const
{
    return m_updateStatusLabel->text();
}

void HelpView::setUpdateCheckInProgress(bool checking, const QString &status)
{
    m_updateCheckInProgress = checking;
    m_stableUpdateButton->setDisabled(checking);
    m_betaUpdateButton->setDisabled(checking);
    if (!status.isNull()) {
        setUpdateStatus(status);
    } else if (!checking) {
        setUpdateStatus(QString());
    }
}

void HelpView::setUpdateStatus(const QString &status)
{
    m_updateStatusLabel->setText(status);
    m_updateStatusLabel->setVisible(!status.isEmpty());
}
