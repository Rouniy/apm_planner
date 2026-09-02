#ifndef PREFLIGHTCHECKLISTMODEL_H
#define PREFLIGHTCHECKLISTMODEL_H

#include <QAbstractListModel>
#include <QHash>
#include <QString>
#include <QVariant>
#include <QVector>

#include <memory>

class QSettings;

struct PreflightTelemetry
{
    bool connected = false;
    int gpsFixType = 0;
    int satelliteCount = 0;
    double linkQuality = 0.0;
    double batteryVoltage = 0.0;
    QString mode = QStringLiteral("UNKNOWN");
    double altitude = 0.0;
};

class PreflightChecklistModel final : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Role
    {
        IdRole = Qt::UserRole + 1,
        DescriptionRole,
        ValueRole,
        SatisfiedRole,
        ManualRole,
        AvailableRole,
        ForegroundRole
    };
    Q_ENUM(Role)

    explicit PreflightChecklistModel(QObject *parent = nullptr);
    explicit PreflightChecklistModel(QSettings *settings,
                                     QObject *parent = nullptr);
    ~PreflightChecklistModel() override;

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index,
                  int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    PreflightTelemetry telemetry() const;
    void setTelemetry(const PreflightTelemetry &telemetry);
    bool setManualState(int row, bool checked);

    static QString settingsGroup();

private:
    enum class Source
    {
        None,
        GpsFix,
        SatelliteCount,
        LinkQuality,
        BatteryVoltage,
        Mode,
        Altitude
    };

    enum class Condition
    {
        Manual,
        LessThan,
        GreaterThan,
        GreaterThanOrEqual
    };

    struct CheckItem
    {
        QString id;
        QString description;
        QString valueTemplate;
        Source source = Source::None;
        Condition condition = Condition::Manual;
        double triggerValue = 0.0;
        bool manualState = false;
    };

    static QVector<CheckItem> defaultItems();
    QVariant sourceValue(const CheckItem &item) const;
    QString displayValue(const CheckItem &item) const;
    bool isAvailable(const CheckItem &item) const;
    bool isSatisfied(const CheckItem &item) const;
    void loadManualStates();
    void saveManualState(const CheckItem &item);

    QVector<CheckItem> m_items;
    PreflightTelemetry m_telemetry;
    std::unique_ptr<QSettings> m_ownedSettings;
    QSettings *m_settings = nullptr;
};

#endif // PREFLIGHTCHECKLISTMODEL_H
