#ifndef DOCKHOST_H
#define DOCKHOST_H

#include <QByteArray>
#include <QStringList>
#include <QWidget>

#include <memory>

class QAction;

class DockHost final : public QWidget
{
    Q_OBJECT

public:
    enum class Location
    {
        Left,
        Right,
        Top,
        Bottom,
        Tabbed
    };
    Q_ENUM(Location)

    explicit DockHost(const QString &viewId, QWidget *parent = nullptr);
    ~DockHost() override;

    QString viewId() const;
    QString affinity() const;
    QStringList dockIds() const;

    bool addDock(const QString &dockId,
                 const QString &title,
                 QWidget *content,
                 Location location,
                 const QString &relativeToDockId = QString(),
                 const QSize &preferredSize = QSize());
    QAction *toggleAction(const QString &dockId) const;
    bool setDockVisible(const QString &dockId, bool visible);

    QByteArray saveLayout() const;
    bool restoreLayout(const QByteArray &envelope);

    static int layoutSchemaVersion();

signals:
    void layoutRestoreRejected(const QString &reason);

private:
    class Private;
    std::unique_ptr<Private> d;
};

#endif
