#ifndef JOYSTICKDEVICE_H
#define JOYSTICKDEVICE_H

#include "JoystickConfiguration.h"
#include <QObject>
#include <QElapsedTimer>
#include <QVector>
class QTimer;
struct _SDL_Joystick;

// Owner-thread SDL2 input reader only. No dependency on the legacy transmitter.
class JoystickDevice final : public QObject
{
    Q_OBJECT
public:
    struct Info {
        qint32 instanceId = -1;
        QString id, name;
        int axes = 0, buttons = 0, hats = 0;
    };
    struct Snapshot {
        bool connected = false;
        qint32 instanceId = -1;
        quint64 generation = 0;
        qint64 monotonicMs = 0;
        QVector<qint16> rawAxes;
        QVector<quint16> axes;
        QVector<bool> buttons;
        QVector<quint8> hats; // SDL bitmask: up1/right2/down4/left8.
    };
    struct CalibrationResult { int calibratedAxes = 0, ignoredAxes = 0; };
    explicit JoystickDevice(QObject *parent = nullptr);
    ~JoystickDevice() override;
    QVector<Info> devices() const { return m_devices; }
    Info selectedDevice() const { return m_selected; }
    Snapshot snapshot() const { return m_snapshot; }
    QString lastError() const { return m_error; }
    bool isOpen() const { return m_snapshot.connected; }
    bool isCalibrating() const { return m_calibrating; }
    JoystickConfiguration::Ranges ranges() const { return m_ranges; }
    bool selectDevice(qint32 instanceId, QString *error = nullptr);
    bool setRanges(const JoystickConfiguration::Ranges &, QString *error = nullptr);
    bool beginRangeCalibration();
    CalibrationResult finishRangeCalibration();
public slots:
    void refresh();
    void poll();
    void clearSelection();
    void cancelRangeCalibration();
    void resetRangeCalibration();
signals:
    void devicesChanged();
    void selectionChanged();
    void snapshotChanged(const JoystickDevice::Snapshot &snapshot);
    void disconnected(const QString &reason);
    void calibrationChanged();
private:
    bool ownerThread() const;
    void closeHandle();
    void rebuildNormalized();
    _SDL_Joystick *m_handle = nullptr;
    QTimer *m_timer = nullptr;
    bool m_sdlOwned = false, m_calibrating = false, m_polling = false;
    quint64 m_generation = 0;
    QVector<Info> m_devices;
    Info m_selected;
    Snapshot m_snapshot;
    JoystickConfiguration::Ranges m_ranges, m_observed;
    QString m_error;
    QElapsedTimer m_clock;
    qint64 m_lastRefresh = 0;
};
Q_DECLARE_METATYPE(JoystickDevice::Snapshot)

#endif
