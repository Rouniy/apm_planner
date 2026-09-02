#include "ConfigHWIDViewModel.h"

#include "DeveloperToolParsers.h"

#include <QMetaType>
#include <QtNumeric>

#include <algorithm>
#include <cmath>

namespace {

struct MavPortName
{
    quint32 id;
    const char *name;
};

// MP10 ConfigHWIDViewModel.MavPortDeviceNames (firmware serial-manager ports).
const MavPortName kMavPortDeviceNames[] = {
    {0, "Unknown"},        {6, "USB0"},          {14, "SERIAL1"},
    {22, "SERIAL2"},       {30, "SERIAL3"},      {38, "SERIAL4"},
    {46, "SERIAL5"},       {54, "SERIAL6"},      {62, "SERIAL7"},
    {70, "SERIAL8"},       {78, "SERIAL9"},      {174, "NET_P1"},
    {182, "NET_P2"},       {190, "NET_P3"},      {198, "NET_P4"},
    {334, "CAN_D1_UC_S1"}, {414, "CAN_D2_UC_S1"}, {494, "SCR_SDEV1"},
    {502, "SCR_SDEV2"},
};

const quint8 kBusTypeUavcan = 3; // Device.BusType.BUS_TYPE_UAVCAN

bool isAsciiDigit(QChar ch)
{
    const ushort u = ch.unicode();
    return u >= '0' && u <= '9';
}

bool rowNameLess(const HwIdRow &left, const HwIdRow &right)
{
    return QString::compare(left.paramName, right.paramName) < 0;
}

} // namespace

ConfigHWIDViewModel::ConfigHWIDViewModel(QObject *parent)
    : QAbstractTableModel(parent)
{
}

int ConfigHWIDViewModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_devices.size();
}

int ConfigHWIDViewModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant ConfigHWIDViewModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_devices.size() ||
        index.column() < 0 || index.column() >= ColumnCount) {
        return QVariant();
    }
    const HwIdRow &row = m_devices.at(index.row());
    if (role == Qt::TextAlignmentRole) {
        switch (index.column()) {
        case DevIDColumn:
        case BusColumn:
        case AddressColumn:
            return QVariant(Qt::AlignRight | Qt::AlignVCenter);
        default:
            return QVariant(Qt::AlignLeft | Qt::AlignVCenter);
        }
    }
    if (role != Qt::DisplayRole && role != SortRole) {
        return QVariant();
    }
    const bool text = role == Qt::DisplayRole;
    switch (index.column()) {
    case ParamNameColumn:
        return row.paramName;
    case DevIDColumn:
        return text ? QVariant(QString::number(row.devId)) : QVariant(row.devId);
    case BusTypeColumn:
        return row.busType;
    case BusColumn:
        return text ? QVariant(QString::number(row.bus)) : QVariant(row.bus);
    case AddressColumn:
        return text ? QVariant(QString::number(row.address)) : QVariant(row.address);
    case DevTypeColumn:
        return row.devType;
    default:
        return QVariant();
    }
}

QVariant ConfigHWIDViewModel::headerData(int section, Qt::Orientation orientation,
                                         int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return QVariant();
    }
    // MP10 DataGrid column headers, verbatim.
    switch (section) {
    case ParamNameColumn:
        return QStringLiteral("ParamName");
    case DevIDColumn:
        return QStringLiteral("DevID");
    case BusTypeColumn:
        return QStringLiteral("BusType");
    case BusColumn:
        return QStringLiteral("Bus");
    case AddressColumn:
        return QStringLiteral("Address");
    case DevTypeColumn:
        return QStringLiteral("DevType");
    default:
        return QVariant();
    }
}

Qt::ItemFlags ConfigHWIDViewModel::flags(const QModelIndex &index) const
{
    if (!index.isValid()) {
        return Qt::NoItemFlags;
    }
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable; // read-only grid
}

void ConfigHWIDViewModel::setParameterSnapshot(const QList<ParameterRecord> &records,
                                               int preferredComponent)
{
    int selectedComponent = preferredComponent;
    const QVector<HwIdRow> rows = BuildRows(records, preferredComponent,
                                            &selectedComponent);
    beginResetModel();
    m_devices = rows;
    m_selectedComponent = selectedComponent;
    endResetModel();
    emit devicesChanged(m_devices.size());
}

bool ConfigHWIDViewModel::IncludesParameter(const QString &paramName)
{
    return (paramName.contains(QLatin1String("_ID")) ||
            paramName.contains(QLatin1String("_DEVID"))) &&
           !paramName.contains(QLatin1String("_IDX")) &&
           !paramName.contains(QLatin1String("FRSKY"));
}

bool ConfigHWIDViewModel::IsMavPortDeviceId(const QString &paramName)
{
    const QLatin1String prefix("MAV");
    const QLatin1String suffix("_DEVID");
    if (!paramName.startsWith(prefix, Qt::CaseInsensitive) ||
        !paramName.endsWith(suffix, Qt::CaseInsensitive) ||
        paramName.size() <= prefix.size() + suffix.size()) {
        return false;
    }
    const int digitsEnd = paramName.size() - suffix.size();
    for (int index = prefix.size(); index < digitsEnd; ++index) {
        if (!isAsciiDigit(paramName.at(index))) {
            return false;
        }
    }
    return true;
}

QString ConfigHWIDViewModel::DecodeMavPortDeviceId(quint32 id)
{
    for (const MavPortName &entry : kMavPortDeviceNames) {
        if (entry.id == id) {
            return QString::fromLatin1(entry.name);
        }
    }
    return QStringLiteral("Unknown (%1)").arg(id);
}

HwIdRow ConfigHWIDViewModel::Decode(const QString &paramName, quint32 id)
{
    HwIdRow row;
    row.paramName = paramName;
    row.devId = static_cast<qint32>(id); // unchecked((int)id)

    if (IsMavPortDeviceId(paramName)) {
        row.busType = QStringLiteral("MAVLink");
        row.devType = DecodeMavPortDeviceId(id);
        return row; // Bus and Address stay 0 like MP10
    }

    const DecodedHardwareId device =
        DeveloperToolParsers::DecodeHardwareId(id, paramName);
    row.busType = device.busTypeName; // BUS_TYPE_ prefix already stripped
    row.bus = device.bus;
    row.address = device.address;
    // MP10 HW ID family selection (note "COMP", not "COMPASS").
    if (device.busType == kBusTypeUavcan) {
        row.devType = QStringLiteral("SENSOR_ID#%1").arg(device.devtype);
    } else if (paramName.contains(QLatin1String("COMP"))) {
        row.devType = DeveloperToolParsers::CompassDeviceTypeName(device.devtype);
    } else if (paramName.contains(QLatin1String("BARO"))) {
        row.devType = DeveloperToolParsers::BaroDeviceTypeName(device.devtype);
    } else if (paramName.contains(QLatin1String("ASP"))) {
        row.devType = DeveloperToolParsers::AirspeedDeviceTypeName(device.devtype);
    } else {
        row.devType = DeveloperToolParsers::ImuDeviceTypeName(device.devtype);
    }
    return row;
}

quint32 ConfigHWIDViewModel::RawId(const QVariant &value)
{
    bool ok = false;
    const int type = value.userType();
    if (type == QMetaType::Float || type == QMetaType::Double) {
        // MP10 casts the double parameter value with (uint): truncation
        // toward zero, negative values wrap.
        const double real = value.toDouble(&ok);
        if (!ok || qIsNaN(real) || qIsInf(real)) {
            return 0;
        }
        return static_cast<quint32>(static_cast<qint64>(std::trunc(real)));
    }
    const qlonglong integer = value.toLongLong(&ok);
    if (ok) {
        return static_cast<quint32>(integer);
    }
    const double real = value.toDouble(&ok);
    if (!ok || qIsNaN(real) || qIsInf(real)) {
        return 0;
    }
    return static_cast<quint32>(static_cast<qint64>(std::trunc(real)));
}

QVector<HwIdRow> ConfigHWIDViewModel::BuildRows(const QList<ParameterRecord> &records,
                                                int preferredComponent,
                                                int *selectedComponent)
{
    QList<int> components;
    for (const ParameterRecord &record : records) {
        if (!components.contains(record.key.componentId)) {
            components.append(record.key.componentId);
        }
    }
    std::sort(components.begin(), components.end());
    const int component = components.contains(preferredComponent)
                              ? preferredComponent
                              : components.value(0, preferredComponent);
    if (selectedComponent) {
        *selectedComponent = component;
    }

    QVector<HwIdRow> rows;
    for (const ParameterRecord &record : records) {
        if (record.key.componentId != component ||
            !IncludesParameter(record.key.name)) {
            continue;
        }
        rows.append(Decode(record.key.name, RawId(record.value)));
    }
    std::stable_sort(rows.begin(), rows.end(), rowNameLess);
    return rows;
}
