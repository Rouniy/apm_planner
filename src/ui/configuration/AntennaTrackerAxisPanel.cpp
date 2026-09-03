#include "AntennaTrackerAxisPanel.h"

#include <QCheckBox>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QSlider>
#include <QVBoxLayout>

namespace {
const int kLabelColumnWidth = 110; // MP10 Grid ColumnDefinitions="110,*"
}

AntennaTrackerAxisPanel::AntennaTrackerAxisPanel(Axis axis, const Options &options, QWidget *parent)
    : QFrame(parent)
    , m_axis(axis)
{
    setObjectName(options.objectPrefix + QStringLiteral("Box"));
    setFrameShape(QFrame::StyledPanel);
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12); // Border Padding="12"
    root->setSpacing(8);                      // StackPanel Spacing="8"

    auto *title = new QLabel(options.title, this);
    title->setObjectName(options.objectPrefix + QStringLiteral("Title"));
    title->setProperty("axisTitle", true);
    root->addWidget(title);

    auto *grid = new QGridLayout;
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(6); // RowSpacing="6"
    grid->setColumnStretch(1, 1);
    const struct
    {
        Field field;
        const char *label;
        const char *name;
    } rows[] = {
        {Field::Range, "Range / Angle", "Range"},
        {Field::PwmRange, "PWM Range", "PwmRange"},
        {Field::Center, "Center PWM", "Center"},
        {Field::Speed, "Speed", "Speed"},
        {Field::Accel, "Acceleration", "Accel"},
    };
    int row = 0;
    for (const auto &entry : rows) {
        auto *label = new QLabel(tr(entry.label), this);
        label->setObjectName(options.objectPrefix + QLatin1String(entry.name)
                             + QStringLiteral("Label"));
        label->setProperty("fieldLabel", true);
        label->setFixedWidth(kLabelColumnWidth);
        auto *edit = new QLineEdit(this);
        edit->setObjectName(options.objectPrefix + QLatin1String(entry.name));
        m_fields[int(entry.field)] = edit;
        const Field field = entry.field;
        connect(edit, &QLineEdit::textChanged, this, [this, field](const QString &text) {
            emit fieldEdited(m_axis, field, text);
        });
        grid->addWidget(label, row, 0);
        grid->addWidget(edit, row, 1);
        ++row;
    }
    root->addLayout(grid);

    auto *trimBox = new QVBoxLayout;
    trimBox->setContentsMargins(0, 0, 0, 0);
    trimBox->setSpacing(2); // StackPanel Spacing="2"
    m_trimLabel = new QLabel(this);
    m_trimLabel->setObjectName(options.objectPrefix + QStringLiteral("TrimLabel"));
    m_trimLabel->setProperty("fieldLabel", true);
    trimBox->addWidget(m_trimLabel);
    m_trimSlider = new QSlider(Qt::Horizontal, this);
    m_trimSlider->setObjectName(options.objectPrefix + QStringLiteral("Trim"));
    m_trimSlider->setTickPosition(QSlider::TicksBelow); // TickPlacement="BottomRight"
    m_trimSlider->setTickInterval(5);                   // TickFrequency="5"
    m_trimSlider->setRange(qRound(options.trimMin), qRound(options.trimMax));
    connect(m_trimSlider, &QSlider::valueChanged, this, [this](int value) {
        updateTrimLabel();
        emit trimEdited(m_axis, value);
    });
    trimBox->addWidget(m_trimSlider);
    if (options.showCenterLabel) {
        m_centerLabel = new QLabel(QStringLiteral("0"), this);
        m_centerLabel->setObjectName(options.objectPrefix + QStringLiteral("CenterLabel"));
        m_centerLabel->setProperty("fieldLabel", true);
        m_centerLabel->setAlignment(Qt::AlignHCenter);
        trimBox->addWidget(m_centerLabel);
    }
    root->addLayout(trimBox);

    m_reverse = new QCheckBox(options.reverseText, this);
    m_reverse->setObjectName(options.objectPrefix + QStringLiteral("Reverse"));
    connect(m_reverse, &QCheckBox::toggled, this, [this](bool checked) {
        emit reverseToggled(m_axis, checked);
    });
    root->addWidget(m_reverse);
    updateTrimLabel();
}

QString AntennaTrackerAxisPanel::fieldText(Field field) const
{
    return m_fields[int(field)]->text();
}

double AntennaTrackerAxisPanel::trim() const
{
    return m_trimSlider->value();
}

bool AntennaTrackerAxisPanel::reverse() const
{
    return m_reverse->isChecked();
}

void AntennaTrackerAxisPanel::setFieldText(Field field, const QString &text)
{
    QLineEdit *edit = m_fields[int(field)];
    if (edit->text() == text) {
        return;
    }
    const QSignalBlocker blocker(edit);
    edit->setText(text);
}

void AntennaTrackerAxisPanel::setTrim(double value)
{
    const int rounded = qRound(value);
    if (m_trimSlider->value() == rounded) {
        updateTrimLabel();
        return;
    }
    const QSignalBlocker blocker(m_trimSlider);
    m_trimSlider->setValue(rounded);
    updateTrimLabel();
}

void AntennaTrackerAxisPanel::setTrimRange(double minimum, double maximum)
{
    const QSignalBlocker blocker(m_trimSlider);
    m_trimSlider->setRange(qRound(minimum), qRound(maximum));
    updateTrimLabel();
}

void AntennaTrackerAxisPanel::setReverse(bool value)
{
    if (m_reverse->isChecked() == value) {
        return;
    }
    const QSignalBlocker blocker(m_reverse);
    m_reverse->setChecked(value);
}

void AntennaTrackerAxisPanel::setControlsEnabled(bool enabled)
{
    for (Field field : {Field::Range, Field::PwmRange, Field::Center}) {
        m_fields[int(field)]->setEnabled(enabled);
    }
}

void AntennaTrackerAxisPanel::setSpeedAccelEnabled(bool enabled)
{
    m_fields[int(Field::Speed)]->setEnabled(enabled);
    m_fields[int(Field::Accel)]->setEnabled(enabled);
}

void AntennaTrackerAxisPanel::updateTrimLabel()
{
    // MP10 StringFormat='Trim: {0:0}'
    m_trimLabel->setText(tr("Trim: %1").arg(m_trimSlider->value()));
}
