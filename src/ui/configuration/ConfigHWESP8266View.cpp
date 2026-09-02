#include "ConfigHWESP8266View.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace {
const int kLabelColumnWidth = 140; // MP10 ColumnDefinitions="140,*"
const int kComboWidth = 120;       // MP10 ComboBox Width="120"
}

ConfigHWESP8266View::ConfigHWESP8266View(Esp8266ParameterClient *client, QWidget *parent)
    : QWidget(parent), m_viewModel(new ConfigHWESP8266ViewModel(this))
{
    setObjectName(QStringLiteral("ConfigHWESP8266View"));
    buildUi();
    m_viewModel->setClient(client);
    syncControls();
}

ConfigHWESP8266View::~ConfigHWESP8266View() = default; // the view model cancels

void ConfigHWESP8266View::buildUi()
{
    setStyleSheet(QStringLiteral(
        "ConfigHWESP8266View { background: #1A201D; color: #E6EDE9; }"
        "QLabel#hwEspTitle { color: #E8E8E8; font-size: 16px; font-weight: bold; }"
        "QLabel#hwEspStatus, QLabel#hwEspDetails, ConfigHWESP8266View QLabel[fieldLabel=\"true\"]"
        " { color: #BBC5BF; }"
        "QFrame#hwEspStationBox { border: 1px solid #0D1210; }"
        "ConfigHWESP8266View QLineEdit, ConfigHWESP8266View QComboBox {"
        " background: #161B18; color: #E6EDE9; border: 1px solid #2A322D; padding: 4px; }"
        "ConfigHWESP8266View QPushButton { padding: 5px 12px; }"));

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    m_scroll = new QScrollArea(this);
    m_scroll->setObjectName(QStringLiteral("hwEspScroll"));
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setWidgetResizable(true);
    auto *content = new QWidget(m_scroll);
    content->setObjectName(QStringLiteral("hwEspContent"));
    auto *root = new QVBoxLayout(content);
    root->setContentsMargins(16, 16, 16, 16); // Grid Margin="16"
    root->setSpacing(0);

    auto fieldLabel = [content](const QString &text, const QString &name) {
        auto *label = new QLabel(text, content);
        label->setObjectName(name);
        label->setProperty("fieldLabel", true);
        label->setFixedWidth(kLabelColumnWidth);
        return label;
    };

    m_title = new QLabel(ConfigHWESP8266ViewModel::Title(), content);
    m_title->setObjectName(QStringLiteral("hwEspTitle"));
    root->addWidget(m_title);

    m_status = new QLabel(content);
    m_status->setObjectName(QStringLiteral("hwEspStatus"));
    m_status->setWordWrap(true);
    m_status->setTextFormat(Qt::PlainText);
    m_status->setContentsMargins(0, 0, 0, 12);
    root->addWidget(m_status);

    // Row 2: SSID / Password / WiFi Channel / UART Baud (rows Margin="0,4").
    auto *fields = new QGridLayout;
    fields->setContentsMargins(0, 0, 0, 12);
    fields->setHorizontalSpacing(8);
    fields->setVerticalSpacing(8);
    fields->setColumnStretch(1, 1);
    m_ssid = new QLineEdit(content);
    m_ssid->setObjectName(QStringLiteral("hwEspSsid"));
    fields->addWidget(fieldLabel(tr("SSID"), QStringLiteral("hwEspSsidLabel")), 0, 0);
    fields->addWidget(m_ssid, 0, 1);
    m_password = new QLineEdit(content);
    m_password->setObjectName(QStringLiteral("hwEspPassword"));
    m_password->setEchoMode(QLineEdit::Normal); // MP10 shows the password in a plain TextBox
    fields->addWidget(fieldLabel(tr("Password"), QStringLiteral("hwEspPasswordLabel")), 1, 0);
    fields->addWidget(m_password, 1, 1);
    m_channel = new QComboBox(content);
    m_channel->setObjectName(QStringLiteral("hwEspChannel"));
    m_channel->setFixedWidth(kComboWidth);
    m_channel->addItems(m_viewModel->channelOptions());
    fields->addWidget(fieldLabel(tr("WiFi Channel"), QStringLiteral("hwEspChannelLabel")), 2, 0);
    fields->addWidget(m_channel, 2, 1, Qt::AlignLeft);
    m_baud = new QComboBox(content);
    m_baud->setObjectName(QStringLiteral("hwEspBaud"));
    m_baud->setFixedWidth(kComboWidth);
    m_baud->addItems(m_viewModel->baudOptions());
    fields->addWidget(fieldLabel(tr("UART Baud"), QStringLiteral("hwEspBaudLabel")), 3, 0);
    fields->addWidget(m_baud, 3, 1, Qt::AlignLeft);
    root->addLayout(fields);

    // Row 3: bordered station box (Padding="12", bottom margin 12).
    auto *stationBox = new QFrame(content);
    stationBox->setObjectName(QStringLiteral("hwEspStationBox"));
    stationBox->setFrameShape(QFrame::StyledPanel);
    m_stationBox = stationBox;
    auto *stationLayout = new QVBoxLayout(stationBox);
    stationLayout->setContentsMargins(12, 12, 12, 12);
    stationLayout->setSpacing(8);
    m_staMode = new QCheckBox(tr("Station (STA) mode"), stationBox);
    m_staMode->setObjectName(QStringLiteral("hwEspStaMode"));
    stationLayout->addWidget(m_staMode);
    m_stationGrid = new QWidget(stationBox);
    m_stationGrid->setObjectName(QStringLiteral("hwEspStationGrid"));
    auto *stationGrid = new QGridLayout(m_stationGrid);
    stationGrid->setContentsMargins(0, 0, 0, 0);
    stationGrid->setHorizontalSpacing(8);
    stationGrid->setVerticalSpacing(8);
    stationGrid->setColumnStretch(1, 1);
    m_ipSta = new QLineEdit(m_stationGrid);
    m_ipSta->setObjectName(QStringLiteral("hwEspIpSta"));
    stationGrid->addWidget(fieldLabel(tr("IP"), QStringLiteral("hwEspIpLabel")), 0, 0);
    stationGrid->addWidget(m_ipSta, 0, 1);
    m_gatewaySta = new QLineEdit(m_stationGrid);
    m_gatewaySta->setObjectName(QStringLiteral("hwEspGatewaySta"));
    stationGrid->addWidget(fieldLabel(tr("Gateway"), QStringLiteral("hwEspGatewayLabel")), 1, 0);
    stationGrid->addWidget(m_gatewaySta, 1, 1);
    m_subnetSta = new QLineEdit(m_stationGrid);
    m_subnetSta->setObjectName(QStringLiteral("hwEspSubnetSta"));
    stationGrid->addWidget(fieldLabel(tr("Subnet"), QStringLiteral("hwEspSubnetLabel")), 2, 0);
    stationGrid->addWidget(m_subnetSta, 2, 1);
    stationLayout->addWidget(m_stationGrid);
    root->addWidget(stationBox);
    root->addSpacing(12);

    // Row 4: buttons (Spacing="12") and the details dump.
    auto *buttons = new QHBoxLayout;
    buttons->setContentsMargins(0, 0, 0, 0);
    buttons->setSpacing(12);
    m_saveButton = new QPushButton(tr("Save"), content);
    m_saveButton->setObjectName(QStringLiteral("hwEspSave"));
    buttons->addWidget(m_saveButton);
    m_resetButton = new QPushButton(tr("Reset to defaults"), content);
    m_resetButton->setObjectName(QStringLiteral("hwEspResetDefaults"));
    buttons->addWidget(m_resetButton);
    buttons->addStretch(1);
    root->addLayout(buttons);
    root->addSpacing(12);
    m_details = new QLabel(content);
    m_details->setObjectName(QStringLiteral("hwEspDetails"));
    m_details->setWordWrap(true);
    m_details->setTextFormat(Qt::PlainText);
    m_details->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(m_details);
    root->addStretch(1);

    m_scroll->setWidget(content);
    outer->addWidget(m_scroll);

    // Widget -> model.
    connect(m_ssid, &QLineEdit::textChanged, m_viewModel, &ConfigHWESP8266ViewModel::setSsid);
    connect(m_password, &QLineEdit::textChanged, m_viewModel,
            &ConfigHWESP8266ViewModel::setPassword);
    connect(m_channel, &QComboBox::currentTextChanged, m_viewModel,
            &ConfigHWESP8266ViewModel::setChannel);
    connect(m_baud, &QComboBox::currentTextChanged, m_viewModel,
            &ConfigHWESP8266ViewModel::setBaud);
    connect(m_staMode, &QCheckBox::toggled, m_viewModel, &ConfigHWESP8266ViewModel::setStaMode);
    connect(m_ipSta, &QLineEdit::textChanged, m_viewModel, &ConfigHWESP8266ViewModel::setIpSta);
    connect(m_gatewaySta, &QLineEdit::textChanged, m_viewModel,
            &ConfigHWESP8266ViewModel::setGatewaySta);
    connect(m_subnetSta, &QLineEdit::textChanged, m_viewModel,
            &ConfigHWESP8266ViewModel::setSubnetSta);
    connect(m_saveButton, &QPushButton::clicked, this, &ConfigHWESP8266View::save);
    connect(m_resetButton, &QPushButton::clicked, this, &ConfigHWESP8266View::resetDefaults);

    // Model -> widgets.
    connect(m_viewModel, &ConfigHWESP8266ViewModel::ssidChanged, this, [this](const QString &v) {
        const QSignalBlocker blocker(m_ssid);
        m_ssid->setText(v);
    });
    connect(m_viewModel, &ConfigHWESP8266ViewModel::passwordChanged, this,
            [this](const QString &v) {
                const QSignalBlocker blocker(m_password);
                m_password->setText(v);
            });
    connect(m_viewModel, &ConfigHWESP8266ViewModel::channelChanged, this,
            [this](const QString &v) { selectComboValue(m_channel, v); });
    connect(m_viewModel, &ConfigHWESP8266ViewModel::baudChanged, this,
            [this](const QString &v) { selectComboValue(m_baud, v); });
    connect(m_viewModel, &ConfigHWESP8266ViewModel::staModeChanged, this, [this](bool v) {
        const QSignalBlocker blocker(m_staMode);
        m_staMode->setChecked(v);
        syncControls();
    });
    connect(m_viewModel, &ConfigHWESP8266ViewModel::ipStaChanged, this, [this](const QString &v) {
        const QSignalBlocker blocker(m_ipSta);
        m_ipSta->setText(v);
    });
    connect(m_viewModel, &ConfigHWESP8266ViewModel::gatewayStaChanged, this,
            [this](const QString &v) {
                const QSignalBlocker blocker(m_gatewaySta);
                m_gatewaySta->setText(v);
            });
    connect(m_viewModel, &ConfigHWESP8266ViewModel::subnetStaChanged, this,
            [this](const QString &v) {
                const QSignalBlocker blocker(m_subnetSta);
                m_subnetSta->setText(v);
            });
    connect(m_viewModel, &ConfigHWESP8266ViewModel::detailsChanged, m_details, &QLabel::setText);
    connect(m_viewModel, &ConfigHWESP8266ViewModel::statusChanged, m_status, &QLabel::setText);
    connect(m_viewModel, &ConfigHWESP8266ViewModel::busyChanged, this,
            [this](bool) { syncControls(); });
    connect(m_viewModel, &ConfigHWESP8266ViewModel::connectedChanged, this,
            [this](bool) { syncControls(); });
    connect(m_staMode, &QCheckBox::toggled, this, [this](bool) { syncControls(); });

    // Initial values from the model (defaults and "Not connected.").
    selectComboValue(m_channel, m_viewModel->channel());
    selectComboValue(m_baud, m_viewModel->baud());
    m_ipSta->setText(m_viewModel->ipSta());
    m_gatewaySta->setText(m_viewModel->gatewaySta());
    m_subnetSta->setText(m_viewModel->subnetSta());
    m_staMode->setChecked(m_viewModel->staMode());
    m_status->setText(m_viewModel->status());
    m_details->setText(m_viewModel->details());
}

void ConfigHWESP8266View::selectComboValue(QComboBox *combo, const QString &value)
{
    const QSignalBlocker blocker(combo);
    int index = combo->findText(value);
    if (index < 0 && !value.isEmpty()) {
        // A device value outside MP10's list stays visible instead of vanishing.
        combo->addItem(value);
        index = combo->count() - 1;
    }
    combo->setCurrentIndex(index);
}

void ConfigHWESP8266View::syncControls()
{
    const bool busy = m_viewModel->isBusy();
    const bool editable = !busy;
    m_ssid->setEnabled(editable);
    m_password->setEnabled(editable);
    m_channel->setEnabled(editable);
    m_baud->setEnabled(editable);
    m_staMode->setEnabled(editable);
    // MP10: the IP/Gateway/Subnet grid follows the Station (STA) checkbox.
    m_stationGrid->setEnabled(editable && m_staMode->isChecked());
    m_saveButton->setEnabled(editable);
    m_resetButton->setEnabled(editable);
}

void ConfigHWESP8266View::setConnected(bool connected)
{
    m_viewModel->setConnected(connected);
}

void ConfigHWESP8266View::activate()
{
    m_viewModel->activate();
}

void ConfigHWESP8266View::deactivate()
{
    m_viewModel->deactivate();
}

void ConfigHWESP8266View::parameterTargetChanged()
{
    m_viewModel->parameterTargetChanged();
}

void ConfigHWESP8266View::save()
{
    m_viewModel->save();
}

void ConfigHWESP8266View::resetDefaults()
{
    m_viewModel->resetDefaults();
}
