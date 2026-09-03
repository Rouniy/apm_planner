#ifndef MAVLINKINSPECTORWINDOW_H
#define MAVLINKINSPECTORWINDOW_H

#include <QPointer>
#include <QWidget>

/** Modeless top-level host matching Mission Planner's MAVLinkInspectorWindow. */
class MAVLinkInspectorWindow final : public QWidget
{
    Q_OBJECT

public:
    explicit MAVLinkInspectorWindow(QWidget *inspectorView,
                                    QWidget *owner = nullptr);

    QWidget *inspectorView() const;

private:
    QPointer<QWidget> m_inspectorView;
};

#endif // MAVLINKINSPECTORWINDOW_H
