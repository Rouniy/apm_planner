#ifndef GPSCORRECTIONSOURCE_H
#define GPSCORRECTIONSOURCE_H

#include "Rtcm3Parser.h"

#include <QByteArray>
#include <QObject>
#include <QStringList>

class QIODevice;
class QSerialPort;
class QTcpSocket;
class QTimer;

struct GpsCorrectionSourceSettings
{
    QString selectedPort = QStringLiteral("NTRIP");
    int baudRate = 115200;
    QString host;
    int casterPort = 2101;
    QString mountPoint;
    QString username;
    QString password;
    bool ntripV1 = false;
    bool sendGga = true;

    bool isNtrip() const
    {
        return selectedPort == QLatin1String("NTRIP");
    }
    QString validationError() const;
};

class GpsCorrectionSource : public QObject
{
    Q_OBJECT

public:
    explicit GpsCorrectionSource(QObject *parent = nullptr)
        : QObject(parent)
    {
    }
    ~GpsCorrectionSource() override = default;

    virtual QStringList availablePorts() const = 0;
    virtual bool active() const = 0;
    virtual bool connected() const = 0;
    virtual bool start(const GpsCorrectionSourceSettings &settings) = 0;
    virtual void stop() = 0;
    virtual void setGgaPosition(double latitude, double longitude,
                                double altitudeMsl, bool valid) = 0;

    /**
     * Identifies the currently open, application-owned serial receiver.
     * Zero means that no writable receiver session exists.  The identifier
     * changes on every successful serial open and is never reused for an
     * automatic reconnect/replacement.
     */
    virtual quint64 receiverSession() const noexcept;
    virtual bool canConfigureReceiver() const noexcept;
    virtual int receiverBaudRate() const noexcept;
    virtual bool setReceiverBaudRate(int baudRate,
                                     quint64 expectedSession,
                                     QString *error = nullptr);

    /**
     * Queues bytes on the already-open serial receiver only when its exact
     * session still matches.  NTRIP and stale/replaced serial sessions reject
     * writes.  Existing subclasses remain read-only unless they override it.
     */
    virtual bool writeReceiverData(const QByteArray &bytes,
                                   quint64 expectedSession,
                                   QString *error = nullptr);

signals:
    void stateChanged(bool active, bool connected);
    void statusChanged(const QString &status);
    void inputBytes(qint64 bytes);
    void rtcmFrame(const QByteArray &frame, quint16 messageId);
    void invalidRtcmFrame(quint16 messageId);
    void receiverBytes(const QByteArray &bytes, quint64 receiverSession);
    void plaintextCredentialsWarning();
};

class QtGpsCorrectionSource final : public GpsCorrectionSource
{
    Q_OBJECT

public:
    explicit QtGpsCorrectionSource(QObject *parent = nullptr);
    ~QtGpsCorrectionSource() override;

    QStringList availablePorts() const override;
    bool active() const override { return m_active; }
    bool connected() const override { return m_connected; }
    bool start(const GpsCorrectionSourceSettings &settings) override;
    void stop() override;
    void setGgaPosition(double latitude, double longitude,
                        double altitudeMsl, bool valid) override;
    quint64 receiverSession() const noexcept override;
    bool canConfigureReceiver() const noexcept override;
    int receiverBaudRate() const noexcept override;
    bool setReceiverBaudRate(int baudRate,
                             quint64 expectedSession,
                             QString *error = nullptr) override;
    bool writeReceiverData(const QByteArray &bytes,
                           quint64 expectedSession,
                           QString *error = nullptr) override;

    static QByteArray buildNtripRequest(
        const GpsCorrectionSourceSettings &settings,
        bool *credentialsInClear = nullptr);
    static int parseNtripStatusCode(const QByteArray &statusLine);
    static QByteArray makeGga(double latitude, double longitude,
                              double altitudeMsl);

    // Public test seam and reusable stream ingestion entry point.
    void ingestBytes(const QByteArray &bytes);

private slots:
    void readAvailable();
    void handleNtripConnected();
    void handleSocketDisconnected();
    void sendGga();

private:
    void startSerial();
    void startNtrip();
    void handleNtripResponse(const QByteArray &bytes);
    void completeNtripHandshake(const QByteArray &trailingData);
    void processNtripPayload(const QByteArray &bytes);
    void setState(bool active, bool connected);
    void fail(const QString &status);
    void closeDevice();
    QString normalizedHost() const;
    int normalizedPort() const;
    bool useTls() const;

    GpsCorrectionSourceSettings m_settings;
    Rtcm3Parser m_parser;
    QIODevice *m_device = nullptr;
    QSerialPort *m_serialPort = nullptr;
    QTcpSocket *m_socket = nullptr;
    QTimer *m_connectTimer = nullptr;
    QTimer *m_watchdogTimer = nullptr;
    QTimer *m_ggaTimer = nullptr;
    QByteArray m_responseBuffer;
    QByteArray m_chunkBuffer;
    qint64 m_chunkBytesRemaining = -1;
    double m_ggaLatitude = 0.0;
    double m_ggaLongitude = 0.0;
    double m_ggaAltitude = 0.0;
    bool m_ggaValid = false;
    bool m_active = false;
    bool m_connected = false;
    bool m_handshakeComplete = false;
    bool m_chunkedTransfer = false;
    bool m_chunkedFinished = false;
    bool m_closing = false;
    quint64 m_receiverSession = 0;
    quint64 m_nextReceiverSession = 1;
};

#endif // GPSCORRECTIONSOURCE_H
