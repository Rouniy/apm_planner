#ifndef CONFIGDEVELOPERTOOLSVIEW_H
#define CONFIGDEVELOPERTOOLSVIEW_H

#include "ActionPageView.h"

/** Mission Planner 10 Developer Tools action page. */
class ConfigDeveloperToolsView final : public ActionPageView
{
    Q_OBJECT

public:
    explicit ConfigDeveloperToolsView(QWidget *parent = nullptr);

    int ImplementedActionCount() const;

public slots:
    void DecodeMavlinkInput(const QString &input);
    void DecodeHardwareIdInput(const QString &input,
                               const QString &parameterName = QString());

private:
    void DecodePacket();
    void DecodeHardwareId();

    int m_implementedActionCount = 0;
};

#endif // CONFIGDEVELOPERTOOLSVIEW_H
