#ifndef DOCKABLEVIEW_H
#define DOCKABLEVIEW_H

#include <QByteArray>
#include <QList>
#include <QSize>
#include <QStringList>
#include <QWidget>

#include <memory>

class QAction;

// A deterministic, non-floating panel surface. Panels are composed from
// QWidget, QSplitter and QTabWidget internally; the API deliberately retains
// the historic docking terminology so callers do not depend on layout chrome.
class DockableView : public QWidget
{
    Q_OBJECT

public:
    enum class PanelLocation
    {
        Left,
        Right,
        Top,
        Bottom,
        Tabbed
    };
    Q_ENUM(PanelLocation)

    explicit DockableView(const QString &viewId, QWidget *parent = nullptr);
    ~DockableView() override;

    QString viewId() const;
    QStringList panelIds() const;
    bool hasPanel(const QString &panelId) const;

    bool addPanel(const QString &panelId,
                  const QString &title,
                  QWidget *content,
                  PanelLocation location,
                  const QString &relativeToPanelId = QString(),
                  const QSize &preferredSize = QSize(),
                  bool requiredDocked = false);
    QAction *panelToggleAction(const QString &panelId) const;
    bool isPanelOpen(const QString &panelId) const;
    bool isPanelDocked(const QString &panelId) const;
    bool setPanelVisible(const QString &panelId, bool visible);
    bool setPanelSizeWeights(const QStringList &panelIds,
                             const QList<int> &weights,
                             Qt::Orientation orientation);
    bool setPanelFixedExtent(const QString &panelId,
                             int extent,
                             Qt::Orientation orientation);

    QByteArray saveLayout() const;
    bool restoreLayout(const QByteArray &layout);

signals:
    void layoutRestoreRejected(const QString &reason);

private:
    class Private;
    std::unique_ptr<Private> d;
};

#endif
