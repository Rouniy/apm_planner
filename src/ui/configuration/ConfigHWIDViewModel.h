#ifndef CONFIGHWIDVIEWMODEL_H
#define CONFIGHWIDVIEWMODEL_H

#include "core/parameters/ParameterStore.h"

#include <QAbstractTableModel>
#include <QList>
#include <QString>
#include <QVector>

/*
 * Mission Planner 10 SETUP > HW ID
 * (ViewModels/GCSViews/ConfigurationView/ConfigHWIDViewModel.cs).
 *
 * One row per *_ID / *_DEVID parameter of the selected component, decoded
 * either as a numbered MAVn_DEVID firmware port or as an ArduPilot device id
 * (bus type, bus, address, device type).
 */
struct HwIdRow
{
    QString paramName;
    qint32 devId = 0;      // shown as a signed 32-bit value like MP10
    QString busType;
    int bus = 0;
    int address = 0;
    QString devType;

    bool operator==(const HwIdRow &other) const
    {
        return paramName == other.paramName && devId == other.devId &&
               busType == other.busType && bus == other.bus &&
               address == other.address && devType == other.devType;
    }
    bool operator!=(const HwIdRow &other) const { return !(*this == other); }
};

class ConfigHWIDViewModel final : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum Column
    {
        ParamNameColumn,
        DevIDColumn,
        BusTypeColumn,
        BusColumn,
        AddressColumn,
        DevTypeColumn,
        ColumnCount
    };
    Q_ENUM(Column)

    // Numeric columns expose their raw value under this role so a proxy can
    // sort them numerically; DisplayRole is always text.
    static constexpr int SortRole = Qt::UserRole;

    explicit ConfigHWIDViewModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

    // Rebuilds the rows from a committed ParameterStore snapshot. Only the
    // preferred component is shown; when it is absent the lowest component of
    // the snapshot is used. An empty snapshot clears the table.
    void setParameterSnapshot(const QList<ParameterRecord> &records,
                              int preferredComponent = 1);
    QVector<HwIdRow> Devices() const { return m_devices; }
    int selectedComponent() const { return m_selectedComponent; }

    // --- MP10 static helpers ---
    // Parameter names shown by the page: contains "_ID" or "_DEVID", but
    // neither "_IDX" nor "FRSKY" (ordinal, case-sensitive like MP10).
    static bool IncludesParameter(const QString &paramName);
    // Case-insensitive MAV<digits>_DEVID (at least one digit).
    static bool IsMavPortDeviceId(const QString &paramName);
    // Firmware port mapping for MAVn_DEVID values, "Unknown (N)" otherwise.
    static QString DecodeMavPortDeviceId(quint32 id);
    // Full MP10 row decode for one parameter value.
    static HwIdRow Decode(const QString &paramName, quint32 id);
    // Raw 32-bit pattern of a parameter value (negative INT32 values wrap
    // like MP10's unchecked cast).
    static quint32 RawId(const QVariant &value);
    static QVector<HwIdRow> BuildRows(const QList<ParameterRecord> &records,
                                      int preferredComponent,
                                      int *selectedComponent = nullptr);

signals:
    void devicesChanged(int count);

private:
    QVector<HwIdRow> m_devices;
    int m_selectedComponent = 1;
};

#endif
