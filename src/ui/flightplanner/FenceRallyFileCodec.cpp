#include "FenceRallyFileCodec.h"

#include "QGCMAVLink.h"
#include "configuration.h"

#include <QFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStringList>

#include <cmath>

namespace MissionPlanner
{
namespace
{
QString savedByHeader()
{
    return QStringLiteral("#saved by %1 %2")
            .arg(QStringLiteral(QGC_APPLICATION_DISPLAY_NAME),
                 QStringLiteral(QGC_APPLICATION_VERSION));
}

QString number(double value)
{
    return QString::number(value, 'g', 17);
}

QStringList lines(const QByteArray &data)
{
    QString text = QString::fromUtf8(data);
    if (!text.isEmpty() && text.front() == QChar::ByteOrderMark) {
        text.remove(0, 1);
    }
    return text.split(
            QRegularExpression(QStringLiteral("\\r?\\n")),
            Qt::KeepEmptyParts);
}

QStringList fields(const QString &line)
{
    return line.split(QRegularExpression(QStringLiteral("[\\s,]+")),
                      Qt::SkipEmptyParts);
}

bool parseFiniteDouble(const QString &text, double *value)
{
    bool ok = false;
    const double parsed = text.toDouble(&ok);
    if (!ok || !std::isfinite(parsed)) {
        return false;
    }
    if (value) {
        *value = parsed;
    }
    return true;
}

bool sameHorizontalPosition(const GeoCoordinate &left,
                            const GeoCoordinate &right)
{
    return std::abs(left.Latitude - right.Latitude) < 1e-9
            && std::abs(left.Longitude - right.Longitude) < 1e-9;
}

FenceRallyFileCodec::SaveResult writeAtomically(
        const QString &path, const QByteArray &data)
{
    FenceRallyFileCodec::SaveResult result;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        result.error = QObject::tr("Cannot open %1 for writing: %2")
                .arg(path, file.errorString());
        return result;
    }
    if (file.write(data) != data.size()) {
        result.error = QObject::tr("Cannot write %1: %2")
                .arg(path, file.errorString());
        file.cancelWriting();
        return result;
    }
    if (!file.commit()) {
        result.error = QObject::tr("Cannot commit %1: %2")
                .arg(path, file.errorString());
        return result;
    }
    result.ok = true;
    return result;
}

QByteArray readFile(const QString &path, QString *error)
{
    if (error) {
        error->clear();
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QObject::tr("Cannot open %1: %2")
                    .arg(path, file.errorString());
        }
        return {};
    }
    return file.readAll();
}
}

FenceRallyFileCodec::FenceLoadResult
FenceRallyFileCodec::DecodeLegacyFence(const QByteArray &data)
{
    FenceLoadResult result;
    QVector<GeoCoordinate> coordinates;
    const QStringList sourceLines = lines(data);
    for (int index = 0; index < sourceLines.size(); ++index) {
        const QString trimmed = sourceLines.at(index).trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('#'))) {
            continue;
        }
        const QStringList tokens = fields(trimmed);
        if (tokens.size() < 2) {
            result.error = QObject::tr(
                    "Invalid fence coordinate at line %1: expected latitude and longitude.")
                    .arg(index + 1);
            return result;
        }
        GeoCoordinate coordinate;
        if (!parseFiniteDouble(tokens.at(0), &coordinate.Latitude)
            || !parseFiniteDouble(tokens.at(1), &coordinate.Longitude)) {
            result.error = QObject::tr(
                    "Invalid fence coordinate at line %1: latitude and longitude must be finite numbers.")
                    .arg(index + 1);
            return result;
        }
        coordinates.append(coordinate);
    }

    if (coordinates.size() < 4) {
        result.error = QObject::tr(
                "Legacy .fen requires a return point and at least 3 polygon vertices.");
        return result;
    }

    result.fence.HasReturn = true;
    result.fence.ReturnPoint.Return = coordinates.takeFirst();
    result.fence.ReturnPoint.Frame = MAV_FRAME_GLOBAL;

    if (coordinates.size() > 1
        && sameHorizontalPosition(coordinates.first(), coordinates.last())) {
        coordinates.removeLast();
    }
    FencePolygon polygon;
    polygon.Mode = FencePolygon::PolyType::Inclusive;
    polygon.Points = coordinates;
    result.fence.Polygons.append(polygon);

    const ModelValidationResult validation = result.fence.validate();
    if (!validation.ok) {
        result.error = validation.error;
        return result;
    }
    result.ok = true;
    return result;
}

FenceRallyFileCodec::EncodeResult
FenceRallyFileCodec::EncodeLegacyFence(const Fence &fence)
{
    EncodeResult result;
    const ModelValidationResult validation = fence.validate();
    if (!validation.ok) {
        result.error = validation.error;
        return result;
    }
    if (!fence.HasReturn) {
        result.error = QObject::tr(
                "Legacy .fen requires a fence return point.");
        return result;
    }
    if (!fence.Circles.isEmpty()) {
        result.error = QObject::tr(
                "Legacy .fen cannot represent fence circles; use QGC .plan instead.");
        return result;
    }
    if (fence.Polygons.size() != 1
        || fence.Polygons.first().Mode != FencePolygon::PolyType::Inclusive) {
        result.error = QObject::tr(
                "Legacy .fen requires exactly one inclusion polygon and cannot represent exclusion polygons; use QGC .plan instead.");
        return result;
    }

    QStringList output;
    output.append(savedByHeader());
    output.append(QStringLiteral("%1 %2")
                  .arg(number(fence.ReturnPoint.Return.Latitude),
                       number(fence.ReturnPoint.Return.Longitude)));
    const FencePolygon &polygon = fence.Polygons.first();
    for (const GeoCoordinate &point : polygon.Points) {
        output.append(QStringLiteral("%1 %2")
                      .arg(number(point.Latitude),
                           number(point.Longitude)));
    }
    output.append(QStringLiteral("%1 %2")
                  .arg(number(polygon.Points.first().Latitude),
                       number(polygon.Points.first().Longitude)));
    result.data = (output.join(QLatin1Char('\n')) + QLatin1Char('\n')).toUtf8();
    result.ok = true;
    return result;
}

FenceRallyFileCodec::FenceLoadResult
FenceRallyFileCodec::LoadLegacyFence(const QString &path)
{
    QString error;
    const QByteArray data = readFile(path, &error);
    if (!error.isEmpty()) {
        FenceLoadResult result;
        result.error = error;
        return result;
    }
    return DecodeLegacyFence(data);
}

FenceRallyFileCodec::SaveResult
FenceRallyFileCodec::SaveLegacyFence(const QString &path,
                                     const Fence &fence)
{
    const EncodeResult encoded = EncodeLegacyFence(fence);
    if (!encoded.ok) {
        SaveResult result;
        result.error = encoded.error;
        return result;
    }
    return writeAtomically(path, encoded.data);
}

FenceRallyFileCodec::RallyLoadResult
FenceRallyFileCodec::DecodeLegacyRally(const QByteArray &data)
{
    RallyLoadResult result;
    const QStringList sourceLines = lines(data);
    for (int index = 0; index < sourceLines.size(); ++index) {
        const QString trimmed = sourceLines.at(index).trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('#'))) {
            continue;
        }
        const QStringList tokens = fields(trimmed);
        if (tokens.size() < 4 || tokens.size() > 7) {
            result.error = QObject::tr(
                    "Invalid rally point at line %1: expected RALLY and 3 to 6 numeric fields.")
                    .arg(index + 1);
            return result;
        }
        if (tokens.first().compare(QStringLiteral("RALLY"),
                                   Qt::CaseInsensitive) != 0) {
            result.error = QObject::tr(
                    "Unsupported rally record '%1' at line %2.")
                    .arg(tokens.first()).arg(index + 1);
            return result;
        }

        RallyPoint point;
        if (!parseFiniteDouble(tokens.at(1), &point.Position.Latitude)
            || !parseFiniteDouble(tokens.at(2), &point.Position.Longitude)
            || !parseFiniteDouble(tokens.at(3), &point.Position.Altitude)) {
            result.error = QObject::tr(
                    "Invalid rally point at line %1: latitude, longitude and altitude must be finite numbers.")
                    .arg(index + 1);
            return result;
        }
        if (tokens.size() > 4
            && !parseFiniteDouble(tokens.at(4), &point.BreakAltitude)) {
            result.error = QObject::tr(
                    "Invalid rally break altitude at line %1.").arg(index + 1);
            return result;
        }
        if (tokens.size() > 5
            && !parseFiniteDouble(tokens.at(5), &point.LandHeading)) {
            result.error = QObject::tr(
                    "Invalid rally landing heading at line %1.").arg(index + 1);
            return result;
        }
        if (tokens.size() > 6) {
            bool flagsOk = false;
            const int flags = tokens.at(6).toInt(&flagsOk);
            if (!flagsOk || flags < 0 || flags > 255) {
                result.error = QObject::tr(
                        "Invalid rally flags at line %1: expected an integer from 0 to 255.")
                        .arg(index + 1);
                return result;
            }
            point.Flags = static_cast<quint8>(flags);
        }
        point.Frame = MAV_FRAME_GLOBAL_RELATIVE_ALT;
        result.rally.Points.append(point);
    }

    if (result.rally.Points.isEmpty()) {
        result.error = QObject::tr(
                "Legacy .ral contains no rally points.");
        return result;
    }
    const ModelValidationResult validation = result.rally.validate();
    if (!validation.ok) {
        result.error = validation.error;
        return result;
    }
    result.ok = true;
    return result;
}

FenceRallyFileCodec::EncodeResult
FenceRallyFileCodec::EncodeLegacyRally(const RallyPoints &rally)
{
    EncodeResult result;
    const ModelValidationResult validation = rally.validate();
    if (!validation.ok) {
        result.error = validation.error;
        return result;
    }
    if (rally.Points.isEmpty()) {
        result.error = QObject::tr("No rally points to save.");
        return result;
    }

    QStringList output;
    output.append(savedByHeader());
    for (const RallyPoint &point : rally.Points) {
        output.append(QStringList{
            QStringLiteral("RALLY"),
            number(point.Position.Latitude),
            number(point.Position.Longitude),
            number(point.Position.Altitude),
            number(point.BreakAltitude),
            number(point.LandHeading),
            QString::number(point.Flags),
        }.join(QLatin1Char('\t')));
    }
    result.data = (output.join(QLatin1Char('\n')) + QLatin1Char('\n')).toUtf8();
    result.ok = true;
    return result;
}

FenceRallyFileCodec::RallyLoadResult
FenceRallyFileCodec::LoadLegacyRally(const QString &path)
{
    QString error;
    const QByteArray data = readFile(path, &error);
    if (!error.isEmpty()) {
        RallyLoadResult result;
        result.error = error;
        return result;
    }
    return DecodeLegacyRally(data);
}

FenceRallyFileCodec::SaveResult
FenceRallyFileCodec::SaveLegacyRally(const QString &path,
                                     const RallyPoints &rally)
{
    const EncodeResult encoded = EncodeLegacyRally(rally);
    if (!encoded.ok) {
        SaveResult result;
        result.error = encoded.error;
        return result;
    }
    return writeAtomically(path, encoded.data);
}

} // namespace MissionPlanner
