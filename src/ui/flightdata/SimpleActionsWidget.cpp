#include "SimpleActionsWidget.h"

#include <QColor>
#include <QFont>
#include <QGridLayout>
#include <QLabel>
#include <QPalette>
#include <QPushButton>
#include <QSizePolicy>
#include <QVBoxLayout>

SimpleActionsWidget::SimpleActionsWidget(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("SimpleActions"));
    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(16, 16, 16, 16);
    rootLayout->setSpacing(10);

    auto *actionsLayout = new QGridLayout;
    actionsLayout->setContentsMargins(0, 0, 0, 0);
    actionsLayout->setHorizontalSpacing(12);
    const struct {
        const char *text;
        const char *mode;
        const char *objectName;
        const char *toolTip;
    } definitions[] = {
        {"LOITER", "Loiter", "SimpleLoiterButton",
         QT_TR_NOOP("Hold position / enter Loiter mode")},
        {"RTL", "RTL", "SimpleRtlButton",
         QT_TR_NOOP("Return to launch")},
        {"AUTO", "Auto", "SimpleAutoButton",
         QT_TR_NOOP("Continue the uploaded mission in Auto mode")}
    };

    for (int column = 0; column < 3; ++column) {
        const auto &definition = definitions[column];
        auto *button = new QPushButton(tr(definition.text), this);
        button->setObjectName(QString::fromLatin1(definition.objectName));
        button->setToolTip(tr(definition.toolTip));
        button->setMinimumHeight(54);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        QFont font = button->font();
        font.setPixelSize(22);
        font.setBold(true);
        button->setFont(font);
        actionsLayout->addWidget(button, 0, column);
        actionsLayout->setColumnStretch(column, 1);
        connect(button, &QPushButton::clicked, this,
                [this, mode = QString::fromLatin1(definition.mode)]() {
                    emit quickModeRequested(mode);
                });
        m_buttons.append(button);
    }
    rootLayout->addLayout(actionsLayout, 1);

    m_status = new QLabel(tr("No active vehicle"), this);
    m_status->setObjectName(QStringLiteral("SimpleActionStatus"));
    m_status->setWordWrap(true);
    rootLayout->addWidget(m_status);
    setActionsAvailable(false);
}

bool SimpleActionsWidget::actionsAvailable() const
{
    return m_actionsAvailable;
}

void SimpleActionsWidget::setActionsAvailable(bool available)
{
    m_actionsAvailable = available;
    for (QPushButton *button : m_buttons) {
        button->setEnabled(available);
    }
    if (!available && m_status->text().isEmpty()) {
        setActionStatus(tr("No active vehicle"), true);
    }
}

void SimpleActionsWidget::setActionStatus(const QString &message, bool error)
{
    m_status->setText(message);
    QPalette palette = m_status->palette();
    palette.setColor(QPalette::WindowText,
                     error ? QColor(QStringLiteral("#FCA5A5"))
                           : QColor(QStringLiteral("#34D399")));
    m_status->setPalette(palette);
}
