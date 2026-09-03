#ifndef COTIDENTITYMODEL_H
#define COTIDENTITYMODEL_H

#include <QAbstractTableModel>
#include <QByteArray>
#include <QHash>
#include <QJsonValue>
#include <QString>
#include <QVariant>
#include <QVector>

struct CotIdentityRecord
{
    // MP10 persists this grid cell verbatim.  Keeping its JSON type means
    // values such as "042", 999 and null survive a load/save cycle even
    // though only an exact decimal system-id cell can match live telemetry.
    QJsonValue systemId = QJsonValue(0);
    QString eventUid;
    bool includeTakv = false;
    QString contactCallsign;
    QString contactEndpoint;
    QString vmf;

    bool operator==(const CotIdentityRecord &other) const;
    bool operator!=(const CotIdentityRecord &other) const
    {
        return !(*this == other);
    }
};

/**
 * Editable MP10 CoT identity grid.
 *
 * MP10 treats the System ID column as an untyped persisted grid cell.  Rows,
 * their order, duplicates and primitive JSON values are therefore preserved;
 * a live MAVLink id uses the first row whose cell text is exactly its ordinary
 * decimal representation. Empty/null persisted input clears the model, while
 * malformed or oversized input is rejected without replacing current rows.
 */
class CotIdentityModel final : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum Column
    {
        SystemIdColumn = 0,
        EventUidColumn,
        TakvColumn,
        ContactCallsignColumn,
        ContactEndpointColumn,
        VmfColumn,
        ColumnCount
    };

    enum DataRole
    {
        SystemIdRole = Qt::UserRole + 1,
        EventUidRole,
        TakvRole,
        ContactCallsignRole,
        ContactEndpointRole,
        VmfRole
    };

    static constexpr int MaxRows = 1024;
    static constexpr int MaxTextLength = 4096;
    static constexpr int MaxJsonBytes = 2 * 1024 * 1024;

    explicit CotIdentityModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index,
                  int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    bool setData(const QModelIndex &index, const QVariant &value,
                 int role = Qt::EditRole) override;
    bool insertRows(int row, int count,
                    const QModelIndex &parent = QModelIndex()) override;
    bool removeRows(int row, int count,
                    const QModelIndex &parent = QModelIndex()) override;
    QHash<int, QByteArray> roleNames() const override;

    bool appendIdentity(const CotIdentityRecord &identity);
    bool insertIdentity(int row, const CotIdentityRecord &identity);
    int rowForSystemId(int systemId) const;
    bool identityForSystemId(int systemId, CotIdentityRecord *identity) const;
    CotIdentityRecord identityAt(int row) const;
    QVector<CotIdentityRecord> identities() const;
    void setIdentities(const QVector<CotIdentityRecord> &identities);

    QByteArray toJson() const;
    bool loadJson(const QByteArray &json, QString *error = nullptr);
    bool loadSetting(const QVariant &storedValue, QString *error = nullptr);

    static QByteArray encodeJson(
        const QVector<CotIdentityRecord> &identities);
    static bool decodeJson(const QByteArray &json,
                           QVector<CotIdentityRecord> *identities,
                           QString *error = nullptr);
    static bool isValidSystemId(int systemId);
    static QString systemIdText(const QJsonValue &value);

private:
    static CotIdentityRecord boundedIdentity(
        const CotIdentityRecord &identity);
    static bool sameText(const QString &left, const QString &right);
    static bool isSupportedCell(const QJsonValue &value);
    static bool isBoundedCell(const QJsonValue &value);

    QVector<CotIdentityRecord> m_identities;
};

#endif
