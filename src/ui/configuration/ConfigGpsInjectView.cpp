#include "ConfigGpsInjectView.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QFrame>
#include <QFont>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPaintEvent>
#include <QPalette>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStyle>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

namespace {
constexpr int kSourceWidth = 160;
constexpr int kBaudWidth = 110;

QFrame *panel(QWidget *parent, const QString &objectName)
{
    auto *frame = new QFrame(parent);
    frame->setObjectName(objectName);
    frame->setProperty("gpsInjectPanel", true);
    frame->setFrameShape(QFrame::NoFrame);
    return frame;
}

QLabel *fieldLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setProperty("gpsInjectFieldLabel", true);
    return label;
}

void setComboItems(QComboBox *combo, const QStringList &items)
{
    QStringList current;
    current.reserve(combo->count());
    for (int index = 0; index < combo->count(); ++index) {
        current.append(combo->itemText(index));
    }
    if (current == items) {
        return;
    }
    combo->clear();
    combo->addItems(items);
}

void selectComboText(QComboBox *combo, const QString &text)
{
    const int index = combo->findText(text, Qt::MatchExactly);
    combo->setCurrentIndex(index);
}

void setLineText(QLineEdit *edit, const QString &text)
{
    if (edit->text() != text) {
        edit->setText(text);
    }
}
} // namespace

ConfigGpsInjectView::ConfigGpsInjectView(QWidget *parent)
    : ConfigGpsInjectView(nullptr, nullptr, parent)
{
}

ConfigGpsInjectView::ConfigGpsInjectView(
    GpsCorrectionSource *source, QSettings *settings, QWidget *parent)
    : QWidget(parent),
      m_viewModel(new ConfigGpsInjectViewModel(source, settings, this))
{
    setObjectName(QStringLiteral("ConfigGpsInjectView"));
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAutoFillBackground(true);
    QPalette opaquePalette = palette();
    opaquePalette.setColor(QPalette::Window,
                           QColor(QStringLiteral("#1A201D")));
    opaquePalette.setColor(QPalette::WindowText,
                           QColor(QStringLiteral("#E6EDE9")));
    setPalette(opaquePalette);
    setStyleSheet(QStringLiteral(
        "ConfigGpsInjectView { background: #1A201D; color: #E6EDE9; }"
        "QLabel#gpsInjectTitle { color: #E8E8E8; font-size: 16px;"
        " font-weight: bold; }"
        "QLabel[gpsInjectFieldLabel=\"true\"] { color: #C8C8C8; }"
        "QFrame[gpsInjectPanel=\"true\"] { background: #171D1A;"
        " border: 1px solid #666666; border-radius: 3px; }"
        "QComboBox, QLineEdit, QSpinBox, QPushButton {"
        " background: #161B18; color: #E6EDE9;"
        " border: 1px solid #2A322D; padding: 4px; }"
        "QComboBox:disabled, QLineEdit:disabled, QSpinBox:disabled,"
        " QPushButton:disabled, QCheckBox:disabled { color: #68736D; }"
        "QTableWidget#gpsInjectBasePositionsTable { background: #0D1210;"
        " alternate-background-color: #131916; color: #E6EDE9;"
        " gridline-color: #2A322D; }"
        "QTableWidget#gpsInjectBasePositionsTable QHeaderView::section {"
        " background: #202623; color: #E6EDE9;"
        " border: 1px solid #2A322D; padding: 5px; }"));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(10);

    auto *title = new QLabel(m_viewModel->Title(), this);
    title->setObjectName(QStringLiteral("gpsInjectTitle"));
    root->addWidget(title);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName(QStringLiteral("gpsInjectStatus"));
    m_statusLabel->setProperty("gpsInjectFieldLabel", true);
    m_statusLabel->setWordWrap(true);
    root->addWidget(m_statusLabel);

    auto *sourceRow = new QWidget(this);
    sourceRow->setObjectName(QStringLiteral("gpsInjectSourceRow"));
    auto *sourceLayout = new QHBoxLayout(sourceRow);
    sourceLayout->setContentsMargins(0, 0, 0, 0);
    sourceLayout->setSpacing(8);

    m_sourceCombo = new QComboBox(sourceRow);
    m_sourceCombo->setObjectName(QStringLiteral("gpsInjectSourceCombo"));
    m_sourceCombo->setFixedWidth(kSourceWidth);
    sourceLayout->addWidget(m_sourceCombo);

    m_baudCombo = new QComboBox(sourceRow);
    m_baudCombo->setObjectName(QStringLiteral("gpsInjectBaudCombo"));
    m_baudCombo->setFixedWidth(kBaudWidth);
    sourceLayout->addWidget(m_baudCombo);

    m_connectButton = new QPushButton(sourceRow);
    m_connectButton->setObjectName(
        QStringLiteral("gpsInjectConnectButton"));
    sourceLayout->addWidget(m_connectButton);

    m_refreshPortsButton = new QPushButton(tr("Refresh"), sourceRow);
    m_refreshPortsButton->setObjectName(
        QStringLiteral("gpsInjectRefreshPortsButton"));
    sourceLayout->addWidget(m_refreshPortsButton);
    sourceLayout->addStretch(1);
    root->addWidget(sourceRow);

    m_ntripPanel = new QFrame(this);
    m_ntripPanel->setObjectName(QStringLiteral("gpsInjectNtripPanel"));
    auto *ntripGrid = new QGridLayout(m_ntripPanel);
    ntripGrid->setContentsMargins(0, 0, 0, 0);
    ntripGrid->setHorizontalSpacing(8);
    ntripGrid->setVerticalSpacing(0);
    ntripGrid->setColumnMinimumWidth(0, 120);
    ntripGrid->setColumnStretch(1, 1);
    ntripGrid->setColumnMinimumWidth(2, 120);
    ntripGrid->setColumnStretch(3, 1);

    ntripGrid->addWidget(fieldLabel(tr("Caster Host"), m_ntripPanel),
                         0, 0);
    m_hostEdit = new QLineEdit(m_ntripPanel);
    m_hostEdit->setObjectName(QStringLiteral("gpsInjectHostEdit"));
    m_hostEdit->setPlaceholderText(tr("caster.example.com"));
    ntripGrid->addWidget(m_hostEdit, 0, 1);
    ntripGrid->addWidget(fieldLabel(tr("Port"), m_ntripPanel), 0, 2);
    m_casterPortSpin = new QSpinBox(m_ntripPanel);
    m_casterPortSpin->setObjectName(
        QStringLiteral("gpsInjectCasterPortSpin"));
    m_casterPortSpin->setRange(1, 65535);
    ntripGrid->addWidget(m_casterPortSpin, 0, 3);

    ntripGrid->addWidget(fieldLabel(tr("Mount Point"), m_ntripPanel),
                         1, 0);
    m_mountEdit = new QLineEdit(m_ntripPanel);
    m_mountEdit->setObjectName(QStringLiteral("gpsInjectMountEdit"));
    m_mountEdit->setPlaceholderText(tr("MOUNT"));
    ntripGrid->addWidget(m_mountEdit, 1, 1);
    ntripGrid->addWidget(fieldLabel(tr("Username"), m_ntripPanel),
                         1, 2);
    m_usernameEdit = new QLineEdit(m_ntripPanel);
    m_usernameEdit->setObjectName(QStringLiteral("gpsInjectUsernameEdit"));
    ntripGrid->addWidget(m_usernameEdit, 1, 3);

    ntripGrid->addWidget(fieldLabel(tr("Password"), m_ntripPanel),
                         2, 2);
    m_passwordEdit = new QLineEdit(m_ntripPanel);
    m_passwordEdit->setObjectName(QStringLiteral("gpsInjectPasswordEdit"));
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    ntripGrid->addWidget(m_passwordEdit, 2, 3);
    root->addWidget(m_ntripPanel);

    auto *flagsRow = new QWidget(this);
    flagsRow->setObjectName(QStringLiteral("gpsInjectCommonFlags"));
    auto *flagsLayout = new QHBoxLayout(flagsRow);
    flagsLayout->setContentsMargins(0, 0, 0, 0);
    flagsLayout->setSpacing(0);

    m_sendGgaCheck = new QCheckBox(
        tr("Send NTRIP GGA? (VRS/Smart)"), flagsRow);
    m_sendGgaCheck->setObjectName(QStringLiteral("gpsInjectSendGgaCheck"));
    m_sendGgaCheck->setContentsMargins(0, 0, 16, 0);
    flagsLayout->addWidget(m_sendGgaCheck);

    m_ntripV1Check = new QCheckBox(
        tr("Send NTRIP protocol v1.0 ?"), flagsRow);
    m_ntripV1Check->setObjectName(QStringLiteral("gpsInjectNtripV1Check"));
    m_ntripV1Check->setContentsMargins(0, 0, 16, 0);
    flagsLayout->addWidget(m_ntripV1Check);

    m_autoConfigCheck = new QCheckBox(
        tr("Automatically Configure Receiver"), flagsRow);
    m_autoConfigCheck->setObjectName(
        QStringLiteral("gpsInjectAutoConfigCheck"));
    m_autoConfigCheck->setContentsMargins(0, 0, 16, 0);
    flagsLayout->addWidget(m_autoConfigCheck);

    m_receiverTypeCombo = new QComboBox(flagsRow);
    m_receiverTypeCombo->setObjectName(
        QStringLiteral("gpsInjectReceiverTypeCombo"));
    m_receiverTypeCombo->setFixedWidth(180);
    flagsLayout->addWidget(m_receiverTypeCombo);
    flagsLayout->addStretch(1);
    root->addWidget(flagsRow);

    auto *linkPanel = panel(this, QStringLiteral("gpsInjectLinkStatusPanel"));
    auto *linkLayout = new QGridLayout(linkPanel);
    linkLayout->setContentsMargins(10, 10, 10, 10);
    linkLayout->setHorizontalSpacing(8);
    linkLayout->setVerticalSpacing(4);
    linkLayout->setColumnMinimumWidth(0, 160);
    linkLayout->setColumnStretch(1, 1);
    auto *linkTitle = fieldLabel(tr("Link Status"), linkPanel);
    linkTitle->setObjectName(QStringLiteral("gpsInjectLinkStatusTitle"));
    QFont linkTitleFont = linkTitle->font();
    linkTitleFont.setBold(true);
    linkTitle->setFont(linkTitleFont);
    linkLayout->addWidget(linkTitle, 0, 0, 1, 2);
    linkLayout->addWidget(fieldLabel(tr("Input data rate"), linkPanel),
                          1, 0);
    m_inputRateLabel = fieldLabel(QString(), linkPanel);
    m_inputRateLabel->setObjectName(QStringLiteral("gpsInjectInputRate"));
    linkLayout->addWidget(m_inputRateLabel, 1, 1);
    linkLayout->addWidget(
        fieldLabel(tr("Output data rate (bps)"), linkPanel), 2, 0);
    m_outputRateLabel = fieldLabel(QString(), linkPanel);
    m_outputRateLabel->setObjectName(QStringLiteral("gpsInjectOutputRate"));
    linkLayout->addWidget(m_outputRateLabel, 2, 1);
    linkLayout->addWidget(fieldLabel(tr("Injected"), linkPanel), 3, 0);
    m_injectedLabel = fieldLabel(QString(), linkPanel);
    m_injectedLabel->setObjectName(QStringLiteral("gpsInjectInjected"));
    linkLayout->addWidget(m_injectedLabel, 3, 1);
    auto *messagesTitle = fieldLabel(tr("Messages Seen"), linkPanel);
    messagesTitle->setObjectName(
        QStringLiteral("gpsInjectMessagesSeenTitle"));
    linkLayout->addWidget(messagesTitle, 4, 0, 1, 2);
    m_messagesSeenLabel = fieldLabel(QString(), linkPanel);
    m_messagesSeenLabel->setObjectName(
        QStringLiteral("gpsInjectMessagesSeen"));
    m_messagesSeenLabel->setWordWrap(true);
    linkLayout->addWidget(m_messagesSeenLabel, 5, 0, 1, 2);
    root->addWidget(linkPanel);

    auto *rtcmPanel = panel(this, QStringLiteral("gpsInjectRtcmPanel"));
    auto *rtcmLayout = new QVBoxLayout(rtcmPanel);
    rtcmLayout->setContentsMargins(10, 10, 10, 10);
    rtcmLayout->setSpacing(6);
    auto *rtcmTitle = fieldLabel(tr("RTCM"), rtcmPanel);
    rtcmTitle->setObjectName(QStringLiteral("gpsInjectRtcmTitle"));
    QFont rtcmTitleFont = rtcmTitle->font();
    rtcmTitleFont.setBold(true);
    rtcmTitle->setFont(rtcmTitleFont);
    rtcmLayout->addWidget(rtcmTitle);

    auto *indicatorRow = new QWidget(rtcmPanel);
    indicatorRow->setObjectName(QStringLiteral("gpsInjectFreshnessRow"));
    auto *indicatorLayout = new QHBoxLayout(indicatorRow);
    indicatorLayout->setContentsMargins(0, 0, 0, 0);
    indicatorLayout->setSpacing(6);
    auto makeIndicator = [indicatorRow, indicatorLayout](
                             const QString &text, const QString &name) {
        auto *indicator = new QLabel(text, indicatorRow);
        indicator->setObjectName(name);
        indicator->setAlignment(Qt::AlignCenter);
        indicator->setFixedSize(70, 24);
        indicatorLayout->addWidget(indicator);
        return indicator;
    };
    m_baseIndicator = makeIndicator(
        tr("Base"), QStringLiteral("gpsInjectBaseIndicator"));
    m_gpsIndicator = makeIndicator(
        tr("Gps"), QStringLiteral("gpsInjectGpsIndicator"));
    m_glonassIndicator = makeIndicator(
        tr("Glonass"), QStringLiteral("gpsInjectGlonassIndicator"));
    m_beidouIndicator = makeIndicator(
        tr("Beidou"), QStringLiteral("gpsInjectBeidouIndicator"));
    m_galileoIndicator = makeIndicator(
        tr("Galileo"), QStringLiteral("gpsInjectGalileoIndicator"));
    indicatorLayout->addStretch(1);
    rtcmLayout->addWidget(indicatorRow);

    auto *baseRow = new QWidget(rtcmPanel);
    auto *baseLayout = new QHBoxLayout(baseRow);
    baseLayout->setContentsMargins(0, 0, 0, 0);
    baseLayout->setSpacing(8);
    auto *baseLabel = fieldLabel(tr("RTCM Base (Lat Lng Alt)"), baseRow);
    baseLabel->setMinimumWidth(160);
    baseLayout->addWidget(baseLabel);
    m_rtcmBasePositionLabel = fieldLabel(QString(), baseRow);
    m_rtcmBasePositionLabel->setObjectName(
        QStringLiteral("gpsInjectRtcmBasePosition"));
    baseLayout->addWidget(m_rtcmBasePositionLabel, 1);
    rtcmLayout->addWidget(baseRow);
    root->addWidget(rtcmPanel);

    m_autoConfigPanel = panel(
        this, QStringLiteral("gpsInjectAutoConfigPanel"));
    auto *autoLayout = new QVBoxLayout(m_autoConfigPanel);
    autoLayout->setContentsMargins(10, 10, 10, 10);
    autoLayout->setSpacing(6);
    auto *autoTitle = fieldLabel(
        tr("Automatic Config Options"), m_autoConfigPanel);
    autoTitle->setObjectName(QStringLiteral("gpsInjectAutoConfigTitle"));
    QFont autoTitleFont = autoTitle->font();
    autoTitleFont.setBold(true);
    autoTitle->setFont(autoTitleFont);
    autoLayout->addWidget(autoTitle);

    m_m8p130PlusCheck = new QCheckBox(
        tr("M8P fw 130+/F9P"), m_autoConfigPanel);
    m_m8p130PlusCheck->setObjectName(
        QStringLiteral("gpsInjectM8p130PlusCheck"));
    autoLayout->addWidget(m_m8p130PlusCheck);

    auto *surveyRow = new QWidget(m_autoConfigPanel);
    auto *surveyLayout = new QHBoxLayout(surveyRow);
    surveyLayout->setContentsMargins(0, 0, 0, 0);
    surveyLayout->setSpacing(8);
    surveyLayout->addWidget(fieldLabel(tr("SurveyIn Acc(m)"), surveyRow));
    m_surveyAccuracyEdit = new QLineEdit(surveyRow);
    m_surveyAccuracyEdit->setObjectName(
        QStringLiteral("gpsInjectSurveyAccuracyEdit"));
    m_surveyAccuracyEdit->setFixedWidth(80);
    surveyLayout->addWidget(m_surveyAccuracyEdit);
    surveyLayout->addWidget(fieldLabel(tr("Time(s)"), surveyRow));
    m_surveyTimeEdit = new QLineEdit(surveyRow);
    m_surveyTimeEdit->setObjectName(
        QStringLiteral("gpsInjectSurveyTimeEdit"));
    m_surveyTimeEdit->setFixedWidth(80);
    surveyLayout->addWidget(m_surveyTimeEdit);
    m_restartSurveyButton = new QPushButton(tr("Restart"), surveyRow);
    m_restartSurveyButton->setObjectName(
        QStringLiteral("gpsInjectRestartSurveyButton"));
    surveyLayout->addWidget(m_restartSurveyButton);
    m_savePositionButton = new QPushButton(
        tr("Save Current Position"), surveyRow);
    m_savePositionButton->setObjectName(
        QStringLiteral("gpsInjectSaveCurrentPositionButton"));
    surveyLayout->addWidget(m_savePositionButton);
    surveyLayout->addStretch(1);
    autoLayout->addWidget(surveyRow);

    m_surveyStatusLabel = new QLabel(m_autoConfigPanel);
    m_surveyStatusLabel->setObjectName(
        QStringLiteral("gpsInjectSurveyStatus"));
    m_surveyStatusLabel->setWordWrap(true);
    m_surveyStatusLabel->setContentsMargins(6, 6, 6, 6);
    autoLayout->addWidget(m_surveyStatusLabel);
    root->addWidget(m_autoConfigPanel);

    m_septentrioPanel = panel(
        this, QStringLiteral("gpsInjectSeptentrioPanel"));
    auto *septentrioLayout = new QVBoxLayout(m_septentrioPanel);
    septentrioLayout->setContentsMargins(10, 10, 10, 10);
    septentrioLayout->setSpacing(8);
    auto *septentrioTitle = fieldLabel(tr("Septentrio"),
                                       m_septentrioPanel);
    septentrioTitle->setObjectName(
        QStringLiteral("gpsInjectSeptentrioTitle"));
    QFont septentrioTitleFont = septentrioTitle->font();
    septentrioTitleFont.setBold(true);
    septentrioTitle->setFont(septentrioTitleFont);
    septentrioLayout->addWidget(septentrioTitle);

    m_septentrioFixedCheck = new QCheckBox(
        tr("Fixed Position"), m_septentrioPanel);
    m_septentrioFixedCheck->setObjectName(
        QStringLiteral("gpsInjectSeptentrioFixedPositionCheck"));
    septentrioLayout->addWidget(m_septentrioFixedCheck);

    m_septentrioPositionWidget = new QWidget(m_septentrioPanel);
    m_septentrioPositionWidget->setObjectName(
        QStringLiteral("gpsInjectSeptentrioPositionGrid"));
    auto *positionGrid = new QGridLayout(m_septentrioPositionWidget);
    positionGrid->setContentsMargins(0, 0, 0, 0);
    positionGrid->setHorizontalSpacing(8);
    positionGrid->setVerticalSpacing(4);
    positionGrid->setColumnMinimumWidth(0, 140);
    positionGrid->setColumnMinimumWidth(1, 140);
    positionGrid->setColumnStretch(2, 1);
    positionGrid->addWidget(
        fieldLabel(tr("Atitude (WGS84)"), m_septentrioPositionWidget),
        0, 0);
    m_septentrioLatitudeEdit = new QLineEdit(m_septentrioPositionWidget);
    m_septentrioLatitudeEdit->setObjectName(
        QStringLiteral("gpsInjectSeptentrioLatitudeEdit"));
    positionGrid->addWidget(m_septentrioLatitudeEdit, 0, 1);
    positionGrid->addWidget(
        fieldLabel(tr("Longitude (WGS84)"), m_septentrioPositionWidget),
        1, 0);
    m_septentrioLongitudeEdit = new QLineEdit(m_septentrioPositionWidget);
    m_septentrioLongitudeEdit->setObjectName(
        QStringLiteral("gpsInjectSeptentrioLongitudeEdit"));
    positionGrid->addWidget(m_septentrioLongitudeEdit, 1, 1);
    positionGrid->addWidget(
        fieldLabel(tr("Altitude (m)"), m_septentrioPositionWidget),
        2, 0);
    m_septentrioAltitudeEdit = new QLineEdit(m_septentrioPositionWidget);
    m_septentrioAltitudeEdit->setObjectName(
        QStringLiteral("gpsInjectSeptentrioAltitudeEdit"));
    positionGrid->addWidget(m_septentrioAltitudeEdit, 2, 1);
    m_setSeptentrioPositionButton = new QPushButton(
        tr("Set Position"), m_septentrioPositionWidget);
    m_setSeptentrioPositionButton->setObjectName(
        QStringLiteral("gpsInjectSetSeptentrioPositionButton"));
    positionGrid->addWidget(m_setSeptentrioPositionButton, 3, 1);
    septentrioLayout->addWidget(m_septentrioPositionWidget);

    auto *rtcmSettings = new QWidget(m_septentrioPanel);
    auto *rtcmSettingsGrid = new QGridLayout(rtcmSettings);
    rtcmSettingsGrid->setContentsMargins(0, 0, 0, 0);
    rtcmSettingsGrid->setHorizontalSpacing(8);
    rtcmSettingsGrid->setVerticalSpacing(4);
    rtcmSettingsGrid->setColumnMinimumWidth(0, 160);
    rtcmSettingsGrid->setColumnStretch(1, 1);
    rtcmSettingsGrid->addWidget(
        fieldLabel(tr("RTCM Message Amount"), rtcmSettings), 0, 0);
    m_septentrioRtcmLevelCombo = new QComboBox(rtcmSettings);
    m_septentrioRtcmLevelCombo->setObjectName(
        QStringLiteral("gpsInjectSeptentrioRtcmLevelCombo"));
    m_septentrioRtcmLevelCombo->setFixedWidth(120);
    rtcmSettingsGrid->addWidget(m_septentrioRtcmLevelCombo, 0, 1,
                                Qt::AlignLeft);
    rtcmSettingsGrid->addWidget(
        fieldLabel(tr("RTCM Message Interval (s)"), rtcmSettings),
        1, 0);
    auto *intervalRow = new QWidget(rtcmSettings);
    auto *intervalLayout = new QHBoxLayout(intervalRow);
    intervalLayout->setContentsMargins(0, 0, 0, 0);
    intervalLayout->setSpacing(8);
    m_septentrioIntervalEdit = new QLineEdit(intervalRow);
    m_septentrioIntervalEdit->setObjectName(
        QStringLiteral("gpsInjectSeptentrioIntervalEdit"));
    m_septentrioIntervalEdit->setFixedWidth(80);
    intervalLayout->addWidget(m_septentrioIntervalEdit);
    m_setSeptentrioIntervalButton = new QPushButton(
        tr("Set Interval"), intervalRow);
    m_setSeptentrioIntervalButton->setObjectName(
        QStringLiteral("gpsInjectSetSeptentrioIntervalButton"));
    intervalLayout->addWidget(m_setSeptentrioIntervalButton);
    intervalLayout->addStretch(1);
    rtcmSettingsGrid->addWidget(intervalRow, 1, 1);
    septentrioLayout->addWidget(rtcmSettings);

    auto *constellationTitle = fieldLabel(
        tr("RTCM Constellation Usage"), m_septentrioPanel);
    constellationTitle->setObjectName(
        QStringLiteral("gpsInjectConstellationUsageTitle"));
    septentrioLayout->addWidget(constellationTitle);
    auto *constellationRow = new QWidget(m_septentrioPanel);
    auto *constellationLayout = new QHBoxLayout(constellationRow);
    constellationLayout->setContentsMargins(0, 0, 0, 0);
    constellationLayout->setSpacing(0);
    m_septentrioGpsCheck = new QCheckBox(tr("GPS"), constellationRow);
    m_septentrioGpsCheck->setObjectName(
        QStringLiteral("gpsInjectSeptentrioGpsCheck"));
    m_septentrioGpsCheck->setContentsMargins(0, 0, 16, 0);
    constellationLayout->addWidget(m_septentrioGpsCheck);
    m_septentrioGlonassCheck = new QCheckBox(
        tr("GLONASS"), constellationRow);
    m_septentrioGlonassCheck->setObjectName(
        QStringLiteral("gpsInjectSeptentrioGlonassCheck"));
    m_septentrioGlonassCheck->setContentsMargins(0, 0, 16, 0);
    constellationLayout->addWidget(m_septentrioGlonassCheck);
    m_septentrioGalileoCheck = new QCheckBox(
        tr("Galileo"), constellationRow);
    m_septentrioGalileoCheck->setObjectName(
        QStringLiteral("gpsInjectSeptentrioGalileoCheck"));
    m_septentrioGalileoCheck->setContentsMargins(0, 0, 16, 0);
    constellationLayout->addWidget(m_septentrioGalileoCheck);
    m_septentrioBeidouCheck = new QCheckBox(
        tr("BeiDou"), constellationRow);
    m_septentrioBeidouCheck->setObjectName(
        QStringLiteral("gpsInjectSeptentrioBeidouCheck"));
    m_septentrioBeidouCheck->setContentsMargins(0, 0, 16, 0);
    constellationLayout->addWidget(m_septentrioBeidouCheck);
    constellationLayout->addStretch(1);
    septentrioLayout->addWidget(constellationRow);
    root->addWidget(m_septentrioPanel);

    auto *savedPanel = panel(
        this, QStringLiteral("gpsInjectSavedBasePositionsPanel"));
    auto *savedLayout = new QVBoxLayout(savedPanel);
    savedLayout->setContentsMargins(10, 10, 10, 10);
    savedLayout->setSpacing(6);
    auto *savedTitle = fieldLabel(tr("Saved Base Positions"), savedPanel);
    savedTitle->setObjectName(
        QStringLiteral("gpsInjectSavedBasePositionsTitle"));
    QFont savedTitleFont = savedTitle->font();
    savedTitleFont.setBold(true);
    savedTitle->setFont(savedTitleFont);
    savedLayout->addWidget(savedTitle);

    m_basePositionsTable = new QTableWidget(savedPanel);
    m_basePositionsTable->setObjectName(
        QStringLiteral("gpsInjectBasePositionsTable"));
    m_basePositionsTable->setColumnCount(6);
    m_basePositionsTable->setHorizontalHeaderLabels({
        tr("Lat/ECEFX"), tr("Long/ECEFY"), tr("Alt/ECEFZ"),
        tr("Name"), tr("Use"), tr("Delete")
    });
    m_basePositionsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_basePositionsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_basePositionsTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_basePositionsTable->setAlternatingRowColors(true);
    m_basePositionsTable->setShowGrid(true);
    m_basePositionsTable->verticalHeader()->hide();
    m_basePositionsTable->horizontalHeader()->setSectionResizeMode(
        0, QHeaderView::Fixed);
    m_basePositionsTable->horizontalHeader()->setSectionResizeMode(
        1, QHeaderView::Fixed);
    m_basePositionsTable->horizontalHeader()->setSectionResizeMode(
        2, QHeaderView::Fixed);
    m_basePositionsTable->horizontalHeader()->setSectionResizeMode(
        3, QHeaderView::Stretch);
    m_basePositionsTable->horizontalHeader()->setSectionResizeMode(
        4, QHeaderView::Fixed);
    m_basePositionsTable->horizontalHeader()->setSectionResizeMode(
        5, QHeaderView::Fixed);
    m_basePositionsTable->setColumnWidth(0, 150);
    m_basePositionsTable->setColumnWidth(1, 150);
    m_basePositionsTable->setColumnWidth(2, 120);
    m_basePositionsTable->setColumnWidth(4, 80);
    m_basePositionsTable->setColumnWidth(5, 90);
    m_basePositionsTable->setMinimumHeight(120);
    m_basePositionsTable->setMaximumHeight(160);
    savedLayout->addWidget(m_basePositionsTable);
    root->addWidget(savedPanel);
    root->addStretch(1);

    connect(m_sourceCombo, &QComboBox::currentTextChanged,
            m_viewModel, &ConfigGpsInjectViewModel::SetSelectedPort);
    connect(m_baudCombo, &QComboBox::currentTextChanged,
            m_viewModel, &ConfigGpsInjectViewModel::SetSelectedBaud);
    connect(m_receiverTypeCombo, &QComboBox::currentTextChanged,
            m_viewModel, &ConfigGpsInjectViewModel::SetSelectedReceiverType);
    connect(m_hostEdit, &QLineEdit::textChanged,
            m_viewModel, &ConfigGpsInjectViewModel::SetHost);
    connect(m_casterPortSpin,
            QOverload<int>::of(&QSpinBox::valueChanged),
            m_viewModel, &ConfigGpsInjectViewModel::SetPort);
    connect(m_mountEdit, &QLineEdit::textChanged,
            m_viewModel, &ConfigGpsInjectViewModel::SetMount);
    connect(m_usernameEdit, &QLineEdit::textChanged,
            m_viewModel, &ConfigGpsInjectViewModel::SetUsername);
    connect(m_passwordEdit, &QLineEdit::textChanged,
            m_viewModel, &ConfigGpsInjectViewModel::SetPassword);
    connect(m_sendGgaCheck, &QCheckBox::toggled,
            m_viewModel, &ConfigGpsInjectViewModel::SetSendGga);
    connect(m_ntripV1Check, &QCheckBox::toggled,
            m_viewModel, &ConfigGpsInjectViewModel::SetNtripV1);
    connect(m_autoConfigCheck, &QCheckBox::toggled,
            m_viewModel, &ConfigGpsInjectViewModel::SetAutoConfig);
    connect(m_m8p130PlusCheck, &QCheckBox::toggled,
            m_viewModel, &ConfigGpsInjectViewModel::SetM8p130Plus);
    connect(m_surveyAccuracyEdit, &QLineEdit::textChanged,
            m_viewModel, &ConfigGpsInjectViewModel::SetSurveyInAcc);
    connect(m_surveyTimeEdit, &QLineEdit::textChanged,
            m_viewModel, &ConfigGpsInjectViewModel::SetSurveyInTime);
    connect(m_septentrioFixedCheck, &QCheckBox::toggled,
            m_viewModel,
            &ConfigGpsInjectViewModel::SetSeptentrioFixedPosition);
    connect(m_septentrioLatitudeEdit, &QLineEdit::textChanged,
            m_viewModel, &ConfigGpsInjectViewModel::SetSeptentrioLat);
    connect(m_septentrioLongitudeEdit, &QLineEdit::textChanged,
            m_viewModel, &ConfigGpsInjectViewModel::SetSeptentrioLng);
    connect(m_septentrioAltitudeEdit, &QLineEdit::textChanged,
            m_viewModel, &ConfigGpsInjectViewModel::SetSeptentrioAlt);
    connect(m_septentrioRtcmLevelCombo, &QComboBox::currentTextChanged,
            m_viewModel,
            &ConfigGpsInjectViewModel::SetSelectedSeptentrioRtcmLevel);
    connect(m_septentrioIntervalEdit, &QLineEdit::textChanged,
            m_viewModel,
            &ConfigGpsInjectViewModel::SetSeptentrioRtcmInterval);
    connect(m_septentrioGpsCheck, &QCheckBox::toggled,
            m_viewModel, &ConfigGpsInjectViewModel::SetSeptentrioGps);
    connect(m_septentrioGlonassCheck, &QCheckBox::toggled,
            m_viewModel, &ConfigGpsInjectViewModel::SetSeptentrioGlonass);
    connect(m_septentrioGalileoCheck, &QCheckBox::toggled,
            m_viewModel, &ConfigGpsInjectViewModel::SetSeptentrioGalileo);
    connect(m_septentrioBeidouCheck, &QCheckBox::toggled,
            m_viewModel, &ConfigGpsInjectViewModel::SetSeptentrioBeidou);

    connect(m_connectButton, &QPushButton::clicked, this, [this]() {
        (void) m_viewModel->ToggleConnect();
    });
    connect(m_refreshPortsButton, &QPushButton::clicked, this, [this]() {
        (void) m_viewModel->RefreshPorts();
    });
    connect(m_restartSurveyButton, &QPushButton::clicked, this, [this]() {
        (void) m_viewModel->RestartSurveyIn();
    });
    connect(m_savePositionButton, &QPushButton::clicked, this, [this]() {
        (void) m_viewModel->SaveCurrentPosition();
    });
    connect(m_setSeptentrioPositionButton, &QPushButton::clicked,
            this, [this]() {
        (void) m_viewModel->ApplySeptentrioPosition();
    });
    connect(m_setSeptentrioIntervalButton, &QPushButton::clicked,
            this, [this]() {
        (void) m_viewModel->ApplySeptentrioRtcm();
    });

    connect(m_viewModel, &ConfigGpsInjectViewModel::propertiesChanged,
            this, &ConfigGpsInjectView::syncFromModel);
    connect(m_viewModel, &ConfigGpsInjectViewModel::stateChanged,
            this, &ConfigGpsInjectView::syncFromModel);
    connect(m_viewModel, &ConfigGpsInjectViewModel::statusChanged,
            this, &ConfigGpsInjectView::syncFromModel);
    connect(m_viewModel, &ConfigGpsInjectViewModel::basePositionsChanged,
            this, &ConfigGpsInjectView::syncFromModel);
    connect(m_viewModel,
            &ConfigGpsInjectViewModel::ubloxAuthorizationRequested,
            this, &ConfigGpsInjectView::showUbloxAuthorization);

    syncFromModel();
}

ConfigGpsInjectView::~ConfigGpsInjectView()
{
    if (m_viewModel) {
        disconnect(m_viewModel, nullptr, this, nullptr);
    }
    const quint64 authorizationId = m_ubloxAuthorizationId;
    m_ubloxAuthorizationId = 0;
    QPointer<QMessageBox> dialog(m_ubloxAuthorizationDialog);
    m_ubloxAuthorizationDialog.clear();
    if (dialog) {
        disconnect(dialog, nullptr, this, nullptr);
        dialog->blockSignals(true);
        dialog->reject();
    }
    if (m_viewModel && authorizationId != 0) {
        (void) m_viewModel->ResolveUbloxAuthorization(
            authorizationId, false);
    }
}

void ConfigGpsInjectView::showUbloxAuthorization(
    quint64 authorizationId, const QString &confirmationText)
{
    if (authorizationId == 0 || !m_viewModel
        || authorizationId != m_viewModel->PendingUbloxAuthorization()) {
        return;
    }
    if (m_ubloxAuthorizationDialog) {
        // The model admits only one frozen authorization at a time.  A
        // duplicate notification must not replace or implicitly accept the
        // already visible consent boundary.
        return;
    }

    auto *dialog = new QMessageBox(
        QMessageBox::Warning, tr("Authorize u-blox Auto Configure"),
        confirmationText, QMessageBox::Yes | QMessageBox::Cancel, this);
    dialog->setObjectName(
        QStringLiteral("gpsInjectUbloxAutoConfigureConfirmation"));
    dialog->setTextFormat(Qt::PlainText);
    dialog->setDefaultButton(QMessageBox::Cancel);
    dialog->setEscapeButton(QMessageBox::Cancel);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    if (QAbstractButton *confirm = dialog->button(QMessageBox::Yes)) {
        confirm->setObjectName(
            QStringLiteral("gpsInjectUbloxAutoConfigureConfirmButton"));
        confirm->setText(tr("Configure and Connect"));
    }
    m_ubloxAuthorizationDialog = dialog;
    m_ubloxAuthorizationId = authorizationId;
    connect(dialog, &QDialog::finished, this,
            [this, authorizationId](int result) {
        if (m_ubloxAuthorizationId != authorizationId) {
            return;
        }
        m_ubloxAuthorizationDialog.clear();
        m_ubloxAuthorizationId = 0;
        if (m_viewModel) {
            (void) m_viewModel->ResolveUbloxAuthorization(
                authorizationId, result == QMessageBox::Yes);
        }
    });
    dialog->open();
}

QSize ConfigGpsInjectView::sizeHint() const
{
    return QSize(900, 760);
}

void ConfigGpsInjectView::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);
    painter.fillRect(event->rect(), palette().brush(QPalette::Window));
}

void ConfigGpsInjectView::syncFromModel()
{
    const QSignalBlocker sourceBlocker(m_sourceCombo);
    const QSignalBlocker baudBlocker(m_baudCombo);
    const QSignalBlocker receiverBlocker(m_receiverTypeCombo);
    const QSignalBlocker hostBlocker(m_hostEdit);
    const QSignalBlocker portBlocker(m_casterPortSpin);
    const QSignalBlocker mountBlocker(m_mountEdit);
    const QSignalBlocker usernameBlocker(m_usernameEdit);
    const QSignalBlocker passwordBlocker(m_passwordEdit);
    const QSignalBlocker sendGgaBlocker(m_sendGgaCheck);
    const QSignalBlocker ntripV1Blocker(m_ntripV1Check);
    const QSignalBlocker autoConfigBlocker(m_autoConfigCheck);
    const QSignalBlocker m8pBlocker(m_m8p130PlusCheck);
    const QSignalBlocker surveyAccuracyBlocker(m_surveyAccuracyEdit);
    const QSignalBlocker surveyTimeBlocker(m_surveyTimeEdit);
    const QSignalBlocker fixedBlocker(m_septentrioFixedCheck);
    const QSignalBlocker latitudeBlocker(m_septentrioLatitudeEdit);
    const QSignalBlocker longitudeBlocker(m_septentrioLongitudeEdit);
    const QSignalBlocker altitudeBlocker(m_septentrioAltitudeEdit);
    const QSignalBlocker levelBlocker(m_septentrioRtcmLevelCombo);
    const QSignalBlocker intervalBlocker(m_septentrioIntervalEdit);
    const QSignalBlocker gpsBlocker(m_septentrioGpsCheck);
    const QSignalBlocker glonassBlocker(m_septentrioGlonassCheck);
    const QSignalBlocker galileoBlocker(m_septentrioGalileoCheck);
    const QSignalBlocker beidouBlocker(m_septentrioBeidouCheck);

    setComboItems(m_sourceCombo, m_viewModel->Ports());
    selectComboText(m_sourceCombo, m_viewModel->SelectedPort());
    setComboItems(m_baudCombo, m_viewModel->BaudRates());
    selectComboText(m_baudCombo, m_viewModel->SelectedBaud());
    setComboItems(m_receiverTypeCombo, m_viewModel->ReceiverTypes());
    selectComboText(m_receiverTypeCombo,
                    m_viewModel->SelectedReceiverType());
    setComboItems(m_septentrioRtcmLevelCombo,
                  m_viewModel->SeptentrioRtcmLevels());
    selectComboText(m_septentrioRtcmLevelCombo,
                    m_viewModel->SelectedSeptentrioRtcmLevel());

    setLineText(m_hostEdit, m_viewModel->Host());
    m_casterPortSpin->setValue(m_viewModel->Port());
    setLineText(m_mountEdit, m_viewModel->Mount());
    setLineText(m_usernameEdit, m_viewModel->Username());
    setLineText(m_passwordEdit, m_viewModel->Password());
    m_sendGgaCheck->setChecked(m_viewModel->SendGga());
    m_ntripV1Check->setChecked(m_viewModel->NtripV1());
    m_autoConfigCheck->setChecked(m_viewModel->AutoConfig());
    m_m8p130PlusCheck->setChecked(m_viewModel->M8p130Plus());
    setLineText(m_surveyAccuracyEdit, m_viewModel->SurveyInAcc());
    setLineText(m_surveyTimeEdit, m_viewModel->SurveyInTime());
    m_septentrioFixedCheck->setChecked(
        m_viewModel->SeptentrioFixedPosition());
    setLineText(m_septentrioLatitudeEdit, m_viewModel->SeptentrioLat());
    setLineText(m_septentrioLongitudeEdit, m_viewModel->SeptentrioLng());
    setLineText(m_septentrioAltitudeEdit, m_viewModel->SeptentrioAlt());
    setLineText(m_septentrioIntervalEdit,
                m_viewModel->SeptentrioRtcmInterval());
    m_septentrioGpsCheck->setChecked(m_viewModel->SeptentrioGps());
    m_septentrioGlonassCheck->setChecked(
        m_viewModel->SeptentrioGlonass());
    m_septentrioGalileoCheck->setChecked(
        m_viewModel->SeptentrioGalileo());
    m_septentrioBeidouCheck->setChecked(
        m_viewModel->SeptentrioBeidou());

    m_statusLabel->setText(m_viewModel->Status());
    m_connectButton->setText(m_viewModel->ConnectLabel());
    m_inputRateLabel->setText(m_viewModel->InputRate());
    m_outputRateLabel->setText(m_viewModel->OutputRate());
    m_injectedLabel->setText(m_viewModel->Injected());
    m_messagesSeenLabel->setText(m_viewModel->MessagesSeen());
    m_rtcmBasePositionLabel->setText(m_viewModel->RtcmBasePos());
    m_surveyStatusLabel->setText(m_viewModel->SurveyInStatus());

    setFreshIndicator(m_baseIndicator, m_viewModel->BaseFresh());
    setFreshIndicator(m_gpsIndicator, m_viewModel->GpsFresh());
    setFreshIndicator(m_glonassIndicator, m_viewModel->GlonassFresh());
    setFreshIndicator(m_beidouIndicator, m_viewModel->BeidouFresh());
    setFreshIndicator(m_galileoIndicator, m_viewModel->GalileoFresh());
    setFreshIndicator(m_surveyStatusLabel,
                      m_viewModel->SurveyInValid());

    const bool canEditSource = m_viewModel->CanEditSource();
    m_connectButton->setEnabled(m_viewModel->CanToggleConnect());
    m_sourceCombo->setEnabled(canEditSource);
    m_baudCombo->setEnabled(canEditSource && m_viewModel->IsSerial());
    m_refreshPortsButton->setEnabled(canEditSource);
    m_ntripPanel->setVisible(m_viewModel->IsNtrip());
    m_ntripPanel->setEnabled(canEditSource);
    m_sendGgaCheck->setEnabled(canEditSource);
    m_ntripV1Check->setEnabled(canEditSource);
    m_autoConfigCheck->setEnabled(canEditSource);
    m_receiverTypeCombo->setVisible(m_viewModel->AutoConfig());
    m_receiverTypeCombo->setEnabled(canEditSource);
    m_autoConfigPanel->setVisible(m_viewModel->AutoConfig());
    m_m8p130PlusCheck->setEnabled(!m_viewModel->ReceiverBusy());
    m_surveyAccuracyEdit->setEnabled(!m_viewModel->ReceiverBusy());
    m_surveyTimeEdit->setEnabled(!m_viewModel->ReceiverBusy());
    m_restartSurveyButton->setEnabled(
        m_viewModel->CanRestartSurveyIn());
    m_savePositionButton->setEnabled(
        m_viewModel->HasCurrentBasePosition());
    m_septentrioPanel->setVisible(m_viewModel->IsSeptentrio());
    m_septentrioPositionWidget->setEnabled(
        m_viewModel->SeptentrioFixedPosition());

    if (m_renderedBasePositions != m_viewModel->BasePositions()) {
        rebuildBasePositions();
    }
    for (int row = 0; row < m_basePositionsTable->rowCount(); ++row) {
        if (QWidget *use = m_basePositionsTable->cellWidget(row, 4)) {
            use->setEnabled(m_viewModel->CanUseBasePosition());
        }
    }
}

void ConfigGpsInjectView::rebuildBasePositions()
{
    m_renderedBasePositions = m_viewModel->BasePositions();
    m_basePositionsTable->setRowCount(m_renderedBasePositions.size());
    for (int rowIndex = 0; rowIndex < m_renderedBasePositions.size();
         ++rowIndex) {
        const BasePosRow row = m_renderedBasePositions.at(rowIndex);
        auto item = [](const QString &text) {
            auto *tableItem = new QTableWidgetItem(text);
            tableItem->setFlags(tableItem->flags() & ~Qt::ItemIsEditable);
            return tableItem;
        };
        m_basePositionsTable->setItem(rowIndex, 0, item(row.Lat));
        m_basePositionsTable->setItem(rowIndex, 1, item(row.Long));
        m_basePositionsTable->setItem(rowIndex, 2, item(row.Alt));
        m_basePositionsTable->setItem(rowIndex, 3, item(row.Name));

        auto *useButton = new QPushButton(tr("Use"), m_basePositionsTable);
        useButton->setObjectName(
            QStringLiteral("gpsInjectUseBasePosition_%1").arg(rowIndex));
        useButton->setProperty("basePositionRow", rowIndex);
        m_basePositionsTable->setCellWidget(rowIndex, 4, useButton);
        connect(useButton, &QPushButton::clicked, this,
                [this, row]() { m_viewModel->UseBasePos(row); });

        auto *deleteButton = new QPushButton(
            tr("Delete"), m_basePositionsTable);
        deleteButton->setObjectName(
            QStringLiteral("gpsInjectDeleteBasePosition_%1")
                .arg(rowIndex));
        deleteButton->setProperty("basePositionRow", rowIndex);
        m_basePositionsTable->setCellWidget(rowIndex, 5, deleteButton);
        connect(deleteButton, &QPushButton::clicked, this,
                [this, row]() { m_viewModel->DeleteBasePos(row); });
    }
}

void ConfigGpsInjectView::setFreshIndicator(QLabel *indicator, bool fresh)
{
    indicator->setProperty("fresh", fresh);
    indicator->setStyleSheet(QStringLiteral(
        "background: %1; color: white; border-radius: 2px;")
        .arg(fresh ? QStringLiteral("#228B22")
                   : QStringLiteral("#8B1A1A")));
    indicator->style()->unpolish(indicator);
    indicator->style()->polish(indicator);
}
