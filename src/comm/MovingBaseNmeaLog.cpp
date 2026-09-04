#include "MovingBaseNmeaLog.h"

#include <QDir>
#include <QFileInfo>
#include <QSaveFile>

#include <utility>

namespace
{
constexpr qint64 FlushThresholdBytes = 64 * 1024;
}

MovingBaseNmeaLog::MovingBaseNmeaLog(QString path, qint64 maximumBytes)
    : m_path(std::move(path)),
      m_maximumBytes(maximumBytes)
{
}

MovingBaseNmeaLog::~MovingBaseNmeaLog()
{
    close();
}

bool MovingBaseNmeaLog::appendLine(
    const QByteArray &line, QString *error)
{
    if (error) {
        error->clear();
    }
    if (m_path.isEmpty() || m_maximumBytes <= 1) {
        setError(error, QStringLiteral("Moving Base log path or size is invalid."));
        return false;
    }
    if (line.size() > MaximumLineBytes
        || line.contains('\n') || line.contains('\r')) {
        setError(error, QStringLiteral("Raw NMEA line is invalid or too long."));
        return false;
    }

    const QByteArray record = line + '\n';
    if (record.size() > m_maximumBytes) {
        setError(error, QStringLiteral("Raw NMEA line exceeds the log size bound."));
        return false;
    }
    if (!ensureOpen(error)) {
        return false;
    }
    if (m_file.size() + record.size() > m_maximumBytes
        && !rotate(error)) {
        return false;
    }
    if (m_file.write(record) != record.size()) {
        setError(error, QStringLiteral("Cannot append Moving Base log: %1")
                            .arg(m_file.errorString()));
        close();
        return false;
    }
    m_unflushedBytes += record.size();
    if (m_unflushedBytes >= FlushThresholdBytes) {
        if (!m_file.flush()) {
            setError(error, QStringLiteral("Cannot flush Moving Base log: %1")
                                .arg(m_file.errorString()));
            close();
            return false;
        }
        m_unflushedBytes = 0;
    }
    return true;
}

void MovingBaseNmeaLog::close()
{
    if (m_file.isOpen()) {
        m_file.flush();
        m_file.close();
    }
    m_unflushedBytes = 0;
}

bool MovingBaseNmeaLog::ensureOpen(QString *error)
{
    if (m_file.isOpen()) {
        return true;
    }
    const QFileInfo info(m_path);
    if (!QDir().mkpath(info.absolutePath())) {
        setError(error, QStringLiteral("Cannot create Moving Base log directory."));
        return false;
    }
    m_file.setFileName(m_path);
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        setError(error, QStringLiteral("Cannot open Moving Base log: %1")
                            .arg(m_file.errorString()));
        return false;
    }
    return true;
}

bool MovingBaseNmeaLog::rotate(QString *error)
{
    close();
    const QString backup = backupPath();
    const QFileInfo currentInfo(m_path);
    if (currentInfo.exists() && currentInfo.size() > m_maximumBytes) {
        QFile current(m_path);
        if (!current.open(QIODevice::ReadOnly)
            || !current.seek(currentInfo.size() - m_maximumBytes)) {
            setError(error, QStringLiteral(
                "Cannot read the bounded tail of the Moving Base log."));
            return false;
        }
        const QByteArray tail = current.read(m_maximumBytes);
        current.close();
        if (tail.size() != m_maximumBytes) {
            setError(error, QStringLiteral(
                "Cannot read the complete bounded Moving Base log tail."));
            return false;
        }
        QSaveFile boundedBackup(backup);
        if (!boundedBackup.open(QIODevice::WriteOnly)
            || boundedBackup.write(tail) != tail.size()
            || !boundedBackup.commit()) {
            setError(error, QStringLiteral(
                "Cannot replace the oversized Moving Base log backup."));
            return false;
        }
        if (!QFile::remove(m_path)) {
            setError(error, QStringLiteral(
                "Cannot replace the oversized Moving Base active log."));
            return false;
        }
        return ensureOpen(error);
    }
    if (QFile::exists(backup) && !QFile::remove(backup)) {
        setError(error, QStringLiteral("Cannot replace the previous Moving Base log backup."));
        return false;
    }
    if (QFile::exists(m_path) && !QFile::rename(m_path, backup)) {
        setError(error, QStringLiteral("Cannot rotate the Moving Base log."));
        return false;
    }
    return ensureOpen(error);
}

void MovingBaseNmeaLog::setError(QString *error, const QString &message)
{
    if (error) {
        *error = message;
    }
}
