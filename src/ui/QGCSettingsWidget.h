#ifndef QGCSETTINGSWIDGET_H
#define QGCSETTINGSWIDGET_H

#include "UASInterface.h"
#include <QPointer>
#include <QWidget>

namespace Ui
{
class QGCSettingsWidget;
}

class QGCSettingsWidget : public QWidget
{
    Q_OBJECT

public:
    explicit QGCSettingsWidget(QWidget *parent = nullptr);
    ~QGCSettingsWidget() override;

protected:
    void showEvent(QShowEvent *evt) override;

private slots:
    void setLogDir();
    void setMAVLinkLogDir();
    void setParamDir();
    void setAppDataDir();
    void setMissionsDir();
    void ratesChanged();
    void setBetaRelease(bool state);
    void setHideDonateButton(bool state);

    void setActiveUAS(UASInterface *uas);

    void mavIdChanged(int id);

    void componentIdChanged(int id);

private:
    void setDataRateLineEdits();

private:
    Ui::QGCSettingsWidget *ui;
    bool m_init = false;
    QPointer<UASInterface> m_uas;
};

#endif // QGCSETTINGSWIDGET_H
