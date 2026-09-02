#ifndef FLIGHTPLANNERMISSIONMODEL_H
#define FLIGHTPLANNERMISSIONMODEL_H

#include "WpRow.h"

#include <QAbstractTableModel>
#include <QVector>

#include <memory>
#include <vector>

class FlightPlannerMissionModel final : public QAbstractTableModel
{
    Q_OBJECT
    Q_PROPERTY(QString MissionType READ MissionType WRITE setMissionType
               NOTIFY missionTypeChanged)
    Q_PROPERTY(QString AltUnit READ AltUnit NOTIFY altitudePresentationChanged)
    Q_PROPERTY(double AltitudeMultiplier READ AltitudeMultiplier
               NOTIFY altitudePresentationChanged)
    Q_PROPERTY(QString DistanceUnit READ DistanceUnit
               NOTIFY distancePresentationChanged)
    Q_PROPERTY(double DistanceMultiplier READ DistanceMultiplier
               NOTIFY distancePresentationChanged)

public:
    enum class MissionStore
    {
        Mission,
        Fence,
        Rally
    };
    Q_ENUM(MissionStore)

    enum Column
    {
        NumberColumn,
        CommandColumn,
        P1Column,
        P2Column,
        P3Column,
        P4Column,
        LatColumn,
        LngColumn,
        AltColumn,
        FrameColumn,
        GradColumn,
        AngleColumn,
        DistColumn,
        AzColumn,
        ZoneColumn,
        EastingColumn,
        NorthingColumn,
        MgrsColumn,
        ColumnCount
    };
    Q_ENUM(Column)

    explicit FlightPlannerMissionModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index,
                  int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex &index, const QVariant &value,
                 int role = Qt::EditRole) override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    bool removeRows(int row, int count,
                    const QModelIndex &parent = QModelIndex()) override;

    QString MissionType() const;
    QString AltUnit() const;
    double AltitudeMultiplier() const;
    QString DistanceUnit() const;
    double DistanceMultiplier() const;
    MissionStore missionStore() const;
    WpRow *rowAt(int row) const;
    QVector<WpRowData> rows(MissionStore store) const;
    int storeRowCount(MissionStore store) const;
    bool setRouteMetrics(int row, const QString &gradient,
                         const QString &angle, const QString &distance,
                         const QString &azimuth);

    static QString storeName(MissionStore store);
    static bool storeForName(const QString &name, MissionStore *store);

public slots:
    void setMissionType(const QString &type);
    void setMissionStore(MissionStore store);
    WpRow *appendRow(const WpRowData &data = WpRowData());
    WpRow *insertRowData(int row, const WpRowData &data = WpRowData());
    bool moveWaypointUp(int row);
    bool moveWaypointDown(int row);
    void clearActiveStore();
    void replaceStore(MissionStore store, const QVector<WpRowData> &rows);
    void appendStore(MissionStore store, const QVector<WpRowData> &rows);
    void setAltitudePresentation(double multiplier, const QString &unit);
    void setDistancePresentation(double multiplier, const QString &unit);

signals:
    void missionTypeChanged(const QString &type);
    void rowsChanged(FlightPlannerMissionModel::MissionStore store);
    void altitudePresentationChanged();
    void distancePresentationChanged();

private:
    using Store = std::vector<std::unique_ptr<WpRow>>;

    Store &store(MissionStore store);
    const Store &store(MissionStore store) const;
    Store &activeStore();
    const Store &activeStore() const;
    std::unique_ptr<WpRow> makeRow(const WpRowData &data);
    void renumber(Store &rows);
    void publishRowChange(WpRow *row);

    MissionStore m_activeStore = MissionStore::Mission;
    Store m_missionRows;
    Store m_fenceRows;
    Store m_rallyRows;
    bool m_mutating = false;
    double m_altitudeMultiplier = 1.0;
    QString m_altitudeUnit = QStringLiteral("m");
    double m_distanceMultiplier = 1.0;
    QString m_distanceUnit = QStringLiteral("m");
};

Q_DECLARE_METATYPE(FlightPlannerMissionModel::MissionStore)

#endif
