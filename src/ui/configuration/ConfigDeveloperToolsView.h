#ifndef CONFIGDEVELOPERTOOLSVIEW_H
#define CONFIGDEVELOPERTOOLSVIEW_H

#include "ActionPageView.h"

#include <QPointer>

class QObject;
class QPushButton;

/** Mission Planner 10 Developer Tools action page. */
class ConfigDeveloperToolsView final : public ActionPageView
{
    Q_OBJECT

public:
    explicit ConfigDeveloperToolsView(QObject *actionSource = nullptr,
                                      QWidget *parent = nullptr);

    int ImplementedActionCount() const;

public slots:
    void DecodeMavlinkInput(const QString &input);
    void DecodeHardwareIdInput(const QString &input,
                               const QString &parameterName = QString());

private:
    QPushButton *AddToolAction(const QString &label,
                               const QString &buttonObjectName,
                               const QString &actionObjectName);
    void DecodePacket();
    void DecodeHardwareId();

    QPointer<QObject> m_actionSource;
    int m_implementedActionCount = 0;
};

#endif // CONFIGDEVELOPERTOOLSVIEW_H
