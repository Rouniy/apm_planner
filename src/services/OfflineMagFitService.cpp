#include "OfflineMagFitService.h"

#include "comm/TlogReader.h"
#include "ui/Loghandling/DataFlashRawReader.h"
#include "optimization.h"

#include <QDateTime>
#include <QFileInfo>
#include <QMap>
#include <QSet>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>
#include <tuple>

namespace {
struct Cancelled {};
using Cancel = std::function<bool()>;
void check(const Cancel &cancel) { if (cancel && cancel()) throw Cancelled{}; }
bool finite(const MagVector &v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
double length(const MagVector &v) { return std::hypot(v.x, std::hypot(v.y, v.z)); }
void require(bool condition, const QString &error)
{
    if (!condition) throw std::runtime_error(error.toStdString());
}
void normalize(alglib::real_1d_array &p)
{
    const double norm = length({p[3], p[4], p[5]});
    if (norm > 0 && std::isfinite(norm)) {
        const double scale = std::sqrt(3.0) / norm;
        p[3] *= scale; p[4] *= scale; p[5] *= scale;
    }
}
double residual(const alglib::real_1d_array &p, const MagVector &sample, double radius)
{
    const MagVector v{sample.x + p[0], sample.y + p[1], sample.z + p[2]};
    if (p.length() == 4) return p[3] - length(v);
    double dx = p[3], dy = p[4], dz = p[5];
    const double norm = length({dx, dy, dz});
    if (norm > 0 && std::isfinite(norm)) {
        const double scale = std::sqrt(3.0) / norm;
        dx *= scale; dy *= scale; dz *= scale;
    }
    const double xy = p.length() == 9 ? p[6] : 0;
    const double xz = p.length() == 9 ? p[7] : 0;
    const double yz = p.length() == 9 ? p[8] : 0;
    return radius - length({dx * v.x + xy * v.y + xz * v.z,
                            xy * v.x + dy * v.y + yz * v.z,
                            xz * v.x + yz * v.y + dz * v.z});
}
alglib::real_1d_array solve(const QVector<MagVector> &samples,
                          std::initializer_list<double> initial, double radius,
                          const Cancel &cancel)
{
    check(cancel);
    alglib::real_1d_array parameters;
    parameters.setcontent(int(initial.size()), initial.begin());
    alglib::minlmstate state;
    alglib::minlmcreatev(samples.size(), parameters, 0.1, state);
    alglib::minlmsetcond(state, 0, 0, 0, 100);
    alglib::minlmsetxrep(state, true);
    // Reverse communication is the same loop as minlmoptimize(), but lets
    // cancellation unwind outside ALGLIB's C callback wrapper. All solver
    // state is stack-owned and released, including cancellation mid-residual.
    while (alglib::minlmiteration(state)) {
        check(cancel);
        if (state.needfi) {
            for (int i = 0; i < samples.size(); ++i) {
                if ((i & 255) == 0) check(cancel);
                const double value = residual(state.x, samples[i], radius);
                require(std::isfinite(value), QStringLiteral("MagFit solver produced a non-finite residual."));
                state.fi[i] = value;
            }
        } else {
            require(state.xupdated, QStringLiteral("Unexpected MagFit solver callback request."));
        }
    }
    check(cancel);
    alglib::minlmreport report;
    alglib::minlmresults(state, parameters, report);
    require(report.terminationtype > 0,
            QStringLiteral("MagFit solver did not converge (termination %1).").arg(report.terminationtype));
    for (int i = 0; i < parameters.length(); ++i)
        require(std::isfinite(parameters[i]), QStringLiteral("MagFit solver returned non-finite parameters."));
    return parameters;
}
double rms(const QVector<MagVector> &samples, const alglib::real_1d_array &p,
           double radius, const Cancel &cancel)
{
    double sum = 0;
    for (int i = 0; i < samples.size(); ++i) {
        if ((i & 255) == 0) check(cancel);
        const double value = residual(p, samples[i], radius);
        sum += value * value;
    }
    return std::sqrt(sum / samples.size());
}
OfflineMagFitResult fit(int compass, const QVector<MagVector> &samples, int sourceSamples,
                        const MagVector &loggedOffsets, bool ellipsoid, const Cancel &cancel)
{
    check(cancel);
    require(compass >= 1 && compass <= 3, QStringLiteral("Compass must be 1, 2 or 3."));
    require(samples.size() >= 10, QStringLiteral("At least 10 magnetometer samples are required."));
    require(samples.size() <= OfflineMagFitService::MaximumSamplesPerCompass
                && sourceSamples >= samples.size()
                && sourceSamples <= OfflineMagFitService::MaximumSamplesPerCompass,
            QStringLiteral("Invalid or excessive magnetometer sample count."));
    require(finite(loggedOffsets), QStringLiteral("Logged offsets contain NaN or infinity."));
    double meanRadius = 0;
    for (int i = 0; i < samples.size(); ++i) {
        if ((i & 255) == 0) check(cancel);
        require(finite(samples[i]), QStringLiteral("Magnetometer samples contain NaN or infinity."));
        meanRadius += length(samples[i]) / samples.size();
    }
    require(std::isfinite(meanRadius) && meanRadius > 0,
            QStringLiteral("Magnetometer samples have no measurable finite radius."));
    const auto sphere = solve(samples, {0, 0, 0, meanRadius}, meanRadius, cancel);
    OfflineMagFitResult result;
    result.compass = compass; result.sourceSamples = sourceSamples; result.usedSamples = samples.size();
    result.loggedOffsets = loggedOffsets;
    result.sphereOffsets = {sphere[0], sphere[1], sphere[2]};
    result.offsets = result.sphereOffsets;
    result.sphereRadius = sphere[3];
    result.sphereRmsError = result.rmsError = rms(samples, sphere, meanRadius, cancel);
    if (ellipsoid) {
        auto diagonal = solve(samples, {sphere[0], sphere[1], sphere[2], 1, 1, 1}, meanRadius, cancel);
        normalize(diagonal);
        auto full = solve(samples, {diagonal[0], diagonal[1], diagonal[2],
                                   diagonal[3], diagonal[4], diagonal[5], 0, 0, 0}, meanRadius, cancel);
        normalize(full);
        result.offsets = {full[0], full[1], full[2]};
        result.diagonals = {full[3], full[4], full[5]};
        result.offDiagonals = {full[6], full[7], full[8]};
        result.rmsError = rms(samples, full, meanRadius, cancel);
    }
    require(finite(result.offsets) && finite(result.diagonals) && finite(result.offDiagonals)
                && std::isfinite(result.rmsError) && std::isfinite(result.sphereRmsError),
            QStringLiteral("MagFit did not converge to finite calibration values."));
    result.hasEllipsoid = ellipsoid;
    unsigned octants = 0;
    for (int i = 0; i < samples.size(); ++i) {
        if ((i & 255) == 0) check(cancel);
        const auto &v = samples[i];
        const int octant = (v.x + result.offsets.x >= 0 ? 1 : 0)
            | (v.y + result.offsets.y >= 0 ? 2 : 0) | (v.z + result.offsets.z >= 0 ? 4 : 0);
        octants |= 1u << octant;
    }
    for (int i = 0; i < 8; ++i) result.coverageOctants += (octants >> i) & 1u;
    check(cancel);
    return result;
}

struct SampleSet { QVector<MagVector> samples; MagVector offsets; };
using Sets = QMap<int, SampleSet>;
void add(Sets &sets, int compass, const MagVector &value, const MagVector &offsets, qint64 &skipped)
{
    if (!finite(value) || (value.x == 0 && value.y == 0 && value.z == 0)) { ++skipped; return; }
    auto &set = sets[compass];
    require(set.samples.size() < OfflineMagFitService::MaximumSamplesPerCompass,
            QStringLiteral("Compass %1 exceeds the safe limit of %2 samples.")
                .arg(compass).arg(OfflineMagFitService::MaximumSamplesPerCompass));
    set.samples.append(value); set.offsets = offsets;
}
double half(quint16 bits)
{
    const int exponent = (bits >> 10) & 31, fraction = bits & 1023;
    double value = exponent == 31 ? (fraction ? std::numeric_limits<double>::quiet_NaN()
                                              : std::numeric_limits<double>::infinity())
        : exponent == 0 ? std::ldexp(double(fraction), -24)
                        : std::ldexp(double(1024 + fraction), exponent - 25);
    return bits & 0x8000 ? -value : value;
}
bool scalar(const QByteArray &raw, const DataFlashRaw::Definition &d,
            const QList<QByteArray> &textValues, bool text, int index, double *value)
{
    if (index < 0) return false;
    if (text) {
        bool ok = false; *value = textValues[index].trimmed().toDouble(&ok);
        return ok && std::isfinite(*value);
    }
    const auto *at = reinterpret_cast<const uchar *>(raw.constData() + d.offsets[index]);
    switch (d.format[index]) {
    case 'b': *value = qint8(*at); break;
    case 'B': case 'M': *value = *at; break;
    case 'h': *value = qFromLittleEndian<qint16>(at); break;
    case 'H': *value = qFromLittleEndian<quint16>(at); break;
    case 'i': *value = qFromLittleEndian<qint32>(at); break;
    case 'I': *value = qFromLittleEndian<quint32>(at); break;
    case 'q': *value = double(qFromLittleEndian<qint64>(at)); break;
    case 'Q': *value = double(qFromLittleEndian<quint64>(at)); break;
    case 'g': *value = half(qFromLittleEndian<quint16>(at)); break;
    case 'f': {
        const quint32 bits = qFromLittleEndian<quint32>(at); float number = 0;
        std::memcpy(&number, &bits, sizeof(number)); *value = number; break;
    }
    case 'd': {
        const quint64 bits = qFromLittleEndian<quint64>(at);
        std::memcpy(value, &bits, sizeof(*value)); break;
    }
    case 'c': *value = qFromLittleEndian<qint16>(at) / 100.0; break;
    case 'C': *value = qFromLittleEndian<quint16>(at) / 100.0; break;
    case 'e': *value = qFromLittleEndian<qint32>(at) / 100.0; break;
    case 'E': *value = qFromLittleEndian<quint32>(at) / 100.0; break;
    case 'L': *value = qFromLittleEndian<qint32>(at) / 10000000.0; break;
    default: return false;
    }
    return std::isfinite(*value);
}
bool field(const QByteArray &raw, const DataFlashRaw::Definition &d,
           const QList<QByteArray> &values, bool text,
           std::initializer_list<const char *> aliases, double *value)
{
    for (const char *alias : aliases)
        if (scalar(raw, d, values, text, d.columns.indexOf(alias), value)) return true;
    return false;
}

struct Provenance {
    QSet<QByteArray> relevant;
    QHash<QByteArray, double> parameters;
    QSet<QByteArray> invalid;
    QStringList reasons;
    QMap<int, quint32> deviceIds;
    QMap<QString, double> frameParameters;

    Provenance() {
        relevant.insert("AHRS_ORIENTATION");
        for (int compass = 1; compass <= 3; ++compass) {
            const QByteArray suffix = compass == 1 ? QByteArray() : QByteArray::number(compass);
            for (const auto &family : {QByteArray("DIA"), QByteArray("ODI")})
                for (char axis : QByteArray("XYZ"))
                    relevant.insert("COMPASS_" + family + suffix + '_' + axis);
            relevant.insert("COMPASS_SCALE" + suffix);
            relevant.insert("COMPASS_DEV_ID" + suffix);
            relevant.insert("COMPASS_PRIO" + QByteArray::number(compass) + "_ID");
            relevant.insert("COMPASS_ORIENT" + suffix);
            relevant.insert(compass == 1 ? QByteArray("COMPASS_EXTERNAL") : "COMPASS_EXTERN" + suffix);
        }
    }
    void block(const QString &reason) {
        if (!reasons.contains(reason)) reasons.append(reason);
    }
    void observeParameter(const QByteArray &raw, const DataFlashRaw::Definition &d,
                          const QList<QByteArray> &values, bool text) {
        const int nameIndex = d.columns.indexOf("Name");
        const int valueIndex = d.columns.indexOf("Value");
        if (nameIndex < 0 || valueIndex < 0) return;
        QByteArray name;
        if (text) name = values[nameIndex].trimmed();
        else if (QByteArray("nNZ").contains(d.format[nameIndex]))
            name = DataFlashRaw::ascii(raw.mid(d.offsets[nameIndex], DataFlashRaw::width(d.format[nameIndex])));
        if (!relevant.contains(name)) return;
        double value = 0;
        if (!scalar(raw, d, values, text, valueIndex, &value)) {
            invalid.insert(name);
            block(QStringLiteral("Relevant logged compass parameters contain invalid/non-finite values."));
            return;
        }
        if (parameters.contains(name) && parameters.value(name) != value) {
            invalid.insert(name);
            block(QStringLiteral("Relevant logged compass parameters changed during the log."));
        }
        parameters.insert(name, value);
    }
    bool value(const QByteArray &name, double *out) const {
        if (!parameters.contains(name) || invalid.contains(name)) return false;
        *out = parameters.value(name); return true;
    }
    void frameValue(const QByteArray &name, int maximum) {
        double recorded = 0;
        if (!value(name, &recorded)) {
            block(QStringLiteral("Missing explicit stable prior frame parameter %1.").arg(QString::fromLatin1(name)));
        } else if (recorded < 0 || recorded > maximum || std::floor(recorded) != recorded) {
            block(QStringLiteral("Frame parameter %1 is custom, invalid or unsupported; frame reconstruction is not implemented.")
                      .arg(QString::fromLatin1(name)));
        } else {
            frameParameters.insert(QString::fromLatin1(name), recorded);
        }
    }
    void sample(int compass, bool hasOffsets, bool hasMotorOffsets, const MagVector &motor, bool healthy) {
        if (!healthy) block(QStringLiteral("Accepted MAG samples lack explicit Health=1; calibration health is not established."));
        if (!hasOffsets) block(QStringLiteral("Accepted MAG samples lack complete finite logged offsets."));
        if (!hasMotorOffsets) block(QStringLiteral("Accepted MAG samples lack explicit finite MOX/MOY/MOZ motor offsets."));
        else if (motor.x != 0 || motor.y != 0 || motor.z != 0)
            block(QStringLiteral("Accepted MAG samples have nonzero motor compensation; raw reconstruction is not implemented."));

        const QByteArray suffix = compass == 1 ? QByteArray() : QByteArray::number(compass);
        frameValue("AHRS_ORIENTATION", 43);
        frameValue("COMPASS_ORIENT" + suffix, 43);
        frameValue(compass == 1 ? QByteArray("COMPASS_EXTERNAL") : "COMPASS_EXTERN" + suffix, 2);
        MagVector dia, odi;
        const bool haveDia = value("COMPASS_DIA" + suffix + "_X", &dia.x)
            && value("COMPASS_DIA" + suffix + "_Y", &dia.y)
            && value("COMPASS_DIA" + suffix + "_Z", &dia.z);
        const bool haveOdi = value("COMPASS_ODI" + suffix + "_X", &odi.x)
            && value("COMPASS_ODI" + suffix + "_Y", &odi.y)
            && value("COMPASS_ODI" + suffix + "_Z", &odi.z);
        double scale = 0, device = 0;
        const bool haveScale = value("COMPASS_SCALE" + suffix, &scale);
        const bool haveDevice = value("COMPASS_DEV_ID" + suffix, &device);
        if (!haveDia || !haveOdi || !haveScale || !haveDevice)
            block(QStringLiteral("Compass %1 lacks explicit, stable prior PARM values for DIA/ODI/SCALE/DEV_ID; missing defaults are not proof.").arg(compass));
        if (haveDia && !((dia.x == 1 && dia.y == 1 && dia.z == 1)
                        || (dia.x == 0 && dia.y == 0 && dia.z == 0)))
            block(QStringLiteral("Compass %1 has nonidentity DIA correction; raw reconstruction is not implemented.").arg(compass));
        if (haveOdi && (odi.x != 0 || odi.y != 0 || odi.z != 0))
            block(QStringLiteral("Compass %1 has nonzero ODI correction; raw reconstruction is not implemented.").arg(compass));
        if (haveScale && scale != 0 && scale != 1)
            block(QStringLiteral("Compass %1 has nonidentity scale correction; raw reconstruction is not implemented.").arg(compass));
        if (haveDevice) {
            if (device < 1 || device > std::numeric_limits<quint32>::max() || std::floor(device) != device)
                block(QStringLiteral("Compass %1 has no exact nonzero integer device ID.").arg(compass));
            else deviceIds.insert(compass, quint32(device));
        }
    }
    void finish(OfflineMagFitReport &report) {
        // Also examine priority values first logged after MAG samples: never
        // infer a remapping from a different ordering or silently switch slots.
        for (auto it = deviceIds.cbegin(); it != deviceIds.cend(); ++it) {
            const QByteArray name = "COMPASS_PRIO" + QByteArray::number(it.key()) + "_ID";
            if (invalid.contains(name)) {
                block(QStringLiteral("Compass priority metadata is invalid or changed."));
                continue;
            }
            if (!parameters.contains(name)) continue;
            const double priority = parameters.value(name);
            if (priority != 0 && priority != double(it.value()))
                block(QStringLiteral("Compass %1 priority/device-ID ordering disagrees; remapping is not supported.").arg(it.key()));
        }
        report.loggedDeviceIds = deviceIds;
        report.loggedFrameParameters = frameParameters;
        // readDataFlash precedes fitting; analyze clears eligibility on every
        // later failure, including an empty or insufficient sample set.
        report.applyEligible = reasons.isEmpty() && !deviceIds.isEmpty();
        report.applyUnavailableReason = reasons.join(' ');
        if (!reasons.isEmpty()) report.warnings << QStringLiteral("Apply unavailable: ") + report.applyUnavailableReason;
    }
};

void readDataFlash(QFile &file, bool text, Sets &sets, OfflineMagFitReport &report,
                   const OfflineMagFitService::Options &options)
{
    DataFlashRaw::Reader reader(&file, text);
    QByteArray raw; int kind = 0;
    qint64 records = 0, skipped = 0, missingOffsets = 0;
    Provenance provenance;
    while (true) {
        check(options.isCancelled);
        if (!reader.next(&raw, &kind)) break;
        if (kind > 0) {
            qint64 metadataBytes = 0;
            for (auto it = reader.metadataValues.cbegin(); it != reader.metadataValues.cend(); ++it)
                metadataBytes += it.key().size() + it.value().size();
            require(metadataBytes <= 16 * 1024 * 1024,
                    QStringLiteral("DataFlash metadata exceeds the 16 MiB safety limit."));
        }
        if ((++records & 1023) == 0 && options.progress)
            options.progress(0.7 * double(file.pos()) / qMax<qint64>(1, file.size()));
        if (kind != -1) continue;
        const auto &d = reader.currentDefinition;
        if (d.name == "PARM") {
            provenance.observeParameter(raw, d, text ? DataFlashRaw::textValues(raw, d) : QList<QByteArray>(), text);
            continue;
        }
        if (d.name != "MAG" && d.name != "MAG2" && d.name != "MAG3") continue;
        const auto values = text ? DataFlashRaw::textValues(raw, d) : QList<QByteArray>();
        MagVector v, offsets;
        if (!field(raw, d, values, text, {"MagX", "X"}, &v.x)
            || !field(raw, d, values, text, {"MagY", "Y"}, &v.y)
            || !field(raw, d, values, text, {"MagZ", "Z"}, &v.z)) { ++skipped; continue; }
        bool missing = false;
        if (!field(raw, d, values, text, {"OfsX", "OX"}, &offsets.x)) { offsets.x = 0; missing = true; }
        if (!field(raw, d, values, text, {"OfsY", "OY"}, &offsets.y)) { offsets.y = 0; missing = true; }
        if (!field(raw, d, values, text, {"OfsZ", "OZ"}, &offsets.z)) { offsets.z = 0; missing = true; }
        if (missing) ++missingOffsets;
        int compass = d.name == "MAG2" ? 2 : d.name == "MAG3" ? 3 : 1;
        if (d.name == "MAG") {
            bool unsafeInstance = false, found = false;
            for (const char *alias : {"I", "Instance", "C"}) {
                const int index = d.columns.indexOf(alias);
                if (index < 0) continue;
                double instance = 0;
                if (!scalar(raw, d, values, text, index, &instance) || instance < 0 || instance > 2
                    || std::abs(instance - std::round(instance)) >= 0.001) { unsafeInstance = true; break; }
                const int candidate = int(std::round(instance)) + 1;
                if (found && compass != candidate) { unsafeInstance = true; break; }
                compass = candidate; found = true;
            }
            if (unsafeInstance) {
                ++skipped;
                provenance.block(QStringLiteral("Skipped MAG records with invalid or conflicting compass instances; sensor identity is incomplete."));
                continue;
            }
        }
        const MagVector sample{v.x - offsets.x, v.y - offsets.y, v.z - offsets.z};
        if (finite(sample) && (sample.x != 0 || sample.y != 0 || sample.z != 0)) {
            MagVector motor;
            const bool haveMotor = field(raw, d, values, text, {"MOX"}, &motor.x)
                && field(raw, d, values, text, {"MOY"}, &motor.y)
                && field(raw, d, values, text, {"MOZ"}, &motor.z);
            double health = 0;
            const bool healthy = field(raw, d, values, text, {"Health"}, &health) && health == 1;
            provenance.sample(compass, !missing, haveMotor, motor, healthy);
        }
        add(sets, compass, sample, offsets, skipped);
    }
    require(reader.error.isEmpty(), reader.error);
    require(file.error() == QFileDevice::NoError, file.errorString());
    if (reader.blankLines) report.warnings << QStringLiteral("Ignored %1 blank DataFlash lines.").arg(reader.blankLines);
    if (reader.trailingBytes) report.warnings << QStringLiteral("Ignored incomplete final DataFlash record: %1 bytes, type %2.")
        .arg(reader.trailingBytes).arg(reader.trailingType);
    if (skipped) report.warnings << QStringLiteral("Skipped %1 missing, non-finite or zero magnetometer samples.").arg(skipped);
    if (missingOffsets) report.warnings << QStringLiteral("Used zero for missing/non-finite logged offset axes in %1 samples.").arg(missingOffsets);
    provenance.finish(report);
}
bool validLength(const mavlink_message_t &m, int minimum, int maximum)
{
    return m.magic == MAVLINK_STX_MAVLINK1 ? m.len == minimum : m.len > 0 && m.len <= maximum;
}
void readTelemetry(QFile &file, Sets &sets, OfflineMagFitReport &report,
                   const OfflineMagFitService::Options &options)
{
    struct State { MagVector offsets; bool useData = false; };
    QHash<int, State> senders;
    int magSender = -1;
    qint64 skipped = 0, malformed = 0, count = 0;
    TlogReader reader(&file);
    reader.setCancelCheck(options.isCancelled);
    TlogRecord record;
    while (reader.next(&record) == TlogReader::Status::Ok) {
        check(options.isCancelled);
        if ((++count & 511) == 0 && options.progress)
            options.progress(0.7 * double(reader.bytesProcessed()) / qMax<qint64>(1, reader.bytesTotal()));
        const auto &m = record.message;
        const bool magnetic = m.msgid == MAVLINK_MSG_ID_RAW_IMU || m.msgid == MAVLINK_MSG_ID_SCALED_IMU2
            || m.msgid == MAVLINK_MSG_ID_SCALED_IMU3;
        const int sender = (int(m.sysid) << 8) | m.compid;
        if (magnetic) {
            require(magSender < 0 || magSender == sender,
                    QStringLiteral("Multiple magnetometer-bearing MAVLink endpoints were recorded; refusing to mix their calibration samples."));
            magSender = sender; // Even below throttle or invalid/zero samples count as a source.
        }
        if (!magnetic && m.msgid != MAVLINK_MSG_ID_SENSOR_OFFSETS && m.msgid != MAVLINK_MSG_ID_VFR_HUD) continue;
        if (!senders.contains(sender)) senders.insert(sender, State{{}, options.throttleThreshold <= 0});
        auto &state = senders[sender];
        MagVector v, offsets; int compass = 1;
        switch (m.msgid) {
        case MAVLINK_MSG_ID_VFR_HUD: {
            if (!validLength(m, MAVLINK_MSG_ID_VFR_HUD_MIN_LEN, MAVLINK_MSG_ID_VFR_HUD_LEN)) { ++malformed; break; }
            mavlink_vfr_hud_t hud{}; mavlink_msg_vfr_hud_decode(&m, &hud);
            state.useData = hud.throttle >= options.throttleThreshold; break;
        }
        case MAVLINK_MSG_ID_SENSOR_OFFSETS: {
            if (!validLength(m, MAVLINK_MSG_ID_SENSOR_OFFSETS_MIN_LEN, MAVLINK_MSG_ID_SENSOR_OFFSETS_LEN)) { ++malformed; break; }
            mavlink_sensor_offsets_t of{}; mavlink_msg_sensor_offsets_decode(&m, &of);
            state.offsets = {double(of.mag_ofs_x), double(of.mag_ofs_y), double(of.mag_ofs_z)}; break;
        }
        case MAVLINK_MSG_ID_RAW_IMU: {
            if (!validLength(m, MAVLINK_MSG_ID_RAW_IMU_MIN_LEN, MAVLINK_MSG_ID_RAW_IMU_LEN)) { ++malformed; break; }
            mavlink_raw_imu_t imu{}; mavlink_msg_raw_imu_decode(&m, &imu);
            offsets = state.offsets;
            v = {imu.xmag - offsets.x, imu.ymag - offsets.y, imu.zmag - offsets.z};
            if (state.useData) add(sets, compass, v, offsets, skipped); break;
        }
        case MAVLINK_MSG_ID_SCALED_IMU2: {
            if (!validLength(m, MAVLINK_MSG_ID_SCALED_IMU2_MIN_LEN, MAVLINK_MSG_ID_SCALED_IMU2_LEN)) { ++malformed; break; }
            mavlink_scaled_imu2_t imu{}; mavlink_msg_scaled_imu2_decode(&m, &imu);
            if (state.useData) add(sets, 2, {double(imu.xmag), double(imu.ymag), double(imu.zmag)}, {}, skipped); break;
        }
        case MAVLINK_MSG_ID_SCALED_IMU3: {
            if (!validLength(m, MAVLINK_MSG_ID_SCALED_IMU3_MIN_LEN, MAVLINK_MSG_ID_SCALED_IMU3_LEN)) { ++malformed; break; }
            mavlink_scaled_imu3_t imu{}; mavlink_msg_scaled_imu3_decode(&m, &imu);
            if (state.useData) add(sets, 3, {double(imu.xmag), double(imu.ymag), double(imu.zmag)}, {}, skipped); break;
        }
        }
    }
    if (reader.status() == TlogReader::Status::Cancelled) throw Cancelled{};
    require(reader.status() != TlogReader::Status::Error, reader.errorString());
    if (reader.skippedBytes() || reader.rejectedFrames())
        report.warnings << QStringLiteral("Telemetry corruption: skipped %1 bytes and rejected %2 frames.")
            .arg(reader.skippedBytes()).arg(reader.rejectedFrames());
    if (reader.status() == TlogReader::Status::Truncated)
        report.warnings << QStringLiteral("Ignored an incomplete telemetry-log tail.");
    if (malformed) report.warnings << QStringLiteral("Ignored %1 malformed telemetry payloads.").arg(malformed);
    if (skipped) report.warnings << QStringLiteral("Skipped %1 non-finite or zero magnetometer samples.").arg(skipped);
}
struct Snapshot {
    QString canonical;
    qint64 size = 0;
    QDateTime modified, changed;
    explicit Snapshot(const QString &path) {
        const QFileInfo info(path);
        require(info.exists() && info.isFile() && !info.isSymLink(),
                QStringLiteral("Choose an existing regular log file, not a directory or symbolic link."));
        canonical = info.canonicalFilePath(); size = info.size();
        modified = info.lastModified(); changed = info.metadataChangeTime();
        require(!canonical.isEmpty(), QStringLiteral("Cannot resolve the input log path."));
    }
    bool same(const Snapshot &other) const {
        return canonical == other.canonical && size == other.size
            && modified == other.modified && changed == other.changed;
    }
};
} // namespace

QVector<MagVector> OfflineMagFitService::prepareTelemetrySamples(
    const QVector<MagVector> &input, QString *error, std::function<bool()> isCancelled)
{
    if (error) error->clear();
    const QVector<MagVector> samples = input; // Pin storage across caller callbacks.
    try {
        check(isCancelled);
        require(samples.size() <= MaximumSamplesPerCompass, QStringLiteral("Too many telemetry samples."));
        // Keys remain doubles, representing integral truncations without an
        // overflowing float-to-int cast for hostile finite input magnitudes.
        using Key = std::tuple<double, double, double>;
        std::map<Key, int> counts;
        QVector<MagVector> filtered;
        filtered.reserve(samples.size());
        for (int i = 0; i < samples.size(); ++i) {
            if ((i & 255) == 0) check(isCancelled);
            const auto &v = samples[i];
            require(finite(v), QStringLiteral("Telemetry samples contain NaN or infinity."));
            const Key key{std::trunc(v.x / 20), std::trunc(v.y / 20), std::trunc(v.z / 20)};
            int &count = counts[key];
            if (count < 3) { ++count; filtered.append(v); }
        }
        qint64 comparisons = 0;
        std::sort(filtered.begin(), filtered.end(), [&](const MagVector &a, const MagVector &b) {
            if ((++comparisons & 4095) == 0) check(isCancelled);
            return length(a) < length(b);
        });
        filtered.resize(filtered.size() - filtered.size() / 16);
        check(isCancelled);
        return filtered;
    } catch (const Cancelled &) {
        if (error) *error = QStringLiteral("Offline MagFit cancelled.");
    } catch (const std::exception &ex) {
        if (error) *error = QString::fromUtf8(ex.what());
    }
    return {};
}

bool OfflineMagFitService::fitSamples(int compass, const QVector<MagVector> &input,
    int sourceSamples, const MagVector &loggedOffsets, bool ellipsoid,
    OfflineMagFitResult *out, QString *error, std::function<bool()> isCancelled)
{
    if (error) error->clear();
    if (!out) { if (error) *error = QStringLiteral("No MagFit result destination."); return false; }
    const QVector<MagVector> samples = input;
    const MagVector offsets = loggedOffsets;
    *out = {};
    try {
        *out = fit(compass, samples, sourceSamples, offsets, ellipsoid, isCancelled);
        return true;
    } catch (const Cancelled &) {
        if (error) *error = QStringLiteral("Offline MagFit cancelled.");
    } catch (const alglib::ap_error &ex) {
        if (error) *error = QStringLiteral("MagFit solver error: %1").arg(QString::fromStdString(ex.msg));
    } catch (const std::exception &ex) {
        if (error) *error = QString::fromUtf8(ex.what());
    }
    return false;
}

OfflineMagFitReport OfflineMagFitService::analyze(const QString &input, const Options &inputOptions)
{
    bool cancellationObserved = false;
    const auto cancelCallback = inputOptions.isCancelled;
    const Options options{qBound(0, inputOptions.throttleThreshold, 100), inputOptions.useEllipsoid,
        [cancelCallback, &cancellationObserved]() {
            cancellationObserved = cancellationObserved || (cancelCallback && cancelCallback());
            return cancellationObserved;
        }, inputOptions.progress};
    const QString path = QFileInfo(input).absoluteFilePath();
    OfflineMagFitReport report;
    report.sourcePath = path; report.throttleThreshold = options.throttleThreshold;
    report.useEllipsoid = options.useEllipsoid;
    try {
        check(options.isCancelled);
        const Snapshot before(path);
        const QString suffix = QFileInfo(path).suffix().toLower();
        require(suffix == "bin" || suffix == "log" || suffix == "tlog",
                QStringLiteral("Offline MagFit accepts .bin, .log or .tlog files."));
        report.isTelemetryLog = suffix == "tlog";
        if (report.isTelemetryLog) {
            report.applyUnavailableReason = QStringLiteral("Telemetry magnetometer fields may already include scale, soft-iron and motor corrections. SENSOR_OFFSETS cannot establish raw reconstruction; telemetry analysis cannot be applied.");
            report.warnings << report.applyUnavailableReason;
        }
        QFile file(before.canonical);
        require(file.open(QIODevice::ReadOnly), file.errorString());
        if (options.progress) options.progress(0);
        check(options.isCancelled);
        require(before.same(Snapshot(path)), QStringLiteral("The input log changed before reading."));
        Sets sets;
        if (report.isTelemetryLog) readTelemetry(file, sets, report, options);
        else readDataFlash(file, suffix == "log", sets, report, options);
        check(options.isCancelled);
        require(before.same(Snapshot(path)), QStringLiteral("The input log changed during reading."));
        require(!sets.isEmpty(), report.isTelemetryLog && options.throttleThreshold > 0
            ? QStringLiteral("No usable magnetometer samples met the telemetry throttle threshold.")
            : QStringLiteral("No usable magnetometer samples were found in the log."));
        if (options.progress) options.progress(0.75);
        int completed = 0;
        for (auto it = sets.cbegin(); it != sets.cend(); ++it) {
            check(options.isCancelled);
            QVector<MagVector> samples = it->samples;
            if (report.isTelemetryLog) {
                QString error;
                samples = prepareTelemetrySamples(it->samples, &error, options.isCancelled);
                check(options.isCancelled);
                require(error.isEmpty(), error);
            }
            report.results.append(fit(it.key(), samples, it->samples.size(), it->offsets,
                                      options.useEllipsoid, options.isCancelled));
            if (report.results.last().coverageOctants < 8)
                report.warnings << QStringLiteral("Compass %1 covers only %2/8 octants; a finite fit is not a safety validation.")
                    .arg(it.key()).arg(report.results.last().coverageOctants);
            if (options.progress) options.progress(0.75 + 0.25 * ++completed / sets.size());
        }
        check(options.isCancelled);
        require(before.same(Snapshot(path)), QStringLiteral("The input log changed during analysis; results were discarded."));
        report.warnings << QStringLiteral("Only logged offsets are removed; scale, DIA/ODI and motor compensation are not undone. Logged device IDs do not prove vehicle provenance; applying requires a separate exact-target identity check. No parameters were written.");
        report.success = true;
    } catch (const Cancelled &) {
        report.cancelled = true; report.error = QStringLiteral("Offline MagFit cancelled.");
    } catch (const alglib::ap_error &ex) {
        report.error = QStringLiteral("MagFit solver error: %1").arg(QString::fromStdString(ex.msg));
    } catch (const std::exception &ex) {
        report.error = QString::fromUtf8(ex.what());
    }
    if (!report.success) { report.results.clear(); report.applyEligible = false; }
    return report;
}
