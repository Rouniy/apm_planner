#ifndef FOLLOWMEGPSINPUT_H
#define FOLLOWMEGPSINPUT_H

#include "NmeaGgaParser.h"
#include "NmeaLineFramer.h"

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QStringList>

#include <functional>

/** Injectable byte source used to test Follow Me without a physical GPS. */
class FollowMeGpsByteSource : public QObject
{
    Q_OBJECT

public:
    explicit FollowMeGpsByteSource(QObject *parent = nullptr)
        : QObject(parent)
    {
    }
    ~FollowMeGpsByteSource() override = default;

    virtual bool open(const QString &portName, int baud,
                      QString *error) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;

signals:
    void bytesReceived(const QByteArray &bytes);
    void resourceError(const QString &error);
};

/**
 * Event-driven, bounded serial NMEA input for Mission Planner Follow Me.
 * A line may contain at most 4096 bytes.  An oversized line is rejected once
 * and discarded through its newline so its tail can never be parsed as a new
 * command position.
 */
class FollowMeGpsInput final : public QObject
{
    Q_OBJECT

public:
    using SourceFactory =
        std::function<FollowMeGpsByteSource *(QObject *parent)>;
    using PortEnumerator = std::function<QStringList()>;

    static constexpr int MaximumLineCharacters =
        NmeaLineFramer::DefaultMaximumLineBytes;

    explicit FollowMeGpsInput(QObject *parent = nullptr);
    FollowMeGpsInput(SourceFactory sourceFactory,
                     PortEnumerator portEnumerator,
                     QObject *parent = nullptr);
    ~FollowMeGpsInput() override;

    QStringList availablePorts() const;
    static QList<int> supportedBaudRates();

    bool open(const QString &portName, int baud, QString *error = nullptr);
    void close();
    bool isOpen() const;
    QString portName() const { return m_portName; }
    int baudRate() const noexcept { return m_baud; }

    // Deliberate test seam for fragmented/coalesced serial input.
    void ingestBytesForTesting(const QByteArray &bytes);

signals:
    void fixReceived(const NmeaGgaFix &fix);
    void noPositionFix(const QString &description);
    void lineRejected(const QString &description);
    void sentenceRejected(NmeaGgaParseError errorCode,
                          const QString &description);
    void inputStopped(const QString &description);

private:
    void ingestBytes(const QByteArray &bytes);
    void processLine(const QByteArray &line);
    void handleResourceError(FollowMeGpsByteSource *source,
                             const QString &description);
    static SourceFactory productionSourceFactory();
    static PortEnumerator productionPortEnumerator();

    SourceFactory m_sourceFactory;
    PortEnumerator m_portEnumerator;
    QPointer<FollowMeGpsByteSource> m_source;
    NmeaLineFramer m_lineFramer;
    QString m_portName;
    int m_baud = 0;
};

#endif // FOLLOWMEGPSINPUT_H
