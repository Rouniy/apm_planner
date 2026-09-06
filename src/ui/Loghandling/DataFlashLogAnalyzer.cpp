#include "DataFlashLogAnalyzer.h"
#include "DataFlashRawReader.h"
#include "DataFlashModeNames.h"

#include <QFileInfo>
#include <QDateTime>
#include <QHash>
#include <QLocale>
#include <QSet>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <new>
#include <utility>

namespace {
using API = DataFlashLogAnalyzer;
using Sample = API::Sample;
using Rows = QVector<Sample>;
using Test = API::TestResult;
using Maybe = std::optional<double>;
constexpr double Epsilon = 1e-9;
const QString Good = QStringLiteral("GOOD"), Warn = QStringLiteral("WARN"),
    Fail = QStringLiteral("FAIL"), Unknown = QStringLiteral("UNKNOWN"), NA = QStringLiteral("NA");
struct Interrupted { bool cancelled; QString error; };
struct Work {
    API::Cancel cancel;
    API::Progress progress;
    quint64 iterations = 0;
    void check() { if (cancel && cancel()) throw Interrupted{true, {}}; }
    void tick() { if ((++iterations & 255) == 0) check(); }
    void report(qint64 done) { if (progress) progress(done, 1000); check(); }
};
QString decimal(double value, int digits, int significantDigits, int scale, bool toEven)
{
    if (std::isnan(value)) return QStringLiteral("NaN");
    if (std::isinf(value)) return value < 0 ? QStringLiteral("-Infinity") : QStringLiteral("Infinity");
    // .NET custom numeric formats first use a 15-significant-digit decimal
    // buffer, then round decimal ties away from zero (not binary Qt fixed).
    const QString scientific = QString::number(std::abs(value), 'e', significantDigits - 1);
    const int e = scientific.indexOf('e');
    QString mantissa = scientific.left(e); mantissa.remove('.');
    const int places = scientific.mid(e + 1).toInt() + scale + 1 + digits;
    QString rounded;
    if (places < 0) rounded = QStringLiteral("0");
    else if (places >= mantissa.size()) rounded = mantissa + QString(places - mantissa.size(), '0');
    else {
        rounded = places ? mantissa.left(places) : QStringLiteral("0");
        const QChar next = mantissa.at(places);
        bool remainder = false;
        for (int i = places + 1; i < mantissa.size(); ++i) if (mantissa.at(i) != QLatin1Char('0')) remainder = true;
        const bool up = next > QLatin1Char('5') || (next == QLatin1Char('5')
            && (!toEven || remainder || rounded.at(rounded.size() - 1).digitValue() % 2));
        if (up) {
            int i = rounded.size() - 1;
            while (i >= 0 && rounded.at(i) == QLatin1Char('9')) rounded[i--] = QLatin1Char('0');
            if (i < 0) rounded.prepend('1'); else rounded[i] = QChar(rounded.at(i).unicode() + 1);
        }
    }
    while (rounded.size() > 1 && rounded.front() == QLatin1Char('0')) rounded.remove(0, 1);
    if (digits) {
        rounded = rounded.rightJustified(digits + 1, '0');
        rounded.insert(rounded.size() - digits, '.');
    }
    if (std::signbit(value)) rounded.prepend('-');
    return rounded;
}
QString fixed(double value, int digits) { return decimal(value, digits, 15, 0, false); }
QString percent(double value)
{
    if (!std::isfinite(value)) return fixed(value, 0);
    QString text = decimal(value, 1, 17, 2, true);
    const int point = text.indexOf('.'), start = text.startsWith('-') ? 1 : 0;
    for (int i = point - 3; i > start; i -= 3) text.insert(i, ',');
    return text + QStringLiteral(" %");
}
QString general(double value)
{
    if (!std::isfinite(value)) return fixed(value, 0);
    if (value == 0 && std::signbit(value)) return QStringLiteral("-0");
    for (int digits = 1; digits <= 17; ++digits) {
        const QString text = QString::number(value, 'g', digits);
        if (text.toDouble() != value) continue;
        const int e = text.indexOf('e');
        if (e < 0) return text;
        const int exponent = text.mid(e + 1).toInt();
        if (exponent < -4 || exponent >= 17) return text.toUpper();
        QString mantissa = text.left(e); const bool negative = mantissa.startsWith('-');
        if (negative) mantissa.remove(0, 1);
        mantissa.remove('.');
        const int point = exponent + 1;
        if (point <= 0) mantissa.prepend(QStringLiteral("0.") + QString(-point, '0'));
        else if (point >= mantissa.size()) mantissa += QString(point - mantissa.size(), '0');
        else mantissa.insert(point, '.');
        return negative ? '-' + mantissa : mantissa;
    }
    return QString::number(value, 'g', 17);
}
Test result(const char *name, const QString &status, const QString &message)
{ return {QString::fromLatin1(name), status, message}; }
int severity(const QString &status)
{ return status == Fail ? 4 : status == Warn ? 3 : status == Unknown ? 2 : status == NA ? 1 : 0; }
QString worse(const QString &a, const QString &b) { return severity(a) >= severity(b) ? a : b; }
Maybe value(const Sample &sample, std::initializer_list<const char *> aliases)
{
    for (const char *alias : aliases)
        for (const auto &entry : sample.values)
            if (entry.first.compare(QLatin1String(alias), Qt::CaseInsensitive) == 0) return entry.second;
    return {};
}
Maybe parameter(const API::Data &data, const char *name)
{
    for (const auto &entry : data.parameters)
        if (entry.first.compare(QLatin1String(name), Qt::CaseInsensitive) == 0) return entry.second;
    return {};
}
QString textValue(const Sample &sample, const char *name)
{
    for (const auto &entry : sample.textValues)
        if (entry.first.compare(QLatin1String(name), Qt::CaseInsensitive) == 0) return entry.second;
    return {};
}
QVector<double> values(const Rows &rows, std::initializer_list<const char *> aliases, Work &work)
{
    QVector<double> found;
    for (const auto &sample : rows) { work.tick(); const auto v = value(sample, aliases); if (v) found.append(*v); }
    return found;
}
double maximum(const QVector<double> &values)
{
    double found = std::numeric_limits<double>::quiet_NaN();
    for (double value : values) if (std::isnan(found) || value > found) found = value;
    return found;
}
double minimum(const QVector<double> &values)
{
    double found = std::numeric_limits<double>::infinity();
    for (double value : values) { if (std::isnan(value)) return value; if (value < found) found = value; }
    return found;
}
double average(const QVector<double> &values)
{ double sum = 0; for (double v : values) sum += v; return sum / values.size(); }
double maxDotNet(double a, double b) { return std::isnan(a) || a >= b ? a : b; }
Maybe magnitude(Maybe x, Maybe y, Maybe z)
{ return x && y && z ? Maybe(std::sqrt(*x * *x + *y * *y + *z * *z)) : Maybe(); }
int integer(double value)
{
    // Actual .NET 10 conv.i4 saturates and maps NaN to zero. Avoid undefined
    // C++ conversion while retaining the reference's unchecked casts.
    if (std::isnan(value)) return 0;
    if (value <= std::numeric_limits<int>::min()) return std::numeric_limits<int>::min();
    if (value >= std::numeric_limits<int>::max()) return std::numeric_limits<int>::max();
    return int(value);
}
double roundEven(double value)
{
    if (!std::isfinite(value)) return value;
    const double floor = std::floor(value), fraction = value - floor;
    if (fraction != 0.5) return fraction < 0.5 ? floor : floor + 1;
    return std::fmod(floor, 2) == 0 ? floor : floor + 1;
}

// Stable source-order ties reproduce the reference's complete linear scan,
// without quadratic ATT×CTUN processing on long logs.
struct LineIndex {
    struct Item { qint64 line; int ordinal; const Sample *sample; };
    QVector<Item> items;
    LineIndex(const Rows &rows, Work &work, std::initializer_list<const char *> aliases = {}) {
        for (int i = 0; i < rows.size(); ++i) {
            work.tick(); if (aliases.size() == 0 || value(rows[i], aliases)) items.append({rows[i].line, i, &rows[i]});
        }
        std::sort(items.begin(), items.end(), [&](const Item &a, const Item &b) {
            work.tick(); return a.line != b.line ? a.line < b.line : a.ordinal < b.ordinal;
        });
    }
    const Item *firstAt(qint64 line) const {
        const auto at = std::lower_bound(items.cbegin(), items.cend(), line,
            [](const Item &a, qint64 b) { return a.line < b; });
        return at == items.cend() ? nullptr : &*at;
    }
    const Sample *previous(qint64 line) const {
        auto at = std::upper_bound(items.cbegin(), items.cend(), line,
            [](qint64 a, const Item &b) { return a < b.line; });
        if (at == items.cbegin()) return nullptr;
        --at; return firstAt(at->line)->sample;
    }
    const Sample *nearest(qint64 line) const {
        const Item *next = firstAt(line);
        auto at = std::lower_bound(items.cbegin(), items.cend(), line,
            [](const Item &a, qint64 b) { return a.line < b; });
        const Item *before = at == items.cbegin() ? nullptr : firstAt((at - 1)->line);
        if (!before) return next ? next->sample : nullptr;
        if (!next) return before->sample;
        const qint64 d1 = line - before->line, d2 = next->line - line;
        return d1 < d2 || (d1 == d2 && before->ordinal < next->ordinal) ? before->sample : next->sample;
    }
};
class Checks {
public:
    const API::Data &data;
    Work &work;
    Checks(const API::Data &data, Work &work) : data(data), work(work) {}
    const Rows &rows(const char *type) const {
        for (const auto &group : data.records)
            if (group.type.compare(QLatin1String(type), Qt::CaseInsensitive) == 0) return group.samples;
        static const Rows empty; return empty;
    }
    QVector<QPair<int, int>> errors() {
        QVector<QPair<int, int>> found;
        for (const auto &sample : rows("ERR")) {
            work.tick(); const auto subsystem = value(sample, {"Subsys"}), code = value(sample, {"ECode"});
            if (subsystem && code) found.append({integer(*subsystem), integer(*code)});
        }
        return found;
    }
    Test empty() {
        if (data.lineCount == 0) return result("Empty", Fail, "log contains no records");
        const auto &ctun = rows("CTUN");
        if (ctun.isEmpty()) return result("Empty", Unknown, "no CTUN throttle data");
        const auto throttle = values(ctun, {"ThrOut", "ThO"}, work);
        if (throttle.isEmpty()) return result("Empty", Unknown, "no throttle output column");
        const double max = maximum(throttle);
        const double threshold = max <= 1.5 ? 0.2 : data.vehicleType == API::VehicleType::Copter ? 200 : 20;
        return max < threshold ? result("Empty", Fail, "throttle never exceeded 20%")
            : result("Empty", Good, "log contains powered flight data");
    }
    Test vibration() {
        QVector<double> vibration;
        for (const auto &sample : rows("VIBE")) for (const char *field : {"VibeX", "VibeY", "VibeZ"}) {
            work.tick(); const auto v = value(sample, {field}); if (v) vibration.append(std::abs(*v));
        }
        if (vibration.isEmpty()) return result("Vibration", Unknown, "no VIBE data");
        const double max = maximum(vibration);
        return result("Vibration", API::Classify(max, 30, 60, true), "maximum " + fixed(max, 1) + " m/s/s");
    }
    Test gps() {
        const auto &gps = rows("GPS").isEmpty() ? rows("GPS2") : rows("GPS");
        if (gps.isEmpty()) return result("GPS", Unknown, "no GPS/GPS2 data");
        const auto sats = values(gps, {"NSats", "NSat", "numSV"}, work);
        const auto hdop = values(gps, {"HDop", "HDp", "EPH"}, work);
        int glitches = 0; for (const auto &e : errors()) if (e.first == 11 && e.second == 2) ++glitches;
        if (sats.isEmpty() && hdop.isEmpty()) return result("GPS", glitches ? Fail : Unknown,
            glitches ? QStringLiteral("GPS glitch errors: %1").arg(glitches) : QStringLiteral("satellite/HDop columns are missing"));
        const double min = sats.isEmpty() ? std::numeric_limits<double>::infinity() : minimum(sats);
        const double max = hdop.isEmpty() ? 0 : maximum(hdop);
        QString status = glitches || min < 5 || max > 10 ? Fail : min < 6 || max > 3 ? Warn : Good;
        QString message = QStringLiteral("min satellites %1, max HDop %2")
            .arg(std::isinf(min) && min > 0 ? QStringLiteral("n/a") : fixed(min, 0), fixed(max, 2));
        if (glitches) message.prepend(QStringLiteral("%1 GPS glitch error(s); ").arg(glitches));
        return result("GPS", status, message);
    }
    Test vcc() {
        auto found = values(rows("CURR"), {"Vcc"}, work);
        if (found.isEmpty()) found = values(rows("POWR"), {"Vcc"}, work);
        if (found.isEmpty()) return result("VCC", Unknown, "no CURR/POWR Vcc data");
        auto sorted = found;
        std::sort(sorted.begin(), sorted.end(), [&](double a, double b) {
            work.tick(); return std::isnan(a) ? !std::isnan(b) : !std::isnan(b) && a < b;
        });
        if (sorted[sorted.size() / 2] > 100) for (double &v : found) { work.tick(); v /= 1000; }
        const double min = minimum(found), spread = maximum(found) - min;
        return result("VCC", min < 4.3 || spread > 0.5 ? Fail : min < 4.6 || spread > 0.3 ? Warn : Good,
            "minimum " + fixed(min, 2) + " V, spread " + fixed(spread, 2) + " V");
    }
    Test compass() {
        QString status = Good; QStringList messages;
        const auto offset = magnitude(parameter(data, "COMPASS_OFS_X"), parameter(data, "COMPASS_OFS_Y"), parameter(data, "COMPASS_OFS_Z"));
        auto assess = [&](double magnitude, const char *source) {
            const QString offsetStatus = magnitude > 600 ? Fail : magnitude > 350 ? Warn : Good;
            status = worse(status, offsetStatus);
            if (offsetStatus != Good) messages.append(QString::fromLatin1(source) + " compass offset magnitude " + fixed(magnitude, 0));
        };
        if (offset) assess(*offset, "parameter");
        QVector<double> logged, fields; bool zero = false;
        for (const auto &sample : rows("MAG")) {
            work.tick();
            const auto ofs = magnitude(value(sample, {"OfsX"}), value(sample, {"OfsY"}), value(sample, {"OfsZ"}));
            if (ofs) logged.append(*ofs);
            const auto field = magnitude(value(sample, {"MagX"}), value(sample, {"MagY"}), value(sample, {"MagZ"}));
            if (field) { if (*field <= Epsilon) zero = true; else fields.append(*field); }
        }
        if (!logged.isEmpty()) assess(maximum(logged), "logged");
        if (!fields.isEmpty()) {
            const double min = minimum(fields), max = maximum(fields), change = (max - min) / min;
            status = worse(status, change > 0.35 ? Fail : change > 0.25 || zero ? Warn : Good);
            messages.append("magnetic field change " + percent(change) + ", range " + fixed(min, 0) + '-' + fixed(max, 0));
        } else if (logged.isEmpty() && !offset) return result("Compass", Unknown, "no MAG or compass-offset data");
        return result("Compass", status, messages.isEmpty() ? QStringLiteral("offsets and magnetic field are within limits") : messages.join("; "));
    }
    Test motorBalance() {
        if (data.vehicleType != API::VehicleType::Copter) return result("Motor balance", NA, "copter-only check");
        if (rows("RCOU").isEmpty()) return result("Motor balance", Unknown, "no RCOU data");
        int count = 8; const auto frame = parameter(data, "FRAME_CLASS");
        if (frame) switch (integer(roundEven(*frame))) {
        case 0: case 11: count = 4; break; case 1: case 4: count = 6; break;
        case 2: case 3: count = 8; break; case 6: count = 3; break;
        case 8: case 9: count = 2; break; case 10: count = 12; break; case 12: count = 10; break;
        }
        QVector<double> averages;
        for (int channel = 1; channel <= count; ++channel) {
            const auto a = QByteArray("C") + QByteArray::number(channel), b = QByteArray("Ch") + QByteArray::number(channel), c = QByteArray("Chan") + QByteArray::number(channel);
            QVector<double> filtered;
            for (double v : values(rows("RCOU"), {a.constData(), b.constData(), c.constData()}, work)) {
                work.tick(); if (v > 0 && v < 3000) filtered.append(v);
            }
            if (!filtered.isEmpty()) averages.append(average(filtered));
        }
        if (averages.size() < 2) return result("Motor balance", Unknown, "fewer than two motor output channels");
        const double spread = maximum(averages) - minimum(averages);
        return result("Motor balance", spread > 150 ? Fail : spread > 75 ? Warn : Good,
            QStringLiteral("%1 channel averages, output spread %2 us").arg(averages.size()).arg(fixed(spread, 0)));
    }
    Test nan() {
        for (const auto &group : data.records) for (const auto &sample : group.samples) for (const auto &field : sample.values) {
            work.tick(); if (!std::isfinite(field.second)) return result("NaN", Fail,
                QStringLiteral("invalid number in %1.%2 at line %3").arg(group.type, field.first).arg(sample.line));
        }
        for (const auto &p : data.parameters) { work.tick(); if (!std::isfinite(p.second)) return result("NaN", Fail, "invalid parameter value: " + p.first); }
        return result("NaN", Good, "no invalid numeric values");
    }
    Test events() {
        QSet<QString> names;
        for (const auto &e : errors()) {
            const int c = e.second; QString name;
            switch (e.first) {
            case 2: if (c == 1) name = "PPM"; break; case 3: if (c == 1 || c == 2) name = "COMPASS"; break;
            case 5: if (c == 1) name = "FS_THR"; break; case 6: if (c == 1) name = "FS_BATT"; break;
            case 7: if (c == 1) name = "GPS"; break; case 8: if (c == 1) name = "GCS"; break;
            case 9: if (c == 1 || c == 2) name = "FENCE"; break; case 10: name = "FLT_MODE"; break;
            case 11: if (c == 2) name = "GPS_GLITCH"; break; case 12: if (c == 1) name = "CRASH"; break;
            }
            if (!name.isEmpty()) names.insert(name);
        }
        if (names.isEmpty()) return result("Event/Failsafe", Good, "no recognized ERR/failsafe events");
        QStringList sorted = names.values(); std::sort(sorted.begin(), sorted.end());
        return result("Event/Failsafe", names.size() == 1 && names.contains("FENCE") ? Warn : Fail, sorted.join(", "));
    }
    Test brownout() {
        bool armed = false;
        for (double event : values(rows("EV"), {"Id"}, work)) {
            if (integer(event) == 10) armed = true; else if (integer(event) == 11) armed = false;
        }
        const auto altitude = values(rows("CTUN"), {"BarAlt", "Alt"}, work);
        if (altitude.isEmpty()) return result("Brownout", Unknown, "no CTUN altitude data");
        return armed && altitude.last() > 3
            ? result("Brownout", Fail, "log ends armed at " + fixed(altitude.last(), 2) + " m; possible truncation/brownout")
            : result("Brownout", Good, "log ending is consistent with a normal shutdown");
    }
    Test duplicate() {
        QVector<QPair<qint64, double>> pitch;
        for (const auto &sample : rows("ATT")) { work.tick(); const auto v = value(sample, {"Pitch"}); if (v) pitch.append({sample.line, *v}); }
        if (pitch.size() < 40) return result("Duplicate data", Unknown, "insufficient ATT.Pitch samples");
        QHash<QByteArray, int> windows;
        for (int i = 0; i <= pitch.size() - 20; ++i) {
            work.tick(); bool constant = true;
            for (int j = 1; j < 20; ++j) if (pitch[i + j].second != pitch[i].second) { constant = false; break; }
            if (constant) continue;
            QByteArray key; key.reserve(160);
            for (int j = 0; j < 20; ++j) {
                double v = pitch[i + j].second; quint64 bits;
                // .NET R text canonicalizes all NaNs but preserves negative
                // zero. Binary identity is equivalent for other doubles.
                if (std::isnan(v)) bits = Q_UINT64_C(0x7ff8000000000000);
                else std::memcpy(&bits, &v, sizeof(bits));
                key.append(reinterpret_cast<const char *>(&bits), sizeof(bits));
            }
            auto previous = windows.constFind(key);
            if (previous != windows.constEnd()) {
                if (i - *previous >= 20) return result("Duplicate data", Fail,
                    QStringLiteral("duplicate 20-sample ATT.Pitch chunks at lines %1 and %2").arg(pitch[*previous].first).arg(pitch[i].first));
            } else windows.insert(key, i);
        }
        return result("Duplicate data", Good, "no repeated ATT.Pitch chunks");
    }
    Test parameters() {
        QStringList errors;
        for (const auto &p : data.parameters) { work.tick(); if (!std::isfinite(p.second)) errors.append(p.first + " is not finite"); }
        if (data.vehicleType == API::VehicleType::Copter) {
            const auto mag = parameter(data, "MAG_ENABLE"), min = parameter(data, "THR_MIN"), mid = parameter(data, "THR_MID");
            if (mag && *mag != 1) errors.append("MAG_ENABLE=" + general(*mag) + " must equal 1");
            if (min && !(*min < 200)) errors.append("THR_MIN=" + general(*min) + " must be below 200");
            if (mid && !(*mid > 299 && *mid < 701)) errors.append("THR_MID=" + general(*mid) + " must be between 300 and 700");
        }
        return errors.isEmpty() ? result("Parameters", Good, "no known unsafe parameter values") : result("Parameters", Fail, errors.join("; "));
    }
    Test performance() {
        if (data.vehicleType != API::VehicleType::Copter) return result("PM", NA, "copter-only check");
        if (rows("PM").isEmpty()) return result("PM", Unknown, "no PM performance data");
        int count = 0; double max = 0; qint64 line = 0;
        for (const auto &sample : rows("PM")) {
            work.tick(); const auto longLoops = value(sample, {"NLon"}), total = value(sample, {"NLoop"});
            if (!longLoops || !total || *total <= 0) continue;
            const double percent = *longLoops / *total * 100;
            if (percent > 6) { if (!count || percent > max) { max = percent; line = sample.line; } ++count; }
        }
        if (!count) return result("PM", Good, "no slow-loop interval above 6%");
        return result("PM", max > 10 || count > 6 ? Fail : Warn,
            QStringLiteral("%1 slow-loop interval(s), maximum %2% at line %3").arg(count).arg(fixed(max, 2)).arg(line));
    }
    Test pitchRoll() {
        if (data.vehicleType != API::VehicleType::Copter) return result("Pitch/Roll", NA, "copter-only check");
        const auto &attitude = rows("ATT"), &ctun = rows("CTUN");
        if (attitude.isEmpty() || ctun.isEmpty()) return result("Pitch/Roll", Unknown, "ATT or CTUN altitude data is missing");
        const auto angle = parameter(data, "ANGLE_MAX"); const double limit = (angle ? *angle / 100 : 45) + 10;
        LineIndex altitudes(ctun, work, {"BarAlt", "Alt"}), modes(rows("MODE"), work);
        for (const auto &sample : attitude) {
            work.tick(); const auto *previous = altitudes.previous(sample.line);
            const auto altitude = previous ? value(*previous, {"BarAlt", "Alt"}) : Maybe();
            if (!altitude || *altitude <= 2) continue;
            const auto *mode = modes.previous(sample.line);
            if (mode) {
                const QString text = textValue(*mode, "Mode").trimmed().toUpper();
                const auto number = value(*mode, {"ModeNum", "Mode"});
                const int n = number ? integer(roundEven(*number)) : -1;
                if (text == "ACRO" || text == "SPORT" || text == "FLIP" || text == "AUTOTUNE"
                    || n == 1 || n == 13 || n == 14 || n == 15) continue;
            }
            const double roll = std::abs(value(sample, {"Roll"}).value_or(0)), pitch = std::abs(value(sample, {"Pitch"}).value_or(0));
            const double max = maxDotNet(roll, pitch);
            if (max > limit) return result("Pitch/Roll", Fail,
                QStringLiteral("%1 %2° exceeds buffered lean limit %3° at line %4")
                    .arg(roll >= pitch ? QStringLiteral("roll") : QStringLiteral("pitch"), fixed(max, 2), fixed(limit, 2)).arg(sample.line));
        }
        return result("Pitch/Roll", Good, "airborne lean stayed within " + fixed(limit, 1) + QChar(0xb0));
    }
    Test thrust() {
        if (data.vehicleType != API::VehicleType::Copter) return result("Thrust", NA, "copter-only check");
        const auto &ctun = rows("CTUN"), &attitude = rows("ATT");
        if (ctun.isEmpty() || attitude.isEmpty()) return result("Thrust", Unknown, "CTUN or ATT data is missing");
        const auto available = values(ctun, {"ThrOut", "ThO"}, work);
        if (available.isEmpty()) return result("Thrust", Unknown, "throttle output column is missing");
        const double threshold = maximum(available) <= 1.5 ? 0.7 : 700;
        LineIndex rolls(attitude, work, {"Roll"}), pitches(attitude, work, {"Pitch"});
        std::optional<Test> worst; int count = 0; double throttleSum = 0, climbSum = 0;
        auto segment = [&] {
            if (count > 50) {
                const double climb = climbSum / count;
                const QString status = climb < 50 ? Fail : climb < 100 ? Warn : Good;
                if (!worst || worse(worst->status, status) == status) worst = result("Thrust", status,
                    "average climb " + fixed(climb, 1) + " cm/s at throttle " + fixed(throttleSum / count, 0));
            }
            count = 0; throttleSum = 0; climbSum = 0;
        };
        for (const auto &sample : ctun) {
            work.tick(); const auto throttle = value(sample, {"ThrOut", "ThO"}), climb = value(sample, {"CRate", "CRt"});
            const auto *r = rolls.nearest(sample.line), *p = pitches.nearest(sample.line);
            const double roll = std::abs(r ? value(*r, {"Roll"}).value_or(0) : 0), pitch = std::abs(p ? value(*p, {"Pitch"}).value_or(0) : 0);
            if (throttle && *throttle > threshold && climb && roll <= 20 && pitch <= 20) { ++count; throttleSum += *throttle; climbSum += *climb; }
            else segment();
        }
        segment(); return worst.value_or(result("Thrust", Good, "no sustained level high-throttle/low-climb interval"));
    }
    Test imuMismatch() {
        const auto &imu1 = rows("IMU"), &imu2 = rows("IMU2");
        if (!imu1.isEmpty() && imu2.isEmpty()) return result("IMU mismatch", NA, "no secondary IMU");
        if (imu1.isEmpty() || imu2.isEmpty()) return result("IMU mismatch", Unknown, "IMU/IMU2 accelerometer data is missing");
        auto time = [](const Sample &s) { return s.timeSeconds > 0 ? s.timeSeconds : s.line / 100.0; };
        int second = 0; double x = 0, y = 0, z = 0, max = 0; Maybe previous;
        for (const auto &first : imu1) {
            work.tick(); const auto ax = value(first, {"AccX"}), ay = value(first, {"AccY"}), az = value(first, {"AccZ"});
            if (!ax || !ay || !az) continue;
            const double now = time(first);
            while (second + 1 < imu2.size() && time(imu2[second + 1]) <= now) { work.tick(); ++second; }
            int nearest = second;
            if (second + 1 < imu2.size() && std::abs(time(imu2[second + 1]) - now) < std::abs(time(imu2[second]) - now)) nearest = second + 1;
            const auto bx = value(imu2[nearest], {"AccX"}), by = value(imu2[nearest], {"AccY"}), bz = value(imu2[nearest], {"AccZ"});
            if (!bx || !by || !bz) continue;
            const double delta = previous ? std::clamp(now - *previous, 0.0, 0.1) : 0;
            x += (*ax - *bx - x) * delta / 5; y += (*ay - *by - y) * delta / 5; z += (*az - *bz - z) * delta / 5;
            max = maxDotNet(max, std::sqrt(x * x + y * y + z * z)); previous = now;
        }
        return result("IMU mismatch", max > 1.5 ? Fail : max > 0.75 ? Warn : Good,
            "filtered accelerometer mismatch " + fixed(max, 2) + " m/s/s (warn 0.75, fail 1.50)");
    }
    Test autotune() {
        if (data.vehicleType != API::VehicleType::Copter) return result("Autotune", NA, "copter-only check");
        QVector<int> events;
        for (double v : values(rows("EV"), {"Id"}, work)) { const int n = integer(v); if (n >= 30 && n <= 37) events.append(n); }
        if (events.isEmpty()) return result("Autotune", NA, "no autotune session");
        if (rows("ATUN").isEmpty() && rows("ATDE").isEmpty()) return result("Autotune", Unknown, "autotune events exist but ATUN/ATDE data is missing");
        int sessions = 0, last = 0;
        for (int n : events) { if (n == 30) { ++sessions; last = 0; } else if (sessions && (n == 33 || n == 34 || n == 35)) last = n; }
        return result("Autotune", last == 33 ? Good : last == 34 || last == 35 ? Fail : Unknown,
            QStringLiteral("%1 session(s); last session %2").arg(sessions).arg(last == 33 ? "success" : last == 34 ? "failed" : last == 35 ? "reached limit" : "has no final result"));
    }
    Test opticalFlow() {
        const auto &flow = rows("OF"), &attitude = rows("ATT");
        if (flow.isEmpty()) return result("Optical flow", NA, "no optical-flow calibration data");
        if (attitude.isEmpty()) return result("Optical flow", Unknown, "OF data exists but ATT data is missing");
        bool roll = false, pitch = false;
        for (const auto &sample : attitude) { work.tick(); roll |= std::abs(value(sample, {"Roll"}).value_or(0)) > 15; pitch |= std::abs(value(sample, {"Pitch"}).value_or(0)) > 15; }
        if (!roll || !pitch) return result("Optical flow", Fail, "calibration requires both roll and pitch sweeps beyond 15°");
        QVector<QPair<double, double>> x, y;
        for (const auto &sample : flow) {
            work.tick(); const auto quality = value(sample, {"Qual"}); if (!quality || !(*quality > 124)) continue;
            const auto bx = value(sample, {"bodyX"}), fx = value(sample, {"flowX"}), by = value(sample, {"bodyY"}), fy = value(sample, {"flowY"});
            if (bx && fx && std::abs(*bx) > 0 && std::abs(*bx) < 2) x.append({*bx, *fx});
            if (by && fy && std::abs(*by) > 0 && std::abs(*by) < 2) y.append({*by, *fy});
        }
        if (x.size() < 100 || y.size() < 100) return result("Optical flow", Fail,
            QStringLiteral("insufficient high-quality samples (X=%1, Y=%2, need 100)").arg(x.size()).arg(y.size()));
        auto fit = [&](const QVector<QPair<double, double>> &points, double *slope, double *error) {
            double mx = 0, my = 0; for (const auto &p : points) { work.tick(); mx += p.first; my += p.second; }
            mx /= points.size(); my /= points.size();
            double sxx = 0, sxy = 0;
            for (const auto &p : points) { work.tick(); sxx += std::pow(p.first - mx, 2); sxy += (p.first - mx) * (p.second - my); }
            if (sxx <= Epsilon) return false;
            *slope = sxy / sxx; if (std::abs(*slope) <= Epsilon) return false;
            const double intercept = my - *slope * mx; double residual = 0;
            for (const auto &p : points) { work.tick(); residual += std::pow(p.second - (intercept + *slope * p.first), 2); }
            *error = std::sqrt(residual / std::max(1, points.size() - 2) / sxx); return true;
        };
        double sx = 0, sy = 0, ex = 0, ey = 0;
        if (!fit(x, &sx, &ex) || !fit(y, &sy, &ey)) return result("Optical flow", Fail, "optical-flow scale fit is degenerate");
        const int nx = integer(roundEven(1000 * ((1 + 0.001 * parameter(data, "FLOW_FXSCALER").value_or(0)) / sx - 1)));
        const int ny = integer(roundEven(1000 * ((1 + 0.001 * parameter(data, "FLOW_FYSCALER").value_or(0)) / sy - 1)));
        // Use wide absolute values: invalid fits must not overflow abs(INT_MIN).
        return result("Optical flow", std::abs(qint64(nx)) > 200 || std::abs(qint64(ny)) > 200 || 1000 * ex > 5 || 1000 * ey > 5 ? Fail : Good,
            QStringLiteral("recommended FLOW_FXSCALER=%1, FLOW_FYSCALER=%2; slope σ=%3/%4").arg(nx).arg(ny).arg(fixed(1000 * ex, 1), fixed(1000 * ey, 1)));
    }
};

void checkData(const API::Data &data, Work &work)
{
    if (data.lineCount < 0 || data.lineCount > std::numeric_limits<int>::max()
        || data.records.size() > 256 || data.parameters.size() > API::MaximumParameters)
        throw Interrupted{false, QStringLiteral("Analyzer model exceeds its line/group/parameter limits.")};
    qint64 sampleCount = 0, numericCount = 0, textBytes = 0;
    QSet<QString> types;
    auto name = [&](const QString &key) {
        if (key.isEmpty() || key.size() > 128) throw Interrupted{false, QStringLiteral("Analyzer field/type/parameter name exceeds its 128-character limit.")};
    };
    auto unique = [&](const auto &fields) {
        QSet<QString> names;
        for (const auto &field : fields) {
            work.tick(); name(field.first);
            const auto folded = field.first.toCaseFolded();
            if (names.contains(folded)) throw Interrupted{false, QStringLiteral("Analyzer model contains ambiguous duplicate names.")};
            names.insert(folded);
        }
    };
    unique(data.parameters);
    for (const auto &group : data.records) {
        name(group.type); const auto type = group.type.toCaseFolded();
        if (types.contains(type)) throw Interrupted{false, QStringLiteral("Analyzer model contains duplicate record groups.")};
        types.insert(type);
        sampleCount += group.samples.size();
        if (sampleCount > API::MaximumSamples) throw Interrupted{false, QStringLiteral("Analyzer selected-sample limit exceeded; no sampling was applied.")};
        for (const auto &sample : group.samples) {
            work.tick();
            if (sample.line < 0 || sample.line > std::numeric_limits<int>::max()
                || sample.values.size() > 64 || sample.textValues.size() > 64)
                throw Interrupted{false, QStringLiteral("Analyzer sample line/field limits exceeded.")};
            numericCount += sample.values.size(); unique(sample.values); unique(sample.textValues);
            for (const auto &text : sample.textValues) textBytes += qint64(text.second.size()) * 2;
            if (numericCount > API::MaximumNumericValues || textBytes > API::MaximumTextBytes)
                throw Interrupted{false, QStringLiteral("Analyzer numeric/text state limit exceeded; no records were dropped.")};
        }
    }
}
QVector<Test> runChecks(const API::Data &data, Work &work, int start)
{
    checkData(data, work);
    Checks checks(data, work);
    using Method = Test (Checks::*)();
    const Method methods[] = {&Checks::empty, &Checks::vibration, &Checks::gps, &Checks::vcc,
        &Checks::compass, &Checks::motorBalance, &Checks::nan, &Checks::events, &Checks::brownout,
        &Checks::duplicate, &Checks::parameters, &Checks::performance, &Checks::pitchRoll,
        &Checks::thrust, &Checks::imuMismatch, &Checks::autotune, &Checks::opticalFlow};
    QVector<Test> tests;
    for (int i = 0; i < 17; ++i) { work.check(); tests.append((checks.*methods[i])()); work.report(start + (1000 - start) * (i + 1) / 17); }
    return tests;
}
struct FieldGroup { const char *type; const char *fields; };
const FieldGroup SelectedFields[] = {
    {"ATT", "Roll,Pitch"}, {"ATDE", ""}, {"ATUN", "Axis,TuneStep,RateMin,RateMax,RPGain,RDGain,SPGain"},
    {"CTUN", "BarAlt,Alt,ThO,ThrOut,CRate,CRt"}, {"CURR", "Vcc"}, {"ERR", "Subsys,ECode"},
    {"EV", "Id"}, {"GPS", "NSats,NSat,numSV,HDop,HDp,EPH,Status"},
    {"GPS2", "NSats,NSat,numSV,HDop,HDp,EPH,Status"}, {"IMU", "AccX,AccY,AccZ"},
    {"IMU2", "AccX,AccY,AccZ"}, {"MAG", "OfsX,OfsY,OfsZ,MagX,MagY,MagZ"},
    {"MODE", "Mode,ModeNum"}, {"OF", "flowX,flowY,bodyX,bodyY,Qual"},
    {"PM", "NLon,NLoop,MaxT"}, {"POWR", "Vcc"}, {"RCOU", ""}, {"VIBE", "VibeX,VibeY,VibeZ"}
};
bool tryNumber(QString text, double *out)
{
    text = text.trimmed();
    const auto lower = text.toLower();
    if (lower == "nan" || lower == "+nan" || lower == "-nan") { *out = std::numeric_limits<double>::quiet_NaN(); return true; }
    if (lower == "infinity" || lower == "+infinity") { *out = std::numeric_limits<double>::infinity(); return true; }
    if (lower == "-infinity") { *out = -std::numeric_limits<double>::infinity(); return true; }
    if (lower == "inf" || lower == "+inf" || lower == "-inf") return false;
    // NumberStyles.AllowThousands with the invariant comma separator.
    text.remove(','); bool ok = false; const double parsed = text.toDouble(&ok);
    if (ok || std::isinf(parsed)) { *out = parsed; return true; }
    return false;
}
QString floatText(float value)
{
    if (!std::isfinite(value)) return fixed(value, 0);
    if (value == 0 && std::signbit(value)) return "-0";
    for (int digits = 1; digits <= 9; ++digits) {
        const QString text = QString::number(value, 'g', digits);
        if (text.toFloat() == value) return text;
    }
    return QString::number(value, 'g', 9);
}
double half(quint16 bits)
{
    const int exponent = (bits >> 10) & 31, fraction = bits & 1023;
    double value = exponent == 31 ? (fraction ? std::numeric_limits<double>::quiet_NaN() : std::numeric_limits<double>::infinity())
        : exponent == 0 ? std::ldexp(double(fraction), -24) : std::ldexp(double(1024 + fraction), exponent - 25);
    return bits & 0x8000 ? -value : value;
}
QString binaryField(const QByteArray &raw, const DataFlashRaw::Definition &d, int index)
{
    const auto *at = reinterpret_cast<const uchar *>(raw.constData() + d.offsets[index]);
    switch (d.format[index]) {
    case 'b': return QString::number(qint8(*at));
    case 'B': case 'M': return QString::number(*at);
    case 'h': return QString::number(qFromLittleEndian<qint16>(at));
    case 'H': return QString::number(qFromLittleEndian<quint16>(at));
    case 'i': return QString::number(qFromLittleEndian<qint32>(at));
    case 'I': return QString::number(qFromLittleEndian<quint32>(at));
    case 'q': return QString::number(qFromLittleEndian<qint64>(at));
    case 'Q': return QString::number(qFromLittleEndian<quint64>(at));
    case 'g': return floatText(float(half(qFromLittleEndian<quint16>(at))));
    case 'f': { const quint32 bits = qFromLittleEndian<quint32>(at); float v; std::memcpy(&v, &bits, sizeof(v)); return floatText(v); }
    case 'd': { const quint64 bits = qFromLittleEndian<quint64>(at); double v; std::memcpy(&v, &bits, sizeof(v)); return general(v); }
    case 'c': return general(qFromLittleEndian<qint16>(at) / 100.0);
    case 'C': return general(qFromLittleEndian<quint16>(at) / 100.0);
    case 'e': return general(qFromLittleEndian<qint32>(at) / 100.0);
    case 'E': return general(qFromLittleEndian<quint32>(at) / 100.0);
    case 'L': return general(qFromLittleEndian<qint32>(at) / 10000000.0);
    case 'n': case 'N': case 'Z': {
        QByteArray text = raw.mid(d.offsets[index], DataFlashRaw::width(d.format[index]));
        while (!text.isEmpty() && text.front() == '\0') text.remove(0, 1);
        while (!text.isEmpty() && text.back() == '\0') text.chop(1);
        for (char &c : text) if (quint8(c) > 127) c = '?';
        return QString::fromLatin1(text);
    }
    case 'a': return QStringLiteral("System.Int16[]"); // DFItem.items: array ToString, not scalar data.
    default: throw Interrupted{false, QStringLiteral("Selected analyzer field uses unsupported DataFlash encoding %1.").arg(QChar(d.format[index]))};
    }
}
struct SourceStamp {
    QString requested, canonical;
    qint64 size;
    QDateTime modified, born;
    bool current() const {
        const QFileInfo info(requested);
        return info.isFile() && !info.isSymLink() && info.canonicalFilePath() == canonical && info.size() == size
            && info.lastModified() == modified && (!born.isValid() || info.birthTime() == born);
    }
};
API::Data load(const QString &path, Work &work, QStringList *warnings)
{
    const QFileInfo info(path);
    if (path.trimmed().isEmpty() || !info.isFile() || info.isSymLink() || info.size() > API::MaximumInputBytes)
        throw Interrupted{false, QStringLiteral("Choose a regular non-symlink BIN/LOG file no larger than 1 GiB.")};
    const QString suffix = info.suffix().toLower();
    if (suffix != "bin" && suffix != "log") throw Interrupted{false, QStringLiteral("Auto Analysis accepts DataFlash .bin or .log files.")};
    const SourceStamp before{info.absoluteFilePath(), info.canonicalFilePath(), info.size(), info.lastModified(), info.birthTime()};
    QFile file(before.canonical);
    if (!file.open(QIODevice::ReadOnly)) throw Interrupted{false, file.errorString()};
    const bool text = suffix == "log";
    DataFlashRaw::Reader reader(&file, text);
    API::Data data;
    QVector<QStringList> fields;
    QHash<QString, int> groupIndex, parameterIndex;
    for (const auto &selected : SelectedFields) {
        const QString type = QString::fromLatin1(selected.type);
        groupIndex.insert(type.toCaseFolded(), data.records.size()); data.records.append({type, {}});
        auto names = QString::fromLatin1(selected.fields).split(',', Qt::SkipEmptyParts);
        if (type == "RCOU") for (int i = 1; i <= 16; ++i) for (const char *prefix : {"C", "Ch", "Chan"}) names.append(QString::fromLatin1(prefix) + QString::number(i));
        fields.append(names);
    }
    qint64 samples = 0, numeric = 0, textBytes = 0, firmwareRows = 0;
    QByteArray firmware;
    QVector<int> numericModeRows;
    QByteArray raw; int kind = 0;
    while (reader.next(&raw, &kind)) {
        work.tick();
        if (file.pos() > before.size) throw Interrupted{false, QStringLiteral("DataFlash source grew during analysis.")};
        if (kind == -2) continue;
        const qint64 line = data.lineCount++;
        if (data.lineCount > std::numeric_limits<int>::max()) throw Interrupted{false, QStringLiteral("DataFlash line index exceeds Int32 reference limits.")};
        if (kind >= 0) continue;
        const auto &d = reader.currentDefinition;
        const QString type = QString::fromLatin1(d.name);
        const bool parm = type.compare("PARM", Qt::CaseInsensitive) == 0, msg = type.compare("MSG", Qt::CaseInsensitive) == 0;
        const auto group = groupIndex.constFind(type.toCaseFolded());
        if (!parm && !msg && group == groupIndex.constEnd()) continue;
        QList<QByteArray> textFields;
        if (text) {
            textFields = DataFlashRaw::textValues(raw, d);
            if (textFields.size() != d.columns.size()) throw Interrupted{false, QStringLiteral("Text analyzer record does not match its declared FMT layout.")};
        }
        auto column = [&](const QString &name) {
            for (int i = 0; i < d.columns.size(); ++i) if (QString::fromLatin1(d.columns[i]).compare(name, Qt::CaseInsensitive) == 0) return i;
            return -1;
        };
        auto field = [&](const QString &name) {
            const int index = column(name);
            return index < 0 ? QString() : text ? QString::fromUtf8(textFields[index]).trimmed() : binaryField(raw, d, index).trimmed();
        };
        if (parm || msg) {
            const QString parameterName = parm ? field("Name") : QString();
            QString message;
            if (msg) { // Null-coalescing aliases, not fallback on empty existing text.
                message = column("Message") >= 0 ? field("Message") : column("Msg") >= 0 ? field("Msg") : field("Text");
                if (message.contains("Copter", Qt::CaseInsensitive)) data.vehicleType = API::VehicleType::Copter;
                else if (message.contains("Plane", Qt::CaseInsensitive)) data.vehicleType = API::VehicleType::Plane;
                else if (message.contains("Rover", Qt::CaseInsensitive)) data.vehicleType = API::VehicleType::Rover;
            } else {
                double value;
                if (!parameterName.isEmpty() && tryNumber(field("Value"), &value)) {
                    if (parameterName.size() > 128) throw Interrupted{false, QStringLiteral("Parameter name exceeds 128 characters.")};
                    auto existing = parameterIndex.constFind(parameterName.toCaseFolded());
                    if (existing == parameterIndex.constEnd()) {
                        if (data.parameters.size() >= API::MaximumParameters) throw Interrupted{false, QStringLiteral("Analyzer parameter limit exceeded.")};
                        parameterIndex.insert(parameterName.toCaseFolded(), data.parameters.size());
                        data.parameters.append({parameterName, value});
                    } else data.parameters[*existing].second = value;
                }
            }
            // DFLogBuffer's BinaryLog prescan determines M-mode rendering;
            // it is separate from LogAnalyzer's final MSG vehicle detection.
            if (!text && firmwareRows++ <= 100000) {
                if (message.contains("ArduCopter") || message.contains("Copter") || parameterName == "RATE_RLL_P" || parameterName == "H_SWASH_PLATE") firmware = "ArduCopter2";
                else if (message.contains("ArduPlane") || message.contains("Plane") || parameterName == "PTCH2SRV_P") firmware = "ArduPlane";
                else if (message.contains("ArduRover") || message.contains("Rover") || parameterName == "SKID_STEER_OUT") firmware = "ArduRover";
                else if (message.contains("AntennaTracker") || message.contains("Tracker")) firmware = "ArduTracker";
            }
            continue;
        }
        if (++samples > API::MaximumSamples) throw Interrupted{false, QStringLiteral("Analyzer selected-sample limit exceeded; no sampling was applied.")};
        Sample sample; sample.line = line;
        for (const QString &name : fields[*group]) {
            work.tick(); const QString rawValue = field(name); if (rawValue.isEmpty()) continue;
            textBytes += qint64(rawValue.size()) * 2;
            if (textBytes > API::MaximumTextBytes) throw Interrupted{false, QStringLiteral("Analyzer retained-text limit exceeded; no records were dropped.")};
            sample.textValues.append({name, rawValue});
            double number;
            if (tryNumber(rawValue, &number)) {
                if (++numeric > API::MaximumNumericValues) throw Interrupted{false, QStringLiteral("Analyzer numeric-value limit exceeded; no records were dropped.")};
                sample.values.append({name, number});
            }
        }
        const QString timeName = column("TimeMS") >= 0 ? QStringLiteral("TimeMS") : column("TimeUS") >= 0 ? QStringLiteral("TimeUS") : QStringLiteral("T");
        bool timeOk = false; const qint64 timestamp = field(timeName).toLongLong(&timeOk);
        if (timeOk) sample.timeSeconds = timeName == "TimeUS" ? (timestamp / 1000.0) / 1000.0 : timestamp / 1000.0;
        const int modeIndex = column("Mode");
        if (!text && type == "MODE" && modeIndex >= 0 && d.format[modeIndex] == 'M') numericModeRows.append(data.records[*group].samples.size());
        data.records[*group].samples.append(std::move(sample));
        if ((samples & 1023) == 0) {
            work.report(before.size ? file.pos() * 650 / before.size : 650);
            if (!before.current()) throw Interrupted{false, QStringLiteral("DataFlash source changed during analysis.")};
        }
    }
    if (!reader.error.isEmpty()) throw Interrupted{false, reader.error};
    if (file.error() != QFile::NoError) throw Interrupted{false, file.errorString()};
    work.report(650);
    if (!before.current()) throw Interrupted{false, QStringLiteral("DataFlash source changed during analysis.")};
    const auto inputHash = reader.hash.result();
    if (!file.seek(0)) throw Interrupted{false, file.errorString()};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        work.check(); const auto chunk = file.read(65536);
        if (chunk.isEmpty() && file.error() != QFile::NoError) throw Interrupted{false, file.errorString()};
        if (file.pos() > before.size) throw Interrupted{false, QStringLiteral("DataFlash source grew during verification.")};
        hash.addData(chunk); work.report(650 + (before.size ? file.pos() * 100 / before.size : 100));
    }
    if (file.pos() != before.size || hash.result() != inputHash || !before.current()) throw Interrupted{false, QStringLiteral("DataFlash source content changed during analysis.")};
    if (reader.blankLines) warnings->append(QStringLiteral("Ignored %1 blank text line(s); no measurement sampling was applied.").arg(reader.blankLines));
    if (reader.trailingBytes) warnings->append(QStringLiteral("Ignored %1-byte incomplete final %2 record; results describe only complete records.")
        .arg(reader.trailingBytes).arg(reader.trailingType));
    if (data.vehicleType == API::VehicleType::Unknown && parameter(data, "FRAME_CLASS")) data.vehicleType = API::VehicleType::Copter;
    auto &modes = data.records[groupIndex.value("mode")].samples;
    for (int index : numericModeRows) {
        work.tick(); auto &sample = modes[index]; const auto number = value(sample, {"Mode"});
        if (!number) continue;
        const QString resolved = DataFlashModeNames::resolve(firmware, integer(*number));
        if (resolved.isEmpty()) continue;
        for (auto &text : sample.textValues) if (text.first == "Mode") text.second = resolved;
        for (int i = 0; i < sample.values.size(); ++i) if (sample.values[i].first == "Mode") { sample.values.removeAt(i); break; }
    }
    work.report(750);
    return data;
}
}

QString DataFlashLogAnalyzer::Classify(double value, double warn, double fail, bool higherWorse)
{
    if (std::isnan(value)) return NA;
    return higherWorse ? value >= fail ? Fail : value >= warn ? Warn : Good
        : value <= fail ? Fail : value <= warn ? Warn : Good;
}

QString DataFlashLogAnalyzer::Format(const QVector<TestResult> &tests)
{
    QStringList lines;
    for (const auto &test : tests) lines.append('[' + test.status + "] " + test.name + ": " + test.message);
    return lines.join('\n');
}

DataFlashLogAnalyzer::Result DataFlashLogAnalyzer::Analyze(const Data &requested, Cancel cancel, Progress progress)
{
    const Data data = requested; // Pin implicitly shared containers before callbacks.
    Result output; Work work{std::move(cancel), std::move(progress)};
    try { work.report(0); output.tests = runChecks(data, work, 0); output.success = true; }
    catch (const Interrupted &stop) { output.cancelled = stop.cancelled; output.error = stop.error; output.tests.clear(); }
    catch (const std::bad_alloc &) { output.error = QStringLiteral("Not enough memory for bounded log analysis."); output.tests.clear(); }
    return output;
}

DataFlashLogAnalyzer::Result DataFlashLogAnalyzer::Analyze(const QString &requested, Cancel cancel, Progress progress)
{
    const QString path = requested;
    Result output; Work work{std::move(cancel), std::move(progress)};
    try {
        work.report(0); const auto data = load(path, work, &output.warnings);
        output.tests = runChecks(data, work, 750); output.success = true;
    } catch (const Interrupted &stop) { output.cancelled = stop.cancelled; output.error = stop.error; output.tests.clear(); }
    catch (const std::bad_alloc &) { output.error = QStringLiteral("Not enough memory for bounded log analysis."); output.tests.clear(); }
    return output;
}
