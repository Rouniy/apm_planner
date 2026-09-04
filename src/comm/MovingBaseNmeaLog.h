#ifndef MOVINGBASENMEALOG_H
#define MOVINGBASENMEALOG_H

#include <QByteArray>
#include <QFile>
#include <QString>

/** Bounded append-only raw NMEA log used by the Moving Base tool. */
class MovingBaseNmeaLog final
{
public:
    static constexpr qint64 DefaultMaximumBytes = 4 * 1024 * 1024;
    static constexpr int MaximumLineBytes = 4096;

    explicit MovingBaseNmeaLog(
        QString path,
        qint64 maximumBytes = DefaultMaximumBytes);
    ~MovingBaseNmeaLog();

    QString path() const { return m_path; }
    QString backupPath() const { return m_path + QStringLiteral(".1"); }
    qint64 maximumBytes() const noexcept { return m_maximumBytes; }
    bool isOpen() const noexcept { return m_file.isOpen(); }

    bool appendLine(const QByteArray &line, QString *error = nullptr);
    void close();

private:
    bool ensureOpen(QString *error);
    bool rotate(QString *error);
    static void setError(QString *error, const QString &message);

    QString m_path;
    qint64 m_maximumBytes = DefaultMaximumBytes;
    QFile m_file;
    qint64 m_unflushedBytes = 0;
};

#endif // MOVINGBASENMEALOG_H
