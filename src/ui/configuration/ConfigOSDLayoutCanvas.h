#ifndef CONFIGOSDLAYOUTCANVAS_H
#define CONFIGOSDLAYOUTCANVAS_H

#include <QPoint>
#include <QRect>
#include <QString>
#include <QVector>
#include <QWidget>

struct ConfigOSDLayoutItem
{
    QString key;
    QString caption;
    QString name;
    bool enabled = false;
    bool clipped = false;
    int x = 0;
    int y = 0;

    bool operator==(const ConfigOSDLayoutItem &other) const
    {
        return key == other.key && caption == other.caption
            && name == other.name && enabled == other.enabled
            && clipped == other.clipped
            && x == other.x && y == other.y;
    }
};

class ConfigOSDLayoutCanvas final : public QWidget
{
    Q_OBJECT

public:
    static constexpr int GridColumns = 30;
    static constexpr int GridRows = 16;
    static constexpr int CellWidth = 26;
    static constexpr int CellHeight = 24;

    explicit ConfigOSDLayoutCanvas(QWidget *parent = nullptr);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

    QVector<ConfigOSDLayoutItem> items() const { return m_items; }
    void setItems(const QVector<ConfigOSDLayoutItem> &items);

    QString selectedKey() const { return m_selectedKey; }
    void setSelectedKey(const QString &key);

    QRect itemRect(const QString &key) const;
    qreal itemVisualOpacity(const QString &key) const;

    static QSize canvasSize();
    static bool isCellInBounds(int x, int y);
    static QPoint cellOrigin(int x, int y);
    static QRect cellRect(int x, int y);
    static QPoint cellForPosition(const QPoint &position);
    static QPoint clampedCellForPosition(const QPoint &position);
    static qreal visualOpacity(bool enabled);

signals:
    void selectionChanged(const QString &key);
    void positionEdited(const QString &key, int x, int y);

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    int indexForKey(const QString &key) const;
    int itemIndexAt(const QPoint &position) const;
    QRect itemRectAt(int index) const;
    QString displayCaption(const ConfigOSDLayoutItem &item) const;
    void updateDragPosition(const QPoint &pointerPosition);
    void cancelDrag();

    QVector<ConfigOSDLayoutItem> m_items;
    QString m_selectedKey;
    int m_dragIndex = -1;
    QPoint m_dragPointerOffset;
    QPoint m_dragOriginalCell;
};

#endif // CONFIGOSDLAYOUTCANVAS_H
