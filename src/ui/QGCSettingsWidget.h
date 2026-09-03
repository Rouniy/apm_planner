#ifndef QGCSETTINGSWIDGET_H
#define QGCSETTINGSWIDGET_H

#include "UASInterface.h"
#include <QPointer>
#include <QWidget>

class QComboBox;
class QCheckBox;
class QLabel;
class QSpinBox;

namespace Ui
{
class QGCSettingsWidget;
}

class QGCSettingsWidget : public QWidget
{
    Q_OBJECT

public:
    enum class SurfaceMode
    {
        Full,
        LegacyPlanner
    };

    explicit QGCSettingsWidget(
        QWidget *parent = nullptr,
        SurfaceMode mode = SurfaceMode::Full);
    ~QGCSettingsWidget() override;

    void selectLegacyTelemetryRates();

signals:
    void altitudeUnitsChanged(const QString &units);
    void distanceUnitsChanged(const QString &units);

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
    void mapWidgetBackendChanged(int index);
    void altitudeUnitsIndexChanged(int index);
    void distanceUnitsIndexChanged(int index);
    void displayLayoutIndexChanged(int index);

private:
    void setDataRateLineEdits();
    void populateMapWidgetBackends();
    void syncDisplayLayout();
    void saveStartupUdpOptions();

private:
    Ui::QGCSettingsWidget *ui;
    bool m_init = false;
    QPointer<UASInterface> m_uas;
    QComboBox *m_mapWidgetBackendComboBox = nullptr;
    QLabel *m_mapWidgetBackendStatus = nullptr;
    QComboBox *m_altitudeUnitsComboBox = nullptr;
    QComboBox *m_distanceUnitsComboBox = nullptr;
    QComboBox *m_displayLayoutComboBox = nullptr;
    QLabel *m_displayLayoutLabel = nullptr;
    QLabel *m_displayLayoutStatus = nullptr;
    QCheckBox *m_startupUdpEnabled = nullptr;
    QSpinBox *m_startupUdpPrimaryPort = nullptr;
    QSpinBox *m_startupUdpAlternatePort = nullptr;
    QLabel *m_startupUdpStatus = nullptr;
};

#endif // QGCSETTINGSWIDGET_H
