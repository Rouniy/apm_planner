#include "ConfigOSDLayoutCanvas.h"

#include <QFontDatabase>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPointer>
#include <QPolygon>
#include <QSet>
#include <QSizePolicy>

#include <algorithm>

namespace {

const QColor kBackground(QStringLiteral("#0d1210"));
const QColor kGrid(QStringLiteral("#39433e"));
const QColor kItemText(QStringLiteral("#e5ece8"));
const QColor kSelection(QStringLiteral("#ffd600"));
const QColor kClipped(QStringLiteral("#ff9f43"));

constexpr qreal kDisabledOpacity = 0.35;

} // namespace

ConfigOSDLayoutCanvas::ConfigOSDLayoutCanvas(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("ConfigOSDLayoutCanvas"));
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setMinimumSize(canvasSize());
    setFixedSize(canvasSize());
    setMouseTracking(true);
    setAutoFillBackground(false);

    QFont canvasFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    canvasFont.setPixelSize(14);
    setFont(canvasFont);
}

QSize ConfigOSDLayoutCanvas::sizeHint() const
{
    return canvasSize();
}

QSize ConfigOSDLayoutCanvas::minimumSizeHint() const
{
    return canvasSize();
}

void ConfigOSDLayoutCanvas::setItems(
    const QVector<ConfigOSDLayoutItem> &items)
{
    cancelDrag();

    QVector<ConfigOSDLayoutItem> normalized;
    normalized.reserve(items.size());
    QSet<QString> keys;
    for (ConfigOSDLayoutItem item : items) {
        if (item.key.isEmpty() || keys.contains(item.key)) {
            continue;
        }
        keys.insert(item.key);
        item.clipped = !isCellInBounds(item.x, item.y);
        item.x = std::clamp(item.x, 0, GridColumns - 1);
        item.y = std::clamp(item.y, 0, GridRows - 1);
        normalized.append(item);
    }

    m_items = normalized;
    if (!m_selectedKey.isEmpty() && indexForKey(m_selectedKey) < 0) {
        m_selectedKey.clear();
        const QPointer<ConfigOSDLayoutCanvas> guard(this);
        emit selectionChanged(QString());
        if (!guard) {
            return;
        }
    }
    update();
}

void ConfigOSDLayoutCanvas::setSelectedKey(const QString &key)
{
    const QString next = key.isEmpty() || indexForKey(key) >= 0
        ? key : QString();
    if (m_selectedKey == next) {
        return;
    }
    m_selectedKey = next;
    const QPointer<ConfigOSDLayoutCanvas> guard(this);
    emit selectionChanged(m_selectedKey);
    if (!guard) {
        return;
    }
    update();
}

QRect ConfigOSDLayoutCanvas::itemRect(const QString &key) const
{
    return itemRectAt(indexForKey(key));
}

qreal ConfigOSDLayoutCanvas::itemVisualOpacity(const QString &key) const
{
    const int index = indexForKey(key);
    return index >= 0 ? visualOpacity(m_items.at(index).enabled) : -1.0;
}

QSize ConfigOSDLayoutCanvas::canvasSize()
{
    return QSize(GridColumns * CellWidth, GridRows * CellHeight);
}

bool ConfigOSDLayoutCanvas::isCellInBounds(int x, int y)
{
    return x >= 0 && x < GridColumns && y >= 0 && y < GridRows;
}

QPoint ConfigOSDLayoutCanvas::cellOrigin(int x, int y)
{
    if (!isCellInBounds(x, y)) {
        return QPoint(-1, -1);
    }
    return QPoint(x * CellWidth, y * CellHeight);
}

QRect ConfigOSDLayoutCanvas::cellRect(int x, int y)
{
    if (!isCellInBounds(x, y)) {
        return QRect();
    }
    return QRect(cellOrigin(x, y), QSize(CellWidth, CellHeight));
}

QPoint ConfigOSDLayoutCanvas::cellForPosition(const QPoint &position)
{
    if (position.x() < 0 || position.y() < 0
        || position.x() >= canvasSize().width()
        || position.y() >= canvasSize().height()) {
        return QPoint(-1, -1);
    }
    return QPoint(position.x() / CellWidth, position.y() / CellHeight);
}

QPoint ConfigOSDLayoutCanvas::clampedCellForPosition(
    const QPoint &position)
{
    const int boundedX = std::clamp(
        position.x(), 0, canvasSize().width() - 1);
    const int boundedY = std::clamp(
        position.y(), 0, canvasSize().height() - 1);
    return QPoint(boundedX / CellWidth, boundedY / CellHeight);
}

qreal ConfigOSDLayoutCanvas::visualOpacity(bool enabled)
{
    return enabled ? 1.0 : kDisabledOpacity;
}

void ConfigOSDLayoutCanvas::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }

    const int index = itemIndexAt(event->pos());
    if (index < 0) {
        cancelDrag();
        setSelectedKey(QString());
        event->accept();
        return;
    }

    const QString key = m_items.at(index).key;
    const QPointer<ConfigOSDLayoutCanvas> guard(this);
    setSelectedKey(key);
    if (!guard) {
        return;
    }
    const int currentIndex = indexForKey(key);
    if (currentIndex < 0 || m_selectedKey != key) {
        cancelDrag();
        event->accept();
        return;
    }
    m_dragIndex = currentIndex;
    m_dragOriginalCell = QPoint(m_items.at(currentIndex).x,
                                m_items.at(currentIndex).y);
    m_dragPointerOffset = event->pos() - cellOrigin(
        m_items.at(currentIndex).x, m_items.at(currentIndex).y);
    event->accept();
}

void ConfigOSDLayoutCanvas::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragIndex < 0) {
        QWidget::mouseMoveEvent(event);
        return;
    }

    updateDragPosition(event->pos());
    event->accept();
}

void ConfigOSDLayoutCanvas::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || m_dragIndex < 0) {
        QWidget::mouseReleaseEvent(event);
        return;
    }

    updateDragPosition(event->pos());
    const ConfigOSDLayoutItem edited = m_items.at(m_dragIndex);
    const bool changed = QPoint(edited.x, edited.y) != m_dragOriginalCell;
    cancelDrag();
    if (changed) {
        emit positionEdited(edited.key, edited.x, edited.y);
    }
    event->accept();
}

void ConfigOSDLayoutCanvas::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)

    QPainter painter(this);
    painter.fillRect(rect(), kBackground);

    painter.setPen(QPen(kGrid, 1));
    for (int column = 0; column <= GridColumns; ++column) {
        const int x = column * CellWidth;
        painter.drawLine(x, 0, x, canvasSize().height());
    }
    for (int row = 0; row <= GridRows; ++row) {
        const int y = row * CellHeight;
        painter.drawLine(0, y, canvasSize().width(), y);
    }

    painter.setFont(font());
    for (int index = 0; index < m_items.size(); ++index) {
        const ConfigOSDLayoutItem &item = m_items.at(index);
        const QRect bounds = itemRectAt(index);

        painter.save();
        painter.setOpacity(visualOpacity(item.enabled));
        painter.setPen(kItemText);
        painter.drawText(bounds.adjusted(3, 0, -3, 0),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         displayCaption(item));
        painter.restore();

        if (item.key == m_selectedKey) {
            painter.save();
            painter.setOpacity(1.0);
            painter.setPen(QPen(kSelection, 2));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(bounds.adjusted(1, 1, -2, -2));
            painter.restore();
        }
        if (item.clipped) {
            painter.save();
            painter.setPen(Qt::NoPen);
            painter.setBrush(kClipped);
            const QPoint topRight = bounds.topRight();
            painter.drawPolygon(QPolygon({
                topRight + QPoint(-9, 1),
                topRight + QPoint(-1, 1),
                topRight + QPoint(-1, 9)
            }));
            painter.restore();
        }
    }
}

int ConfigOSDLayoutCanvas::indexForKey(const QString &key) const
{
    for (int index = 0; index < m_items.size(); ++index) {
        if (m_items.at(index).key == key) {
            return index;
        }
    }
    return -1;
}

int ConfigOSDLayoutCanvas::itemIndexAt(const QPoint &position) const
{
    for (int index = m_items.size() - 1; index >= 0; --index) {
        if (itemRectAt(index).contains(position)) {
            return index;
        }
    }
    return -1;
}

QRect ConfigOSDLayoutCanvas::itemRectAt(int index) const
{
    if (index < 0 || index >= m_items.size()) {
        return QRect();
    }

    const ConfigOSDLayoutItem &item = m_items.at(index);
    const QPoint origin = cellOrigin(item.x, item.y);
    const QFontMetrics metrics(font());
    const int width = std::max(
        CellWidth, metrics.horizontalAdvance(displayCaption(item)) + 6);
    return QRect(origin, QSize(width, CellHeight));
}

QString ConfigOSDLayoutCanvas::displayCaption(
    const ConfigOSDLayoutItem &item) const
{
    return item.caption.isEmpty() ? item.name : item.caption;
}

void ConfigOSDLayoutCanvas::updateDragPosition(
    const QPoint &pointerPosition)
{
    if (m_dragIndex < 0 || m_dragIndex >= m_items.size()) {
        return;
    }

    const QPoint cell = clampedCellForPosition(
        pointerPosition - m_dragPointerOffset);
    ConfigOSDLayoutItem &item = m_items[m_dragIndex];
    if (item.x == cell.x() && item.y == cell.y()) {
        return;
    }
    item.x = cell.x();
    item.y = cell.y();
    update();
}

void ConfigOSDLayoutCanvas::cancelDrag()
{
    m_dragIndex = -1;
    m_dragPointerOffset = QPoint();
    m_dragOriginalCell = QPoint();
}
