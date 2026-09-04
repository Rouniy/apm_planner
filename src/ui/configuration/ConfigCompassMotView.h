#ifndef CONFIGCOMPASSMOTVIEW_H
#define CONFIGCOMPASSMOTVIEW_H

#include "comm/VehicleEndpoint.h"

#include <QMetaObject>
#include <QPointer>
#include <QTimer>
#include <QWidget>

class CompassCalibrationService;
class QCheckBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QCustomPlot;

class ConfigCompassMotView final : public QWidget
{
    Q_OBJECT

public:
    explicit ConfigCompassMotView(QWidget *parent = nullptr);
    ~ConfigCompassMotView() override;

    QSize sizeHint() const override;
    void setCalibrationContext(CompassCalibrationService *service,
                               const VehicleTargetLease &target);
    void setConnected(bool connected);
    void setArmed(bool armed);
    void setSupportedVehicle(bool supported,
                             const QString &reason = QString());
    void setParameterSnapshotReady(bool ready);

    static QString SafetyConfirmationTitle();
    static QString SafetyConfirmationText();

public slots:
    void deactivate();

signals:
    void calibrationFinished(const VehicleTargetLease &target);

private:
    void buildUi();
    void startCalibration();
    void stopCalibration();
    void clearUnsafeSession();
    void syncState();
    void syncPlot();
    bool serviceOwnsTarget() const;
    bool baseReady() const;
    static QString stateText(int state);

    QPointer<CompassCalibrationService> m_service;
    VehicleTargetLease m_target;
    QMetaObject::Connection m_changedConnection;
    QMetaObject::Connection m_destroyedConnection;
    QCheckBox *m_safetyCheck = nullptr;
    QPushButton *m_start = nullptr;
    QPushButton *m_finish = nullptr;
    QPushButton *m_clearUnsafe = nullptr;
    QLabel *m_targetStatus = nullptr;
    QLabel *m_stateStatus = nullptr;
    QLabel *m_throttle = nullptr;
    QLabel *m_current = nullptr;
    QLabel *m_interference = nullptr;
    QLabel *m_compensation = nullptr;
    QCustomPlot *m_plot = nullptr;
    QPlainTextEdit *m_log = nullptr;
    bool m_connected = false;
    bool m_armed = false;
    bool m_supportedVehicle = false;
    bool m_parameterSnapshotReady = false;
    QString m_unsupportedReason;
    bool m_terminalNotified = false;
    QTimer m_freshnessTimer;
};

#endif // CONFIGCOMPASSMOTVIEW_H
