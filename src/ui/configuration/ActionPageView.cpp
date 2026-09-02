#include "ActionPageView.h"

#include <QFont>
#include <QGridLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSizePolicy>
#include <QVBoxLayout>

#include <utility>

ActionPageView::ActionPageView(const QString &title,
                               const QString &instructions,
                               QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("ActionPageView"));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(10);

    m_title = new QLabel(title, this);
    m_title->setObjectName(QStringLiteral("Title"));
    QFont titleFont = m_title->font();
    titleFont.setPointSize(titleFont.pointSize() + 5);
    titleFont.setBold(true);
    m_title->setFont(titleFont);
    layout->addWidget(m_title);

    m_instructions = new QLabel(instructions, this);
    m_instructions->setObjectName(QStringLiteral("Instructions"));
    m_instructions->setWordWrap(true);
    m_instructions->setMaximumWidth(760);
    layout->addWidget(m_instructions);

    m_actionScroll = new QScrollArea(this);
    m_actionScroll->setObjectName(QStringLiteral("ActionItemsScroll"));
    m_actionScroll->setFrameShape(QFrame::NoFrame);
    m_actionScroll->setWidgetResizable(true);
    m_actionScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_actionScroll->setMinimumHeight(120);
    m_actionScroll->setMaximumHeight(340);

    m_actionHost = new QWidget(m_actionScroll);
    m_actionHost->setObjectName(QStringLiteral("ActionItemsPanel"));
    m_actionLayout = new QGridLayout(m_actionHost);
    m_actionLayout->setContentsMargins(0, 0, 0, 0);
    m_actionLayout->setHorizontalSpacing(8);
    m_actionLayout->setVerticalSpacing(8);
    m_actionLayout->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_actionScroll->setWidget(m_actionHost);
    layout->addWidget(m_actionScroll);

    m_log = new QPlainTextEdit(this);
    m_log->setObjectName(QStringLiteral("Log"));
    m_log->setReadOnly(true);
    m_log->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_log->setPlaceholderText(tr("Tool results and diagnostics appear here."));
    m_log->setStyleSheet(QStringLiteral(
        "QPlainTextEdit#Log { background: #000000; color: #34D399; "
        "border: 1px solid #121614; font-family: monospace; }"));
    layout->addWidget(m_log, 1);
}

QPushButton *ActionPageView::AddAction(
    const QString &label, const QString &objectName,
    ActionCallback callback, bool enabled,
    const QString &unavailableReason)
{
    auto *button = new QPushButton(label, m_actionHost);
    button->setObjectName(objectName);
    button->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
    button->setEnabled(enabled);
    button->setToolTip(enabled ? QString() : unavailableReason);

    m_actions.append(button);
    ++m_actionCount;
    reflowActions(m_actionHost->contentsRect().width(), true);

    if (callback) {
        connect(button, &QPushButton::clicked, this,
                [callback = std::move(callback)]() { callback(); });
    }
    return button;
}

QPushButton *ActionPageView::AddUnavailableAction(
    const QString &label, const QString &objectName,
    const QString &reason)
{
    return AddAction(label, objectName, ActionCallback(), false, reason);
}

QString ActionPageView::Title() const
{
    return m_title->text();
}

QString ActionPageView::Instructions() const
{
    return m_instructions->text();
}

QString ActionPageView::Log() const
{
    return m_log->toPlainText();
}

int ActionPageView::ActionCount() const
{
    return m_actionCount;
}

int ActionPageView::ColumnCount() const
{
    return m_columnCount;
}

void ActionPageView::AppendLog(const QString &line)
{
    if (!line.isEmpty()) {
        m_log->appendPlainText(line);
    }
}

void ActionPageView::ClearLog()
{
    m_log->clear();
}

void ActionPageView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    // The child layout is updated after this event, so use the page width
    // directly instead of the action host's previous geometry.
    reflowActions(event->size().width() - 32);
}

void ActionPageView::reflowActions(int availableWidth, bool force)
{
    constexpr int maximumColumns = 4;
    constexpr int minimumButtonWidth = 170;
    const int spacing = m_actionLayout->horizontalSpacing();

    int preferredButtonWidth = minimumButtonWidth;
    for (QPushButton *button : qAsConst(m_actions)) {
        preferredButtonWidth = qMax(preferredButtonWidth,
                                    button->sizeHint().width());
    }

    const int usableWidth = qMax(1, availableWidth);
    const int columns = qBound(
        1, (usableWidth + spacing) / (preferredButtonWidth + spacing),
        maximumColumns);
    if (!force && columns == m_columnCount) {
        return;
    }

    m_columnCount = columns;
    for (int column = 0; column < maximumColumns; ++column) {
        m_actionLayout->setColumnStretch(column,
                                         column < columns ? 1 : 0);
    }
    for (int index = 0; index < m_actions.size(); ++index) {
        QPushButton *button = m_actions.at(index);
        m_actionLayout->removeWidget(button);
        m_actionLayout->addWidget(button, index / columns, index % columns);
    }
    m_actionHost->updateGeometry();
}
