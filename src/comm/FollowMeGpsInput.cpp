#include "FollowMeGpsInput.h"

#include <QSerialPort>
#include <QSerialPortInfo>

#include <algorithm>
#include <utility>

namespace
{
class FollowMeGpsSerialSource final : public FollowMeGpsByteSource
{
public:
    explicit FollowMeGpsSerialSource(QObject *parent)
        : FollowMeGpsByteSource(parent)
        , m_serial(new QSerialPort(this))
    {
        // Bound the bytes retained by Qt before the line framer applies its
        // much smaller per-record limit.
        m_serial->setReadBufferSize(64 * 1024);
        connect(m_serial, &QSerialPort::readyRead, this, [this]() {
            const QByteArray bytes = m_serial->readAll();
            if (!bytes.isEmpty()) {
                emit bytesReceived(bytes);
            }
        });
        connect(m_serial, &QSerialPort::errorOccurred, this,
                [this](QSerialPort::SerialPortError error) {
            if (m_reportingFailure || error == QSerialPort::NoError) {
                return;
            }
            if (error != QSerialPort::ResourceError
                && error != QSerialPort::DeviceNotFoundError
                && error != QSerialPort::PermissionError
                && error != QSerialPort::ReadError) {
                return;
            }
            m_reportingFailure = true;
            emit resourceError(
                tr("NMEA GPS port %1 stopped: %2")
                    .arg(m_serial->portName(), m_serial->errorString()));
        });
    }

    bool open(const QString &portName, int baud, QString *error) override
    {
        close();
        m_reportingFailure = false;
        m_serial->setPortName(portName);
        const bool configured = m_serial->setBaudRate(baud)
            && m_serial->setDataBits(QSerialPort::Data8)
            && m_serial->setParity(QSerialPort::NoParity)
            && m_serial->setStopBits(QSerialPort::OneStop)
            && m_serial->setFlowControl(QSerialPort::NoFlowControl);
        if (!configured) {
            if (error) {
                *error = tr("Cannot configure NMEA GPS port %1 at %2 baud: %3")
                    .arg(portName).arg(baud).arg(m_serial->errorString());
            }
            return false;
        }
        if (!m_serial->open(QIODevice::ReadOnly)) {
            if (error) {
                *error = tr("Cannot open NMEA GPS port %1: %2")
                    .arg(portName, m_serial->errorString());
            }
            return false;
        }
        return true;
    }

    void close() override
    {
        if (m_serial->isOpen()) {
            m_serial->close();
        }
    }

    bool isOpen() const override { return m_serial->isOpen(); }

private:
    QSerialPort *m_serial = nullptr;
    bool m_reportingFailure = false;
};
}

FollowMeGpsInput::FollowMeGpsInput(QObject *parent)
    : FollowMeGpsInput(productionSourceFactory(),
                       productionPortEnumerator(), parent)
{
}

FollowMeGpsInput::FollowMeGpsInput(
    SourceFactory sourceFactory, PortEnumerator portEnumerator,
    QObject *parent)
    : QObject(parent)
    , m_sourceFactory(std::move(sourceFactory))
    , m_portEnumerator(std::move(portEnumerator))
{
    qRegisterMetaType<NmeaGgaFix>();
    qRegisterMetaType<NmeaGgaParseError>();
}

FollowMeGpsInput::~FollowMeGpsInput()
{
    close();
}

QStringList FollowMeGpsInput::availablePorts() const
{
    QStringList ports;
    if (m_portEnumerator) {
        try {
            ports = m_portEnumerator();
        } catch (...) {
            ports.clear();
        }
    }
    ports.removeAll(QString());
    ports.removeDuplicates();
    std::sort(ports.begin(), ports.end());
    return ports;
}

QList<int> FollowMeGpsInput::supportedBaudRates()
{
    return {4800, 9600, 14400, 19200,
            28800, 38400, 57600, 115200};
}

bool FollowMeGpsInput::open(const QString &portName, int baud,
                            QString *error)
{
    close();
    if (error) {
        error->clear();
    }
    const QString selectedPort = portName.trimmed();
    if (selectedPort.isEmpty()) {
        if (error) {
            *error = tr("Select a GPS serial port first.");
        }
        return false;
    }
    if (!supportedBaudRates().contains(baud)) {
        if (error) {
            *error = tr("Unsupported NMEA GPS baud rate %1.").arg(baud);
        }
        return false;
    }
    if (!m_sourceFactory) {
        if (error) {
            *error = tr("The NMEA GPS input service is unavailable.");
        }
        return false;
    }

    FollowMeGpsByteSource *source = m_sourceFactory(this);
    if (!source) {
        if (error) {
            *error = tr("The NMEA GPS input service is unavailable.");
        }
        return false;
    }
    if (source->parent() != this) {
        source->setParent(this);
    }
    connect(source, &FollowMeGpsByteSource::bytesReceived,
            this, [this, source](const QByteArray &bytes) {
        if (m_source == source) {
            ingestBytes(bytes);
        }
    });
    connect(source, &FollowMeGpsByteSource::resourceError,
            this, [this, source](const QString &description) {
        handleResourceError(source, description);
    });

    QString openError;
    if (!source->open(selectedPort, baud, &openError)) {
        disconnect(source, nullptr, this, nullptr);
        source->close();
        delete source;
        if (error) {
            *error = openError.isEmpty()
                ? tr("Cannot open NMEA GPS port %1.").arg(selectedPort)
                : openError;
        }
        return false;
    }

    m_source = source;
    m_portName = selectedPort;
    m_baud = baud;
    return true;
}

void FollowMeGpsInput::close()
{
    FollowMeGpsByteSource *source = m_source.data();
    m_source = nullptr;
    m_lineFramer.reset();
    m_portName.clear();
    m_baud = 0;
    if (!source) {
        return;
    }
    disconnect(source, nullptr, this, nullptr);
    source->close();
    source->deleteLater();
}

bool FollowMeGpsInput::isOpen() const
{
    return m_source && m_source->isOpen();
}

void FollowMeGpsInput::ingestBytesForTesting(const QByteArray &bytes)
{
    ingestBytes(bytes);
}

void FollowMeGpsInput::ingestBytes(const QByteArray &bytes)
{
    const NmeaLineFramerResult framed = m_lineFramer.ingest(bytes);
    for (int index = 0; index < framed.oversizedLines; ++index) {
        emit lineRejected(
            tr("NMEA input line exceeds %1 characters.")
                .arg(MaximumLineCharacters));
    }
    for (const QByteArray &line : framed.lines) {
        processLine(line);
    }
}

void FollowMeGpsInput::processLine(const QByteArray &line)
{
    const NmeaGgaParseResult result = NmeaGgaParser::parse(
        QString::fromLatin1(line));
    if (result.isValid()) {
        emit fixReceived(result.fix);
        return;
    }
    if (result.errorCode == NmeaGgaParseError::NoPositionFix) {
        emit noPositionFix(result.error);
        return;
    }
    emit sentenceRejected(result.errorCode, result.error);
}

void FollowMeGpsInput::handleResourceError(
    FollowMeGpsByteSource *source, const QString &description)
{
    if (!source || m_source != source) {
        return;
    }
    const QString failure = description.isEmpty()
        ? tr("The NMEA GPS input stopped because its serial port failed.")
        : description;
    close();
    emit inputStopped(failure);
}

FollowMeGpsInput::SourceFactory
FollowMeGpsInput::productionSourceFactory()
{
    return [](QObject *parent) -> FollowMeGpsByteSource * {
        return new FollowMeGpsSerialSource(parent);
    };
}

FollowMeGpsInput::PortEnumerator
FollowMeGpsInput::productionPortEnumerator()
{
    return []() {
        QStringList ports;
        for (const QSerialPortInfo &info
             : QSerialPortInfo::availablePorts()) {
            ports.append(info.portName());
        }
        return ports;
    };
}
