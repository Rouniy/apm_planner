#ifndef CONFIGADVANCEDVIEW_H
#define CONFIGADVANCEDVIEW_H

#include "ActionPageView.h"

#include <QPointer>

class QObject;
class QPushButton;

/** Mission Planner 10 SETUP -> Advanced -> Advanced Tools. */
class ConfigAdvancedView final : public ActionPageView
{
    Q_OBJECT

public:
    explicit ConfigAdvancedView(QObject *actionSource,
                                QWidget *parent = nullptr);

    int ImplementedActionCount() const;

private:
    QPushButton *AddToolAction(const QString &label,
                               const QString &buttonObjectName,
                               const QString &actionObjectName);

    QPointer<QObject> m_actionSource;
    int m_implementedActionCount = 0;
};

#endif // CONFIGADVANCEDVIEW_H
