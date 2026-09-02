#include "ConfigHWBTSerialService.h"

#include "ConfigHWBTViewModel.h"
#include "LinkManager.h"
#include "SerialLinkInterface.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QPointer>
#include <QMutex>
#include <QMutexLocker>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QSet>
#include <QThread>

#include <algorithm>
#include <atomic>

struct ConfigHWBTSerialOperationGate
{
    std::atomic_bool canceled{false};
    QMutex ownerMutex;
    ConfigHWBTSerialService *owner = nullptr;
};

namespace {
struct SerialOperation
{
    std::shared_ptr<ConfigHWBTSerialOperationGate> gate;
    quint64 generation = 0;
    QString portName;
    QString name;
    QString selectedBaud;
    QString pin;
    bool success = false;
    bool canceled = false;
};

QString portIdentity(const QString &portName)
{
    QSerialPortInfo serialInfo(portName);
    QString candidate = serialInfo.systemLocation();
    if (candidate.isEmpty()) {
        candidate = portName;
    }
    QFileInfo fileInfo(candidate);
    const QString canonical = fileInfo.canonicalFilePath();
    if (!canonical.isEmpty()) {
        candidate = canonical;
    } else if (fileInfo.isAbsolute()) {
        candidate = fileInfo.absoluteFilePath();
    }
#ifdef Q_OS_WIN
    return candidate.toCaseFolded();
#else
    return candidate;
#endif
}

void postProgress(
    const std::shared_ptr<SerialOperation> &operation,
    const QString &line)
{
    ConfigHWBTSerialService *owner = nullptr;
    {
        QMutexLocker locker(&operation->gate->ownerMutex);
        owner = operation->gate->owner;
        if (!owner) {
            return;
        }
        QMetaObject::invokeMethod(
            owner, [owner, generation = operation->generation, line]() {
            emit owner->progress(generation, line);
        }, Qt::QueuedConnection);
    }
}

bool waitCancelable(
    int durationMs, const std::shared_ptr<SerialOperation> &operation)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < durationMs) {
        if (operation->gate->canceled.load(std::memory_order_acquire)) {
            operation->canceled = true;
            return false;
        }
        const int remaining = durationMs - static_cast<int>(timer.elapsed());
        if (remaining <= 0) {
            return true;
        }
        QThread::msleep(static_cast<unsigned long>(qMin(25, remaining)));
    }
    return true;
}

bool writeBytes(
    QSerialPort *port, const QByteArray &bytes,
    const std::shared_ptr<SerialOperation> &operation)
{
    if (operation->gate->canceled.load(std::memory_order_acquire)) {
        operation->canceled = true;
        return false;
    }
    if (!port || port->write(bytes) != bytes.size()
        || !port->waitForBytesWritten(500)) {
        postProgress(operation, QObject::tr("Could not write to port: %1")
                         .arg(port ? port->errorString()
                                   : QObject::tr("serial port unavailable")));
        return false;
    }
    return true;
}

QString displayCommand(const QByteArray &command)
{
    if (command.startsWith("AT+PSWD=") || command.startsWith("AT+PIN")) {
        return command.startsWith("AT+PSWD=")
            ? QStringLiteral("AT+PSWD=<redacted>")
            : QStringLiteral("AT+PIN<redacted>");
    }
    QString text = QString::fromLatin1(command);
    text.replace(QStringLiteral("\r"), QStringLiteral("\\r"));
    text.replace(QStringLiteral("\n"), QStringLiteral("\\n"));
    return text;
}

QByteArray readAvailable(
    QSerialPort *port, const std::shared_ptr<SerialOperation> &operation)
{
    QByteArray response = port ? port->readAll() : QByteArray();
    QElapsedTimer timer;
    timer.start();
    while (port && timer.elapsed() < 200 && response.size() < 4096) {
        if (operation->gate->canceled.load(std::memory_order_acquire)) {
            operation->canceled = true;
            break;
        }
        const int remaining = 200 - static_cast<int>(timer.elapsed());
        if (remaining <= 0
            || !port->waitForReadyRead(qMin(10, remaining))) {
            break;
        }
        response.append(port->readAll());
    }
    if (response.size() > 4096) {
        response.truncate(4096);
    }
    return response;
}

QString safeResponseText(const QByteArray &response, const QString &pin)
{
    QByteArray safe = response;
    const QByteArray secret = pin.toLatin1();
    if (!secret.isEmpty()) {
        safe.replace(secret, QByteArrayLiteral("<redacted>"));
    }
    return QString::fromLatin1(safe);
}

bool runSerialOperation(const std::shared_ptr<SerialOperation> &operation)
{
    const QList<QByteArray> commands =
        ConfigHWBTViewModel::BuildCommands(
            operation->name, operation->selectedBaud, operation->pin);

    for (int baud : ConfigHWBTViewModel::ProbeBauds()) {
        if (operation->gate->canceled.load(std::memory_order_acquire)) {
            operation->canceled = true;
            return false;
        }
        postProgress(operation, QObject::tr("Try baud %1").arg(baud));

        QSerialPort port;
        port.setPortName(operation->portName);
        port.setBaudRate(baud);
        port.setDataBits(QSerialPort::Data8);
        port.setParity(QSerialPort::NoParity);
        port.setStopBits(QSerialPort::OneStop);
        port.setFlowControl(QSerialPort::NoFlowControl);
        if (!port.open(QIODevice::ReadWrite)) {
            postProgress(operation,
                         QObject::tr("Could not open port: %1")
                             .arg(port.errorString()));
            return false;
        }
        port.clear(QSerialPort::AllDirections);

        if (!writeBytes(&port, QByteArrayLiteral("AT"), operation)
            || !waitCancelable(1100, operation)
            || !writeBytes(&port, QByteArrayLiteral("\r\n"), operation)
            || !waitCancelable(200, operation)) {
            return false;
        }
        const QByteArray answer = readAvailable(&port, operation);
        if (operation->canceled) {
            return false;
        }
        if (!answer.contains("OK")) {
            postProgress(operation, QObject::tr("No Answer"));
            if (!waitCancelable(1100, operation)) {
                return false;
            }
            continue;
        }

        postProgress(operation, QObject::tr("Valid Answer"));
        for (const QByteArray &command : commands) {
            postProgress(operation,
                         QObject::tr("Sending %1")
                             .arg(displayCommand(command)));
            if (!writeBytes(&port, command, operation)
                || !waitCancelable(1000, operation)) {
                return false;
            }
            const QByteArray response = readAvailable(&port, operation);
            if (operation->canceled) {
                return false;
            }
            postProgress(operation,
                         QObject::tr("Resp %1")
                             .arg(safeResponseText(
                                 response, operation->pin)));
        }
        return true;
    }
    return false;
}

void appendPort(QStringList *ports, QSet<QString> *identities,
                const QString &port)
{
    if (!ports || !identities || port.trimmed().isEmpty()) {
        return;
    }
    QString identity = QDir::cleanPath(port);
#ifdef Q_OS_WIN
    identity = identity.toCaseFolded();
#endif
    if (!identities->contains(identity)) {
        identities->insert(identity);
        ports->append(port);
    }
}
} // namespace

ConfigHWBTSerialService::ConfigHWBTSerialService(QObject *parent)
    : QObject(parent)
{
}

ConfigHWBTSerialService::~ConfigHWBTSerialService()
{
    if (!m_thread) {
        return;
    }
    QThread *thread = m_thread;
    const auto gate = m_operationGate;
    if (gate) {
        gate->canceled.store(true, std::memory_order_release);
        QMutexLocker locker(&gate->ownerMutex);
        gate->owner = nullptr;
    }
    disconnect(thread, nullptr, this, nullptr);
    thread->requestInterruption();
    thread->quit();
    if (!thread->wait(2000)) {
        // QSerialPort::open() is synchronous and controlled by the platform
        // driver. Never freeze page/app teardown on a broken driver, and
        // never delete a running QThread. The detached thread owns all of
        // its operation state and deletes itself after a later return.
        connect(thread, &QThread::finished,
                thread, &QObject::deleteLater);
        m_thread = nullptr;
        m_operationGate.reset();
        return;
    }
    delete thread;
    m_thread = nullptr;
    m_operationGate.reset();
}

QStringList ConfigHWBTSerialService::availablePorts()
{
    QStringList result;
    QSet<QString> identities;
    for (const QSerialPortInfo &info : QSerialPortInfo::availablePorts()) {
#ifdef Q_OS_WIN
        const QString port = info.portName();
#else
        const QString port = info.systemLocation().isEmpty()
            ? info.portName() : info.systemLocation();
#endif
        appendPort(&result, &identities, port);
    }
#ifndef Q_OS_WIN
    const QStringList patterns = {
        QStringLiteral("ttyACM*"), QStringLiteral("ttyUSB*"),
        QStringLiteral("rfcomm*"), QStringLiteral("tty.*"),
        QStringLiteral("cu.*")
    };
    QDir dev(QStringLiteral("/dev"));
    const QStringList deviceEntries = dev.entryList(
        patterns, QDir::System | QDir::Files | QDir::NoDotAndDotDot,
        QDir::Name);
    for (const QString &entry : deviceEntries) {
        appendPort(&result, &identities, dev.absoluteFilePath(entry));
    }
    QDir byId(QStringLiteral("/dev/serial/by-id"));
    const QStringList stableEntries = byId.entryList(
        QDir::System | QDir::Files | QDir::NoDotAndDotDot,
        QDir::Name);
    for (const QString &entry : stableEntries) {
        appendPort(&result, &identities, byId.absoluteFilePath(entry));
    }
#endif
    return result;
}

void ConfigHWBTSerialService::program(
    quint64 generation, const QString &portName,
    const QString &name, const QString &selectedBaud,
    const QString &pin)
{
    if (m_thread) {
        emit progress(generation,
                      tr("Another Bluetooth operation is still stopping."));
        emit finished(generation, false, false);
        return;
    }
    if (portIsInUse(portName)) {
        emit progress(generation,
                      tr("Could not open port: selected serial port is "
                         "already in use by a vehicle link."));
        emit finished(generation, false, false);
        return;
    }

    auto operation = std::make_shared<SerialOperation>();
    operation->gate =
        std::make_shared<ConfigHWBTSerialOperationGate>();
    operation->gate->owner = this;
    operation->generation = generation;
    operation->portName = portName;
    operation->name = name;
    operation->selectedBaud = selectedBaud;
    operation->pin = pin;
    m_operationGate = operation->gate;
    m_activeGeneration = generation;

    QThread *thread = QThread::create([operation]() {
        operation->success = runSerialOperation(operation);
    });
    m_thread = thread;
    connect(thread, &QThread::finished, this,
            [this, thread, operation]() {
        if (m_thread == thread) {
            m_thread = nullptr;
            m_operationGate.reset();
            m_activeGeneration = 0;
        }
        emit finished(operation->generation, operation->success,
                      operation->canceled);
        thread->deleteLater();
    });
    thread->start();
}

void ConfigHWBTSerialService::cancel(quint64 generation)
{
    if (!m_thread || !m_operationGate
        || (generation != 0 && generation != m_activeGeneration)) {
        return;
    }
    m_operationGate->canceled.store(true, std::memory_order_release);
    m_thread->requestInterruption();
}

bool ConfigHWBTSerialService::portIsInUse(const QString &portName)
{
    const QString wanted = portIdentity(portName);
    LinkManager *manager = LinkManager::instance();
    for (int linkId : manager->getLinks()) {
        if (!manager->getLinkConnected(linkId)) {
            continue;
        }
        auto *serial = qobject_cast<SerialLinkInterface *>(
            manager->getLink(linkId));
        if (serial && portIdentity(serial->getPortName()) == wanted) {
            return true;
        }
    }
    return false;
}
