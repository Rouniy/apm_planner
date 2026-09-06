#pragma once
#include <QBuffer>
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QImage>

namespace GeoRefRuntimeFixture {
inline bool save(const QString &path, const QByteArray &data) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::NewOnly)
        && file.write(data) == data.size() && file.flush();
}
inline bool photo(const QString &path, const QDateTime &shot) {
    QImage image(8, 6, QImage::Format_RGB32); image.fill(QColor(14, 120, 70));
    QByteArray jpeg; QBuffer buffer(&jpeg); buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "JPEG")) return false;
    QByteArray tiff; QDataStream stream(&tiff, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.writeRawData("II", 2); stream << quint16(42) << quint32(8);
    stream << quint16(1) << quint16(0x8769) << quint16(4) << quint32(1) << quint32(26) << quint32(0);
    stream << quint16(1) << quint16(0x9003) << quint16(2) << quint32(20) << quint32(44) << quint32(0);
    const QByteArray date = shot.toLocalTime().toString("yyyy:MM:dd HH:mm:ss").toLatin1() + '\0';
    stream.writeRawData(date.constData(), date.size());
    QByteArray app1 = QByteArray("Exif\0\0", 6) + tiff;
    const int length = app1.size() + 2;
    return save(path, jpeg.left(2) + QByteArray::fromHex("ffe1")
        + char(length >> 8) + char(length & 255) + app1 + jpeg.mid(2));
}
inline bool create(const QString &directory, QString *logPath, QString *photoDirectory) {
    *logPath = QDir(directory).filePath("geo-flight.log");
    *photoDirectory = QDir(directory).filePath("geo-photos");
    if (!QDir().mkpath(*photoDirectory)) return false;
    const QByteArray log =
        "FMT,128,89,FMT,BBnNZ,Type,Length,Name,Format,Columns\n"
        "FMT,150,34,GPS,QBIHLLee,TimeUS,Status,GMS,GWk,Lat,Lng,Alt,RAlt\n"
        "FMT,153,49,CAM,QIHLLeeefff,TimeUS,GPSTime,GPSWeek,Lat,Lng,Alt,RelAlt,GPSAlt,R,P,Y\n"
        "GPS,1000000,3,200000,2400,47.5,8.5,450,20\n"
        "CAM,1000000,200000,2400,47.5,8.5,123.5,12.5,450,1,2,3\n"
        "GPS,3000000,3,202000,2400,47.6,8.6,451,21\n"
        "CAM,3000000,202000,2400,47.6,8.6,124.5,13.5,451,4,5,6\n";
    const auto epoch = QDateTime(QDate(1980, 1, 6), QTime(0, 0), Qt::UTC)
        .addDays(2400 * 7).addSecs(200 - 17 + 12);
    return save(*logPath, log)
        && photo(QDir(*photoDirectory).filePath("one.jpg"), epoch)
        && photo(QDir(*photoDirectory).filePath("two.jpg"), epoch.addSecs(2));
}
}
