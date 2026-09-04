#include "ConfigExtendedTuningViewModel.h"

#include <QHash>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QVariantMap>

#include <algorithm>
#include <cmath>

namespace {
constexpr int kWriteTimeoutMs = 5000;

struct ExtendedTuningSchema
{
    QList<ExtendedTuningGroupDescriptor> groups;
    QList<ExtendedTuningRow> rows;
};

QVariantMap change(const QString &name, const QVariant &value)
{
    return {{QStringLiteral("name"), name},
            {QStringLiteral("value"), value}};
}

ExtendedTuningSchema makeSchema()
{
    ExtendedTuningSchema schema;
    auto addGroup = [&schema](const QString &title) {
        schema.groups.append({title, schema.rows.size(), 0});
    };
    auto addRow = [&schema](const QString &label,
                            const QStringList &candidates,
                            ParamField::EditorKind kind) {
        ExtendedTuningRow row;
        row.groupTitle = schema.groups.constLast().title;
        row.label = label;
        row.candidates = candidates;
        row.field.editorKind = kind;
        schema.rows.append(row);
        ++schema.groups.last().rowCount;
    };
    auto num = [&addRow](const QString &label,
                         const QStringList &candidates) {
        addRow(label, candidates, ParamField::EditorKind::Numeric);
    };
    auto combo = [&addRow](const QString &label,
                           const QStringList &candidates) {
        addRow(label, candidates, ParamField::EditorKind::Combo);
    };

    addGroup(QStringLiteral("Transmitter Tuning"));
    combo(QStringLiteral("Tune"), {QStringLiteral("TUNE")});
    num(QStringLiteral("Tune Min"),
        {QStringLiteral("TUNE_LOW"), QStringLiteral("TUNE_MIN")});
    num(QStringLiteral("Tune Max"),
        {QStringLiteral("TUNE_HIGH"), QStringLiteral("TUNE_MAX")});
    combo(QStringLiteral("CH6 Opt"),
          {QStringLiteral("CH6_OPT"), QStringLiteral("CH6_OPTION"),
           QStringLiteral("RC6_OPTION")});
    combo(QStringLiteral("CH7 Opt"),
          {QStringLiteral("CH7_OPT"), QStringLiteral("CH7_OPTION"),
           QStringLiteral("RC7_OPTION")});
    combo(QStringLiteral("CH8 Opt"),
          {QStringLiteral("CH8_OPT"), QStringLiteral("CH8_OPTION"),
           QStringLiteral("RC8_OPTION")});
    combo(QStringLiteral("CH9 Opt"),
          {QStringLiteral("CH9_OPT"), QStringLiteral("CH9_OPTION"),
           QStringLiteral("RC9_OPTION")});
    combo(QStringLiteral("CH10 Opt"),
          {QStringLiteral("CH10_OPT"), QStringLiteral("CH10_OPTION"),
           QStringLiteral("RC10_OPTION")});

    addGroup(QStringLiteral("Rate Roll"));
    num(QStringLiteral("P"),
        {QStringLiteral("Q_A_RAT_RLL_P"), QStringLiteral("RATE_RLL_P"),
         QStringLiteral("ATC_RAT_RLL_P")});
    num(QStringLiteral("I"),
        {QStringLiteral("Q_A_RAT_RLL_I"), QStringLiteral("RATE_RLL_I"),
         QStringLiteral("ATC_RAT_RLL_I")});
    num(QStringLiteral("IMAX"),
        {QStringLiteral("Q_A_RAT_RLL_IMAX"),
         QStringLiteral("ATC_RAT_RLL_IMAX"),
         QStringLiteral("RATE_RLL_IMAX")});
    num(QStringLiteral("D"),
        {QStringLiteral("Q_A_RAT_RLL_D"), QStringLiteral("RATE_RLL_D"),
         QStringLiteral("ATC_RAT_RLL_D")});
    num(QStringLiteral("FLTE"),
        {QStringLiteral("Q_A_RAT_RLL_FLTE"),
         QStringLiteral("RATE_RLL_FILT"),
         QStringLiteral("ATC_RAT_RLL_FILT"),
         QStringLiteral("ATC_RAT_RLL_FLTE")});
    num(QStringLiteral("FLTD"),
        {QStringLiteral("Q_A_RAT_RLL_FLTD"),
         QStringLiteral("ATC_RAT_RLL_FLTD")});
    num(QStringLiteral("FLTT"),
        {QStringLiteral("Q_A_RAT_RLL_FLTT"),
         QStringLiteral("ATC_RAT_RLL_FLTT")});

    addGroup(QStringLiteral("Rate Pitch"));
    num(QStringLiteral("P"),
        {QStringLiteral("Q_A_RAT_PIT_P"), QStringLiteral("RATE_PIT_P"),
         QStringLiteral("ATC_RAT_PIT_P")});
    num(QStringLiteral("I"),
        {QStringLiteral("Q_A_RAT_PIT_I"), QStringLiteral("RATE_PIT_I"),
         QStringLiteral("ATC_RAT_PIT_I")});
    num(QStringLiteral("IMAX"),
        {QStringLiteral("Q_A_RAT_PIT_IMAX"),
         QStringLiteral("ATC_RAT_PIT_IMAX"),
         QStringLiteral("RATE_PIT_IMAX")});
    num(QStringLiteral("D"),
        {QStringLiteral("Q_A_RAT_PIT_D"), QStringLiteral("RATE_PIT_D"),
         QStringLiteral("ATC_RAT_PIT_D")});
    num(QStringLiteral("FLTE"),
        {QStringLiteral("Q_A_RAT_PIT_FLTE"),
         QStringLiteral("RATE_PIT_FILT"),
         QStringLiteral("ATC_RAT_PIT_FILT"),
         QStringLiteral("ATC_RAT_PIT_FLTE")});
    num(QStringLiteral("FLTD"),
        {QStringLiteral("Q_A_RAT_PIT_FLTD"),
         QStringLiteral("ATC_RAT_PIT_FLTD")});
    num(QStringLiteral("FLTT"),
        {QStringLiteral("Q_A_RAT_PIT_FLTT"),
         QStringLiteral("ATC_RAT_PIT_FLTT")});

    addGroup(QStringLiteral("Rate Yaw"));
    num(QStringLiteral("P"),
        {QStringLiteral("Q_A_RAT_YAW_P"), QStringLiteral("RATE_YAW_P"),
         QStringLiteral("ATC_RAT_YAW_P")});
    num(QStringLiteral("I"),
        {QStringLiteral("Q_A_RAT_YAW_I"), QStringLiteral("RATE_YAW_I"),
         QStringLiteral("ATC_RAT_YAW_I")});
    num(QStringLiteral("IMAX"),
        {QStringLiteral("Q_A_RAT_YAW_IMAX"),
         QStringLiteral("ATC_RAT_YAW_IMAX"),
         QStringLiteral("RATE_YAW_IMAX")});
    num(QStringLiteral("D"),
        {QStringLiteral("Q_A_RAT_YAW_D"), QStringLiteral("RATE_YAW_D"),
         QStringLiteral("ATC_RAT_YAW_D")});
    num(QStringLiteral("FLTE"),
        {QStringLiteral("Q_A_RAT_YAW_FLTE"),
         QStringLiteral("RATE_YAW_FILT"),
         QStringLiteral("ATC_RAT_YAW_FILT"),
         QStringLiteral("ATC_RAT_YAW_FLTE")});
    num(QStringLiteral("FLTD"),
        {QStringLiteral("Q_A_RAT_YAW_FLTD"),
         QStringLiteral("ATC_RAT_YAW_FLTD")});
    num(QStringLiteral("FLTT"),
        {QStringLiteral("Q_A_RAT_YAW_FLTT"),
         QStringLiteral("ATC_RAT_YAW_FLTT")});

    addGroup(QStringLiteral("Stabilize Roll (Error to Rate)"));
    num(QStringLiteral("P"),
        {QStringLiteral("Q_A_ANG_RLL_P"), QStringLiteral("STB_RLL_P"),
         QStringLiteral("ATC_ANG_RLL_P")});
    num(QStringLiteral("ACCEL MAX"),
        {QStringLiteral("Q_A_ACCEL_R_MAX"),
         QStringLiteral("Q_A_ACC_R_MAX"),
         QStringLiteral("ATC_ACCEL_R_MAX"),
         QStringLiteral("ATC_ACC_R_MAX")});

    addGroup(QStringLiteral("Stabilize Pitch (Error to Rate)"));
    num(QStringLiteral("P"),
        {QStringLiteral("Q_A_ANG_PIT_P"), QStringLiteral("STB_PIT_P"),
         QStringLiteral("ATC_ANG_PIT_P")});
    num(QStringLiteral("ACCEL MAX"),
        {QStringLiteral("Q_A_ACCEL_P_MAX"),
         QStringLiteral("Q_A_ACC_P_MAX"),
         QStringLiteral("ATC_ACCEL_P_MAX"),
         QStringLiteral("ATC_ACC_P_MAX")});

    addGroup(QStringLiteral("Stabilize Yaw (Error to Rate)"));
    num(QStringLiteral("P"),
        {QStringLiteral("Q_A_ANG_YAW_P"), QStringLiteral("STB_YAW_P"),
         QStringLiteral("ATC_ANG_YAW_P")});
    num(QStringLiteral("ACCEL MAX"),
        {QStringLiteral("Q_A_ACCEL_Y_MAX"),
         QStringLiteral("Q_A_ACC_Y_MAX"),
         QStringLiteral("ATC_ACCEL_Y_MAX"),
         QStringLiteral("ATC_ACC_Y_MAX")});

    addGroup(QStringLiteral("Throttle Accel (Accel to motor)"));
    num(QStringLiteral("P"),
        {QStringLiteral("Q_P_ACCZ_P"), QStringLiteral("Q_P_D_ACC_P"),
         QStringLiteral("THR_ACCEL_P"), QStringLiteral("ACCEL_Z_P"),
         QStringLiteral("PSC_ACCZ_P"), QStringLiteral("PSC_D_ACC_P")});
    num(QStringLiteral("I"),
        {QStringLiteral("Q_P_ACCZ_I"), QStringLiteral("Q_P_D_ACC_I"),
         QStringLiteral("THR_ACCEL_I"), QStringLiteral("ACCEL_Z_I"),
         QStringLiteral("PSC_ACCZ_I"), QStringLiteral("PSC_D_ACC_I")});
    num(QStringLiteral("IMAX"),
        {QStringLiteral("Q_P_ACCZ_IMAX"),
         QStringLiteral("Q_P_D_ACC_IMAX"),
         QStringLiteral("THR_ACCEL_IMAX"),
         QStringLiteral("ACCEL_Z_IMAX"),
         QStringLiteral("PSC_ACCZ_IMAX"),
         QStringLiteral("PSC_D_ACC_IMAX")});
    num(QStringLiteral("D"),
        {QStringLiteral("Q_P_ACCZ_D"), QStringLiteral("Q_P_D_ACC_D"),
         QStringLiteral("THR_ACCEL_D"), QStringLiteral("ACCEL_Z_D"),
         QStringLiteral("PSC_ACCZ_D"), QStringLiteral("PSC_D_ACC_D")});

    addGroup(QStringLiteral("Throttle Rate (VSpd to accel)"));
    num(QStringLiteral("P"),
        {QStringLiteral("Q_P_VELZ_P"), QStringLiteral("Q_P_D_VEL_P"),
         QStringLiteral("THR_RATE_P"), QStringLiteral("VEL_Z_P"),
         QStringLiteral("PSC_VELZ_P"), QStringLiteral("PSC_D_VEL_P")});

    addGroup(QStringLiteral("Altitude Hold (Alt to climbrate)"));
    num(QStringLiteral("P"),
        {QStringLiteral("Q_P_POSZ_P"), QStringLiteral("Q_P_D_POS_P"),
         QStringLiteral("THR_ALT_P"), QStringLiteral("POS_Z_P"),
         QStringLiteral("PSC_POSZ_P"), QStringLiteral("PSC_D_POS_P")});

    addGroup(QStringLiteral("Velocity XY (Vel to Accel)"));
    num(QStringLiteral("P"),
        {QStringLiteral("Q_P_VELXY_P"), QStringLiteral("Q_P_NE_VEL_P"),
         QStringLiteral("LOITER_LAT_P"), QStringLiteral("VEL_XY_P"),
         QStringLiteral("PSC_VELXY_P"), QStringLiteral("PSC_NE_VEL_P")});
    num(QStringLiteral("I"),
        {QStringLiteral("Q_P_VELXY_I"), QStringLiteral("Q_P_NE_VEL_I"),
         QStringLiteral("LOITER_LAT_I"), QStringLiteral("VEL_XY_I"),
         QStringLiteral("PSC_VELXY_I"), QStringLiteral("PSC_NE_VEL_I")});
    num(QStringLiteral("IMAX"),
        {QStringLiteral("Q_P_VELXY_IMAX"),
         QStringLiteral("Q_P_NE_VEL_IMAX"),
         QStringLiteral("LOITER_LAT_IMAX"),
         QStringLiteral("VEL_XY_IMAX"),
         QStringLiteral("PSC_VELXY_IMAX"),
         QStringLiteral("PSC_NE_VEL_IMAX")});
    num(QStringLiteral("D"),
        {QStringLiteral("Q_P_VELXY_D"), QStringLiteral("Q_P_NE_VEL_D"),
         QStringLiteral("LOITER_LAT_D"), QStringLiteral("PSC_VELXY_D"),
         QStringLiteral("PSC_NE_VEL_D")});

    addGroup(QStringLiteral("Position XY (Dist to Speed)"));
    num(QStringLiteral("P"),
        {QStringLiteral("Q_P_POSXY_P"), QStringLiteral("HLD_LAT_P"),
         QStringLiteral("POS_XY_P"), QStringLiteral("PSC_POSXY_P")});
    num(QStringLiteral("INPUT TC"),
        {QStringLiteral("Q_A_INPUT_TC"), QStringLiteral("ATC_INPUT_TC")});

    addGroup(QStringLiteral("WPNav (cm's)"));
    num(QStringLiteral("Speed"),
        {QStringLiteral("Q_WP_SPEED"), QStringLiteral("Q_WP_SPD"),
         QStringLiteral("WPNAV_SPEED"), QStringLiteral("WP_SPD")});
    num(QStringLiteral("Radius"),
        {QStringLiteral("Q_WP_RADIUS"),
         QStringLiteral("Q_WP_RADIUS_M"),
         QStringLiteral("WPNAV_RADIUS"),
         QStringLiteral("WP_RADIUS_M")});
    num(QStringLiteral("Speed Dn"),
        {QStringLiteral("Q_WP_SPEED_DN"),
         QStringLiteral("Q_WP_SPD_DN"),
         QStringLiteral("WPNAV_SPEED_DN"),
         QStringLiteral("WP_SPD_DN")});
    num(QStringLiteral("Speed Up"),
        {QStringLiteral("Q_WP_SPEED_UP"),
         QStringLiteral("Q_WP_SPD_UP"),
         QStringLiteral("WPNAV_SPEED_UP"),
         QStringLiteral("WP_SPD_UP")});
    num(QStringLiteral("Loiter Speed"),
        {QStringLiteral("Q_LOIT_SPEED"),
         QStringLiteral("Q_LOIT_SPEED_MS"),
         QStringLiteral("WPNAV_LOIT_SPEED"),
         QStringLiteral("LOIT_SPEED"),
         QStringLiteral("LOIT_SPEED_MS")});

    addGroup(QStringLiteral("Basic Filters"));
    num(QStringLiteral("Gyro"), {QStringLiteral("INS_GYRO_FILTER")});
    num(QStringLiteral("Accel"), {QStringLiteral("INS_ACCEL_FILTER")});

    addGroup(QStringLiteral("Static Notch Filter"));
    combo(QStringLiteral("Enabled"),
          {QStringLiteral("INS_NOTCH_ENABLE")});
    num(QStringLiteral("Frequency"),
        {QStringLiteral("INS_NOTCH_FREQ")});
    num(QStringLiteral("BandWidth"),
        {QStringLiteral("INS_NOTCH_BW")});
    num(QStringLiteral("Attenuation"),
        {QStringLiteral("INS_NOTCH_ATT")});

    addGroup(QStringLiteral("Harmonic Notch Filter"));
    combo(QStringLiteral("Enabled"),
          {QStringLiteral("INS_HNTCH_ENABLE")});
    combo(QStringLiteral("Mode"), {QStringLiteral("INS_HNTCH_MODE")});
    num(QStringLiteral("Reference"), {QStringLiteral("INS_HNTCH_REF")});
    num(QStringLiteral("Frequency"), {QStringLiteral("INS_HNTCH_FREQ")});
    num(QStringLiteral("Attenuation"), {QStringLiteral("INS_HNTCH_ATT")});
    num(QStringLiteral("Bandwidth"), {QStringLiteral("INS_HNTCH_BW")});
    combo(QStringLiteral("Options"), {QStringLiteral("INS_HNTCH_OPTS")});
    num(QStringLiteral("Harmonics"), {QStringLiteral("INS_HNTCH_HMNCS")});

    addGroup(QStringLiteral("Filter Logs"));
    combo(QStringLiteral("Mask"), {QStringLiteral("INS_LOG_BAT_MASK")});
    num(QStringLiteral("Options"), {QStringLiteral("INS_LOG_BAT_OPT")});

    // MP10 mirrors every existing _RLL_ descriptor into its _PIT_ peer.
    // Explicit indices avoid relying on a particular firmware alias spelling.
    for (int offset = 0; offset < 7; ++offset) {
        schema.rows[8 + offset].mirrorTargetRow = 15 + offset;
    }
    schema.rows[29].mirrorTargetRow = 31; // Stabilize Roll P -> Pitch P
    return schema;
}

const ExtendedTuningSchema &referenceSchema()
{
    static const ExtendedTuningSchema schema = makeSchema();
    return schema;
}

bool isQPlaneName(const QString &name)
{
    return name.startsWith(QStringLiteral("Q_A_"))
        || name.startsWith(QStringLiteral("Q_P_"))
        || name.startsWith(QStringLiteral("Q_WP_"))
        || name.startsWith(QStringLiteral("Q_LOIT_"));
}
} // namespace

bool ExtendedTuningRow::dirty() const
{
    if (!exists) {
        return false;
    }
    bool acceptedOk = false;
    bool stagedOk = false;
    const double accepted = acceptedValue.toDouble(&acceptedOk);
    const double staged = field.value.toDouble(&stagedOk);
    return acceptedOk && stagedOk
        ? std::abs(accepted - staged) > 1.0e-6
        : acceptedValue != field.value;
}

ConfigExtendedTuningViewModel::ConfigExtendedTuningViewModel(
    QObject *parent)
    : QObject(parent),
      m_groups(ReferenceGroups())
{
    rebuildRows();
    updateStatus();
}

int ConfigExtendedTuningViewModel::WriteTimeoutMs()
{
    return kWriteTimeoutMs;
}

QList<ExtendedTuningGroupDescriptor>
ConfigExtendedTuningViewModel::ReferenceGroups()
{
    return referenceSchema().groups;
}

QList<ExtendedTuningRow> ConfigExtendedTuningViewModel::ReferenceRows()
{
    return referenceSchema().rows;
}

QStringList ConfigExtendedTuningViewModel::ReferenceParameterNames()
{
    QStringList result{QStringLiteral("Q_ENABLE"),
                       QStringLiteral("H_SWASH_TYPE")};
    QSet<QString> seen(result.constBegin(), result.constEnd());
    for (const ExtendedTuningRow &row : referenceSchema().rows) {
        for (const QString &candidate : row.candidates) {
            if (!seen.contains(candidate)) {
                result.append(candidate);
                seen.insert(candidate);
            }
        }
    }
    return result;
}

QString ConfigExtendedTuningViewModel::Title() const
{
    return tr("Extended Tuning");
}

QString ConfigExtendedTuningViewModel::Intro() const
{
    return tr("Copter / QuadPlane PID gains and tuning. Fields show n/a "
              "if not present on this firmware.");
}

QStringList ConfigExtendedTuningViewModel::LargeIncreaseNames() const
{
    QStringList result;
    for (int index = 0; index < m_rows.size(); ++index) {
        const ExtendedTuningRow &row = m_rows.at(index);
        if (!row.dirty() || !CanEditRow(index)) {
            continue;
        }
        bool acceptedOk = false;
        bool stagedOk = false;
        const double accepted = row.acceptedValue.toDouble(&acceptedOk);
        const double staged = row.field.value.toDouble(&stagedOk);
        if (acceptedOk && stagedOk && std::isfinite(accepted)
            && std::isfinite(staged) && staged > accepted * 2.0) {
            result.append(row.resolvedName);
        }
    }
    return result;
}

bool ConfigExtendedTuningViewModel::Dirty() const
{
    return std::any_of(m_rows.constBegin(), m_rows.constEnd(),
                       [](const ExtendedTuningRow &row) {
        return row.dirty();
    });
}

bool ConfigExtendedTuningViewModel::CanEdit() const
{
    return m_connected && m_heartbeatFresh && !m_armed
        && m_snapshotReady
        && !m_pending.active && !m_reconciliationRequired;
}

bool ConfigExtendedTuningViewModel::CanEditRow(int zeroBasedRow) const
{
    if (!CanEdit() || zeroBasedRow < 0
        || zeroBasedRow >= m_rows.size()) {
        return false;
    }
    const ExtendedTuningRow &row = m_rows.at(zeroBasedRow);
    if (!row.exists || row.field.readOnly) {
        return false;
    }
    return m_quadPlaneState == QuadPlaneState::Enabled
        || !isQuadPlaneParameter(row.resolvedName);
}

bool ConfigExtendedTuningViewModel::CanSave() const
{
    for (int index = 0; index < m_rows.size(); ++index) {
        if (m_rows.at(index).dirty() && CanEditRow(index)) {
            return true;
        }
    }
    return false;
}

bool ConfigExtendedTuningViewModel::RequiresLargeIncreaseConfirmation() const
{
    return !LargeIncreaseNames().isEmpty();
}

void ConfigExtendedTuningViewModel::setCatalog(
    const ParameterMetaDataCatalog &catalog, bool enforceMetadataRanges)
{
    const QList<ExtendedTuningRow> previous = m_rows;
    m_catalog = catalog;
    m_enforceMetadataRanges = enforceMetadataRanges;
    rebuildRows();
    for (int index = 0;
         index < previous.size() && index < m_rows.size(); ++index) {
        if (previous.at(index).dirty() && m_rows[index].exists
            && previous.at(index).resolvedName
                == m_rows.at(index).resolvedName) {
            m_rows[index].field.value = previous.at(index).field.value;
        }
    }
    updateStatus();
    emit structureChanged();
    emit rowsChanged();
    emit stateChanged();
}

void ConfigExtendedTuningViewModel::setParameterSnapshot(
    const QList<ConfigFriendlyParameterValue> &parameters,
    int preferredComponent, bool completeSnapshot,
    bool preserveStagedEdits)
{
    if (m_pending.active && m_pending.batchId != 0
        && m_pending.changes.size() > 1) {
        finishUncertain(
            tr("The parameter snapshot changed during an Extended Tuning "
               "write; refreshing the full vehicle state is required."),
            true);
        return;
    }
    if (m_pending.active) {
        // A snapshot is not an owned terminal result for a pending batch.
        return;
    }

    const QList<ExtendedTuningRow> previous = m_rows;
    const bool preserve = preserveStagedEdits
        && !m_reconciliationRequired && Dirty();

    const QStringList referenceNames = ReferenceParameterNames();
    const QSet<QString> relevantNames(referenceNames.constBegin(),
                                      referenceNames.constEnd());
    // The page belongs to the component captured by its target lease. Never
    // borrow a plausible-looking parameter surface from another component in
    // the same vehicle snapshot.
    m_componentId = preferredComponent;

    ++m_snapshotGeneration;
    m_pending = {};
    m_snapshotParameters = parameters;
    m_snapshotComplete = completeSnapshot;
    m_snapshotReady = completeSnapshot;
    m_operationStatus.clear();

    QHash<QString, QVariant> exactValues;
    if (completeSnapshot) {
        for (const ConfigFriendlyParameterValue &parameter : parameters) {
            if (parameter.componentId == m_componentId) {
                exactValues.insert(normalizedName(parameter.name),
                                   parameter.value);
            }
        }
    }
    if (!completeSnapshot) {
        m_quadPlaneState = QuadPlaneState::Unknown;
    } else if (exactValues.contains(QStringLiteral("Q_ENABLE"))) {
        m_quadPlaneState =
            exactValues.value(QStringLiteral("Q_ENABLE")).toDouble() == 0.0
            ? QuadPlaneState::Disabled : QuadPlaneState::Enabled;
    } else {
        bool hasQSchema = false;
        for (auto iterator = exactValues.constBegin();
             iterator != exactValues.constEnd() && !hasQSchema; ++iterator) {
            hasQSchema = isQPlaneName(iterator.key())
                && relevantNames.contains(iterator.key());
        }
        m_quadPlaneState = hasQSchema
            ? QuadPlaneState::Enabled : QuadPlaneState::Unavailable;
    }
    if (completeSnapshot) {
        m_reconciliationRequired = false;
    }

    rebuildRows();
    updateInitialRollPitchLock();
    if (preserve && previous.size() == m_rows.size()) {
        for (int index = 0; index < m_rows.size(); ++index) {
            if (previous.at(index).dirty() && m_rows[index].exists
                && previous.at(index).resolvedName
                    == m_rows.at(index).resolvedName) {
                m_rows[index].field.value = previous.at(index).field.value;
            }
        }
        if (Dirty()) {
            m_operationStatus = tr(
                "Parameters refreshed; staged tuning changes were "
                "preserved.");
        }
    }

    updateStatus();
    emit structureChanged();
    emit rowsChanged();
    emit stateChanged();
}

void ConfigExtendedTuningViewModel::setConnected(bool connected)
{
    if (m_connected == connected) {
        return;
    }
    m_connected = connected;
    if (!connected) {
        m_heartbeatFresh = false;
        if (m_pending.active && m_pending.batchId != 0
            && m_pending.changes.size() > 1) {
            finishUncertain(
                tr("Extended Tuning state is uncertain after connection "
                   "loss; a full parameter refresh is required."),
                false);
            return;
        }
        if (m_pending.active) {
            m_pending = {};
            m_operationStatus = tr(
                "The write was not confirmed; staged changes were kept.");
        }
    }
    updateStatus();
    const QPointer<ConfigExtendedTuningViewModel> guard(this);
    emit stateChanged();
    if (guard && connected && m_connected && m_reconciliationRequired) {
        emit refreshRequested(m_componentId);
    }
}

void ConfigExtendedTuningViewModel::setArmed(bool armed)
{
    if (m_armed == armed) {
        return;
    }
    m_armed = armed;
    updateStatus();
    emit stateChanged();
}

void ConfigExtendedTuningViewModel::setHeartbeatFresh(bool fresh)
{
    const bool actualFresh = fresh && m_connected;
    if (m_heartbeatFresh == actualFresh) {
        return;
    }
    m_heartbeatFresh = actualFresh;
    updateStatus();
    emit stateChanged();
}

void ConfigExtendedTuningViewModel::setHeartbeat(bool fresh, bool armed)
{
    const bool actualFresh = fresh && m_connected;
    if (m_heartbeatFresh == actualFresh && m_armed == armed) {
        return;
    }
    m_heartbeatFresh = actualFresh;
    m_armed = armed;
    updateStatus();
    emit stateChanged();
}

void ConfigExtendedTuningViewModel::setLockRollPitch(bool enabled)
{
    if (m_lockRollPitch == enabled) {
        return;
    }
    m_lockRollPitch = enabled;
    emit stateChanged();
}

bool ConfigExtendedTuningViewModel::stageValue(
    int zeroBasedRow, const QVariant &value)
{
    return stageRowValue(zeroBasedRow, value, true);
}

bool ConfigExtendedTuningViewModel::stageValue(
    const QString &resolvedName, const QVariant &value)
{
    const QString name = normalizedName(resolvedName);
    for (int index = 0; index < m_rows.size(); ++index) {
        if (m_rows.at(index).resolvedName == name) {
            return stageRowValue(index, value, true);
        }
    }
    return false;
}

QVariantList ConfigExtendedTuningViewModel::DirtyChanges() const
{
    QVariantList changes;
    if (!m_snapshotReady) {
        return changes;
    }
    for (int index = 0; index < m_rows.size(); ++index) {
        const ExtendedTuningRow &row = m_rows.at(index);
        if (row.dirty() && CanEditRow(index)) {
            changes.append(change(row.resolvedName, row.field.value));
        }
    }
    return changes;
}

bool ConfigExtendedTuningViewModel::Save(bool confirmedLargeIncrease)
{
    if (!CanSave()) {
        updateStatus();
        emit stateChanged();
        return false;
    }
    const QStringList large = LargeIncreaseNames();
    if (!confirmedLargeIncrease && !large.isEmpty()) {
        m_operationStatus = tr(
            "Confirmation is required because one or more values more "
            "than double their accepted value.");
        updateStatus();
        const QPointer<ConfigExtendedTuningViewModel> guard(this);
        emit stateChanged();
        if (guard) {
            emit largeIncreaseConfirmationRequested(large);
        }
        return false;
    }

    const QVariantList changes = DirtyChanges();
    if (changes.isEmpty()) {
        return false;
    }
    m_pending = {};
    m_pending.active = true;
    m_pending.changes = changes;
    m_pending.requestId = ++m_requestGeneration;
    m_pending.snapshotGeneration = m_snapshotGeneration;
    m_operationStatus.clear();
    updateStatus();

    const QPointer<ConfigExtendedTuningViewModel> guard(this);
    emit stateChanged();
    if (!guard || !m_pending.active
        || m_pending.requestId != m_requestGeneration
        || m_pending.snapshotGeneration != m_snapshotGeneration) {
        return false;
    }

    const quint64 requestId = m_pending.requestId;
    const quint64 snapshotGeneration = m_pending.snapshotGeneration;
    QTimer::singleShot(kWriteTimeoutMs, this,
                       [this, requestId, snapshotGeneration]() {
        if (!m_pending.active || m_pending.requestId != requestId
            || m_pending.snapshotGeneration != snapshotGeneration
            || m_pending.batchId != 0
            || m_snapshotGeneration != snapshotGeneration) {
            return;
        }
        finishFailure(tr("write submission timeout"));
    });

    emit writeRequested(requestId, m_componentId, changes);
    return !guard.isNull();
}

bool ConfigExtendedTuningViewModel::Refresh()
{
    if (!m_connected || m_pending.active) {
        return false;
    }
    m_operationStatus = tr("Refreshing the full parameter list…");
    updateStatus();
    const quint64 snapshotGeneration = m_snapshotGeneration;
    const QPointer<ConfigExtendedTuningViewModel> guard(this);
    emit stateChanged();
    if (!guard || !m_connected || m_pending.active
        || snapshotGeneration != m_snapshotGeneration) {
        return false;
    }
    emit refreshRequested(m_componentId);
    return true;
}

bool ConfigExtendedTuningViewModel::Discard()
{
    if (m_pending.active || !Dirty()) {
        return false;
    }
    for (ExtendedTuningRow &row : m_rows) {
        if (row.exists) {
            row.field.value = row.acceptedValue;
            row.field.status.clear();
        }
    }
    m_operationStatus.clear();
    updateStatus();
    emit rowsChanged();
    emit stateChanged();
    return true;
}

void ConfigExtendedTuningViewModel::parameterChanged(
    int componentId, const QString &name, const QVariant &value)
{
    if (componentId != m_componentId || !m_snapshotReady
        || m_reconciliationRequired) {
        return;
    }
    const QString normalized = normalizedName(name);
    if (normalized == QLatin1String("Q_ENABLE")) {
        for (ConfigFriendlyParameterValue &parameter :
             m_snapshotParameters) {
            if (parameter.componentId == m_componentId
                && normalizedName(parameter.name) == normalized) {
                if (valuesEqual(parameter.value, value)) {
                    return;
                }
                parameter.value = value;
                m_quadPlaneState = value.toDouble() == 0.0
                    ? QuadPlaneState::Disabled : QuadPlaneState::Enabled;
                m_operationStatus.clear();
                updateStatus();
                emit stateChanged();
                return;
            }
        }
        return;
    }
    if (m_pending.active) {
        return;
    }
    for (ExtendedTuningRow &row : m_rows) {
        if (!row.exists || row.resolvedName != normalized) {
            continue;
        }
        const bool wasDirty = row.dirty();
        row.acceptedValue = typedValue(value, row.acceptedValue);
        if (!wasDirty) {
            row.field.value = row.acceptedValue;
        }
        if (row.field.editorKind == ParamField::EditorKind::Combo) {
            const bool known = std::any_of(
                row.field.options.constBegin(), row.field.options.constEnd(),
                [&row](const ParamOption &option) {
                    return valuesEqual(option.value, row.acceptedValue);
                });
            if (!known) {
                row.field.options.append(
                    {row.acceptedValue,
                     tr("Unknown (%1)").arg(row.acceptedValue.toString())});
            }
        }
        for (ConfigFriendlyParameterValue &parameter :
             m_snapshotParameters) {
            if (parameter.componentId == m_componentId
                && normalizedName(parameter.name) == normalized) {
                parameter.value = row.acceptedValue;
                break;
            }
        }
        m_operationStatus.clear();
        updateStatus();
        emit rowsChanged();
        emit stateChanged();
        return;
    }
}

void ConfigExtendedTuningViewModel::parameterWriteSubmitted(
    quint64 requestId, qulonglong batchId)
{
    if (!m_pending.active || m_pending.requestId != requestId
        || m_pending.snapshotGeneration != m_snapshotGeneration
        || m_pending.batchId != 0) {
        return;
    }
    if (batchId == 0) {
        finishFailure(tr("write was rejected"));
        return;
    }
    m_pending.batchId = batchId;
}

void ConfigExtendedTuningViewModel::parameterWriteSubmissionFailed(
    quint64 requestId, const QString &reason)
{
    if (!m_pending.active || m_pending.requestId != requestId
        || m_pending.snapshotGeneration != m_snapshotGeneration
        || m_pending.batchId != 0) {
        return;
    }
    finishFailure(reason.trimmed().isEmpty()
                      ? tr("write submission failed") : reason.trimmed());
}

void ConfigExtendedTuningViewModel::parameterWriteFailed(
    qulonglong batchId, int componentId, const QString &name,
    const QString &reason)
{
    if (!m_pending.active || m_pending.batchId == 0
        || batchId != m_pending.batchId || componentId != m_componentId
        || !pendingContains(name)) {
        return;
    }
    const QString failure = reason.trimmed().isEmpty()
        ? tr("write failed") : reason.trimmed();
    if (m_pending.changes.size() > 1) {
        m_pending.failureReason = failure;
        updateStatus();
        emit stateChanged();
        return;
    }
    finishFailure(failure);
}

void ConfigExtendedTuningViewModel::parameterWriteCancelled(
    qulonglong batchId, int componentId, const QString &name)
{
    if (!m_pending.active || m_pending.batchId == 0
        || batchId != m_pending.batchId || componentId != m_componentId
        || !pendingContains(name)) {
        return;
    }
    if (m_pending.changes.size() > 1) {
        m_pending.failureReason = tr("write cancelled");
        updateStatus();
        emit stateChanged();
        return;
    }
    finishFailure(tr("write cancelled"));
}

void ConfigExtendedTuningViewModel::parameterBatchCompleted(
    qulonglong batchId, int succeeded, int failed)
{
    if (!m_pending.active || m_pending.batchId == 0
        || batchId != m_pending.batchId) {
        return;
    }
    const int total = m_pending.changes.size();
    const bool success = failed == 0 && succeeded == total
        && m_pending.failureReason.isEmpty();
    if (success) {
        finishSuccess();
    } else if (total > 1) {
        finishUncertain(
            tr("Extended Tuning parameters may have been partially applied; "
               "refreshing the full vehicle state is required."),
            true);
    } else {
        finishFailure(m_pending.failureReason.isEmpty()
                          ? tr("write failed")
                          : m_pending.failureReason);
    }
}

void ConfigExtendedTuningViewModel::refreshFailed(const QString &reason)
{
    const QString failure = reason.trimmed().isEmpty()
        ? tr("parameter refresh failed") : reason.trimmed();
    m_operationStatus = m_reconciliationRequired
        ? tr("Extended Tuning state remains uncertain; refresh failed: %1")
              .arg(failure)
        : failure;
    updateStatus();
    emit stateChanged();
}

void ConfigExtendedTuningViewModel::refreshCanceled()
{
    m_operationStatus = m_reconciliationRequired
        ? tr("Extended Tuning state remains uncertain; refresh was "
             "canceled.")
        : tr("parameter refresh canceled");
    updateStatus();
    emit stateChanged();
}

void ConfigExtendedTuningViewModel::rebuildRows()
{
    const QList<ExtendedTuningRow> descriptors = ReferenceRows();
    QHash<QString, QVariant> values;
    if (m_snapshotComplete) {
        for (const ConfigFriendlyParameterValue &parameter :
             m_snapshotParameters) {
            if (parameter.componentId == m_componentId) {
                values.insert(normalizedName(parameter.name),
                              parameter.value);
            }
        }
    }

    m_rows.clear();
    m_rows.reserve(descriptors.size());
    for (ExtendedTuningRow row : descriptors) {
        for (const QString &candidate : row.candidates) {
            if (values.contains(candidate)) {
                row.resolvedName = candidate;
                row.exists = true;
                break;
            }
        }

        row.field.componentId = m_componentId;
        row.field.name = row.exists ? row.resolvedName
                                    : row.candidates.value(0);
        row.field.label = row.label;
        row.field.value = row.exists
            ? values.value(row.resolvedName) : QVariant();
        row.acceptedValue = row.field.value;
        row.field.status = row.exists ? QString() : tr("n/a");

        const ParameterMetaData metadata = row.exists
            ? m_catalog.value(row.resolvedName) : ParameterMetaData();
        row.field.units = metadata.units;
        row.field.description = metadata.description;
        row.field.readOnly = !row.exists || metadata.readOnly;
        for (const ParameterMetaDataOption &option : metadata.values) {
            row.field.options.append({option.value, option.label});
        }
        if (!metadata.bitmaskValues.isEmpty()) {
            row.field.editorKind = ParamField::EditorKind::Bitmask;
            row.field.options.clear();
            for (const QPair<int, QString> &bit :
                 metadata.bitmaskValues) {
                row.field.bitOptions.append({bit.first, bit.second});
            }
        }
        if (row.exists
            && row.field.editorKind == ParamField::EditorKind::Combo) {
            const bool known = std::any_of(
                row.field.options.constBegin(), row.field.options.constEnd(),
                [&row](const ParamOption &option) {
                    return ConfigExtendedTuningViewModel::valuesEqual(
                        option.value, row.acceptedValue);
                });
            if (!known) {
                row.field.options.append(
                    {row.acceptedValue,
                     tr("Unknown (%1)").arg(row.acceptedValue.toString())});
            }
        }
        if (metadata.hasRange) {
            row.field.minimum = metadata.minimum;
            row.field.maximum = metadata.maximum;
            row.field.hasRange = true;
            row.field.enforceRange = m_enforceMetadataRanges;
        }
        if (metadata.hasIncrement && metadata.increment > 0.0) {
            row.field.increment = metadata.increment;
        } else if (row.exists) {
            bool numeric = false;
            const double current = row.acceptedValue.toDouble(&numeric);
            if (numeric && std::isfinite(current)
                && std::abs(current - std::round(current)) < 1.0e-9) {
                row.field.increment = 1.0;
            }
        }
        m_rows.append(row);
    }
}

void ConfigExtendedTuningViewModel::updateStatus()
{
    if (!m_connected) {
        m_status = tr("offline");
    } else if (m_reconciliationRequired) {
        m_status = m_operationStatus.isEmpty()
            ? tr("Extended Tuning state is uncertain; a full parameter "
                 "refresh is required.")
            : m_operationStatus;
    } else if (m_pending.active) {
        m_status = m_pending.failureReason.isEmpty()
            ? tr("Writing Extended Tuning parameters…")
            : tr("A tuning parameter failed; waiting for the complete "
                 "batch result…");
    } else if (!m_heartbeatFresh) {
        m_status = tr("Waiting for a fresh vehicle heartbeat.");
    } else if (m_armed) {
        m_status = tr("Vehicle is armed; tuning changes are disabled.");
    } else if (!m_snapshotComplete
               || m_quadPlaneState == QuadPlaneState::Unknown) {
        m_status = tr("A complete exact-component parameter snapshot is "
                      "required.");
    } else if (m_quadPlaneState == QuadPlaneState::Disabled) {
        m_status = tr("QuadPlane is disabled by Q_ENABLE; available RC and "
                      "INS fields remain editable.");
    } else if (m_quadPlaneState == QuadPlaneState::Unavailable) {
        m_status = tr("QuadPlane tuning parameters are unavailable on this "
                      "firmware; available RC and INS fields remain "
                      "editable.");
    } else if (!m_operationStatus.isEmpty()) {
        m_status = m_operationStatus;
    } else if (Dirty()) {
        m_status = tr("Tuning changes are staged; select Write Params to "
                      "apply.");
    } else {
        m_status.clear();
    }
}

void ConfigExtendedTuningViewModel::updateInitialRollPitchLock()
{
    if (!m_snapshotComplete) {
        m_lockRollPitch = true;
        return;
    }
    bool differs = false;
    const QList<int> initialLockRows{8, 9, 11};
    for (int index : initialLockRows) {
        const int target = index < m_rows.size()
            ? m_rows.at(index).mirrorTargetRow : -1;
        if (target < 0 || target >= m_rows.size()) {
            differs = true;
            break;
        }
        const ExtendedTuningRow &roll = m_rows.at(index);
        const ExtendedTuningRow &pitch = m_rows.at(target);
        differs = roll.exists != pitch.exists
            || (roll.exists
                && !valuesEqual(roll.acceptedValue,
                                pitch.acceptedValue));
    }
    bool helicopter = false;
    for (const ConfigFriendlyParameterValue &parameter :
         m_snapshotParameters) {
        if (parameter.componentId == m_componentId
            && normalizedName(parameter.name)
                == QLatin1String("H_SWASH_TYPE")) {
            helicopter = true;
            break;
        }
    }
    m_lockRollPitch = !differs && !helicopter;
}

bool ConfigExtendedTuningViewModel::stageRowValue(
    int zeroBasedRow, const QVariant &value, bool permitMirror)
{
    if (!CanEditRow(zeroBasedRow)) {
        return false;
    }
    ExtendedTuningRow &row = m_rows[zeroBasedRow];
    if (!row.exists || row.field.readOnly || !value.isValid()) {
        return false;
    }

    QVariant candidate;
    if (row.field.editorKind == ParamField::EditorKind::Combo) {
        const auto option = std::find_if(
            row.field.options.constBegin(), row.field.options.constEnd(),
            [&value](const ParamOption &item) {
                return ConfigExtendedTuningViewModel::valuesEqual(
                    item.value, value);
            });
        if (option == row.field.options.constEnd()) {
            return false;
        }
        candidate = option->value;
    } else {
        bool ok = false;
        const double numeric = value.toDouble(&ok);
        if (!ok || !std::isfinite(numeric)
            || (row.field.hasRange && row.field.enforceRange
                && (numeric < row.field.minimum
                    || numeric > row.field.maximum))) {
            return false;
        }
        candidate = typedValue(value, row.acceptedValue);
    }
    if (valuesEqual(row.field.value, candidate)) {
        return false;
    }

    row.field.value = candidate;
    row.field.status.clear();
    if (permitMirror && m_lockRollPitch && row.mirrorTargetRow >= 0
        && row.mirrorTargetRow < m_rows.size()) {
        ExtendedTuningRow &pitch = m_rows[row.mirrorTargetRow];
        if (pitch.exists && !pitch.field.readOnly) {
            bool numericOk = false;
            const double numeric = candidate.toDouble(&numericOk);
            if (numericOk && std::isfinite(numeric)
                && (!pitch.field.hasRange || !pitch.field.enforceRange
                    || (numeric >= pitch.field.minimum
                        && numeric <= pitch.field.maximum))) {
                pitch.field.value = typedValue(candidate,
                                               pitch.acceptedValue);
                pitch.field.status.clear();
            }
        }
    }
    m_operationStatus.clear();
    updateStatus();
    emit rowsChanged();
    emit stateChanged();
    return true;
}

bool ConfigExtendedTuningViewModel::pendingContains(
    const QString &name) const
{
    const QString normalized = normalizedName(name);
    for (const QVariant &item : m_pending.changes) {
        if (normalizedName(item.toMap()
                               .value(QStringLiteral("name")).toString())
            == normalized) {
            return true;
        }
    }
    return false;
}

void ConfigExtendedTuningViewModel::finishSuccess()
{
    if (!m_pending.active) {
        return;
    }
    const QVariantList changes = m_pending.changes;
    for (const QVariant &item : changes) {
        const QVariantMap entry = item.toMap();
        const QString name = normalizedName(
            entry.value(QStringLiteral("name")).toString());
        const QVariant value = entry.value(QStringLiteral("value"));
        for (ExtendedTuningRow &row : m_rows) {
            if (row.exists && row.resolvedName == name) {
                row.acceptedValue = typedValue(value, row.acceptedValue);
                row.field.value = row.acceptedValue;
                row.field.status = QStringLiteral("✓");
                break;
            }
        }
        for (ConfigFriendlyParameterValue &parameter :
             m_snapshotParameters) {
            if (parameter.componentId == m_componentId
                && normalizedName(parameter.name) == name) {
                parameter.value = value;
                break;
            }
        }
    }
    m_pending = {};
    m_operationStatus = tr("Extended Tuning parameters saved.");
    updateStatus();
    emit rowsChanged();
    emit stateChanged();
}

void ConfigExtendedTuningViewModel::finishFailure(const QString &reason)
{
    if (!m_pending.active) {
        return;
    }
    m_pending = {};
    m_operationStatus = reason.trimmed().isEmpty()
        ? tr("write failed") : reason.trimmed();
    updateStatus();
    emit rowsChanged();
    emit stateChanged();
}

void ConfigExtendedTuningViewModel::finishUncertain(
    const QString &reason, bool requestRefresh)
{
    const bool shouldRefresh = requestRefresh && m_connected;
    m_pending = {};
    m_reconciliationRequired = true;
    m_snapshotReady = false;
    m_snapshotComplete = false;
    m_quadPlaneState = QuadPlaneState::Unknown;
    m_snapshotParameters.clear();
    m_operationStatus = reason;
    rebuildRows();
    updateStatus();
    const QPointer<ConfigExtendedTuningViewModel> guard(this);
    emit structureChanged();
    emit rowsChanged();
    emit stateChanged();
    if (guard && shouldRefresh && m_connected
        && m_reconciliationRequired) {
        emit refreshRequested(m_componentId);
    }
}

QString ConfigExtendedTuningViewModel::normalizedName(
    const QString &name)
{
    return name.trimmed().toUpper();
}

bool ConfigExtendedTuningViewModel::isQuadPlaneParameter(
    const QString &name)
{
    return isQPlaneName(normalizedName(name));
}

bool ConfigExtendedTuningViewModel::valuesEqual(
    const QVariant &left, const QVariant &right)
{
    if (!left.isValid() || !right.isValid()) {
        return !left.isValid() && !right.isValid();
    }
    bool leftOk = false;
    bool rightOk = false;
    const double leftValue = left.toDouble(&leftOk);
    const double rightValue = right.toDouble(&rightOk);
    return leftOk && rightOk
        ? std::abs(leftValue - rightValue) <= 1.0e-6
        : left == right;
}

QVariant ConfigExtendedTuningViewModel::typedValue(
    const QVariant &candidate, const QVariant &reference)
{
    bool ok = false;
    const double numeric = candidate.toDouble(&ok);
    if (!ok || !reference.isValid()) {
        return candidate;
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const int typeId = reference.typeId();
#else
    const int typeId = reference.userType();
#endif
    switch (typeId) {
    case QMetaType::Int:
        return static_cast<int>(std::llround(numeric));
    case QMetaType::UInt:
        return static_cast<uint>(std::llround(numeric));
    case QMetaType::LongLong:
        return static_cast<qlonglong>(std::llround(numeric));
    case QMetaType::ULongLong:
        return static_cast<qulonglong>(std::llround(numeric));
    case QMetaType::Float:
        return static_cast<float>(numeric);
    case QMetaType::Double:
        return numeric;
    default:
        return candidate;
    }
}
