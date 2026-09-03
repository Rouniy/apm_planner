#ifndef CONFIGOSDVIEWMODEL_H
#define CONFIGOSDVIEWMODEL_H

#include "ParamField.h"

#include <QList>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>

/*
 * One complete MP10 Onboard OSD item. The accepted values are the last
 * committed vehicle snapshot (or a confirmed write); enabled/x/y are local
 * staged values. Unlike MP10's immediate writes, the Qt port stages edits and
 * submits one deterministic exact-target batch so a failed or replaced target
 * cannot silently consume part of a drag operation.
 */
struct ConfigOSDItem
{
    int screen = 0;
    QString name;
    QString enableParameter;
    QString xParameter;
    QString yParameter;

    bool acceptedEnabled = false;
    int acceptedX = 0;
    int acceptedY = 0;

    bool enabled = false;
    int x = 0;
    int y = 0;

    bool enableDirty() const { return enabled != acceptedEnabled; }
    bool xDirty() const { return x != acceptedX; }
    bool yDirty() const { return y != acceptedY; }
    bool dirty() const { return enableDirty() || xDirty() || yDirty(); }
};

inline bool operator==(const ConfigOSDItem &left,
                       const ConfigOSDItem &right)
{
    return left.screen == right.screen
        && left.name == right.name
        && left.enableParameter == right.enableParameter
        && left.xParameter == right.xParameter
        && left.yParameter == right.yParameter
        && left.acceptedEnabled == right.acceptedEnabled
        && left.acceptedX == right.acceptedX
        && left.acceptedY == right.acceptedY
        && left.enabled == right.enabled
        && left.x == right.x
        && left.y == right.y;
}

Q_DECLARE_METATYPE(ConfigOSDItem)
Q_DECLARE_METATYPE(QList<ConfigOSDItem>)

/*
 * Transport-neutral state for Mission Planner 10's CONFIG > Onboard OSD.
 *
 * The owner supplies one committed parameter snapshot, forwards the single
 * writeParamsRequested batch to QGCUASParamManager::writeParameters(), then
 * reports the batch lifecycle. Every callback is accepted only for the exact
 * component, snapshot revision and submitted batch id owned by this model.
 */
class ConfigOSDViewModel final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString Title READ Title CONSTANT)
    Q_PROPERTY(QString Intro READ Intro CONSTANT)
    Q_PROPERTY(QString Status READ Status NOTIFY statusChanged)
    Q_PROPERTY(int SelectedScreen READ SelectedScreen NOTIFY selectedScreenChanged)
    Q_PROPERTY(int DirtyFieldCount READ DirtyFieldCount NOTIFY dirtyChanged)
    Q_PROPERTY(bool HasDirtyChanges READ HasDirtyChanges NOTIFY dirtyChanged)
    Q_PROPERTY(bool Busy READ Busy NOTIFY stateChanged)
    Q_PROPERTY(bool CanEdit READ CanEdit NOTIFY stateChanged)
    Q_PROPERTY(bool CanWrite READ CanWrite NOTIFY stateChanged)

public:
    explicit ConfigOSDViewModel(QObject *parent = nullptr);
    ~ConfigOSDViewModel() override;

    // Exact MP10 labels and preview geometry.
    static QString Title();
    static QString Intro();
    static int Columns() { return 30; }
    static int Rows() { return 16; }
    static int CellWidth() { return 26; }
    static int CellHeight() { return 24; }
    static int CanvasWidth() { return Columns() * CellWidth(); }
    static int CanvasHeight() { return Rows() * CellHeight(); }

    static QString NoParametersStatus();
    static QString ScreenStatus(int screen, int itemCount);
    static QString OfflineStatus();
    static QString UnreadyStatus();
    static QString NoChangesStatus();
    static QString WritingStatus();
    static QString SuccessStatus();
    static QString FailureStatus(const QString &detail = QString());
    static QString CancelledStatus();
    static QString TargetChangedStatus();
    static QString TimeoutStatus();

    QString Status() const { return m_status; }
    QList<int> Screens() const { return m_screens; }
    int SelectedScreen() const { return m_selectedScreen; }
    QList<ConfigOSDItem> Items() const;
    QList<ConfigOSDItem> AllItems() const { return m_items; }
    bool Item(int screen, const QString &name, ConfigOSDItem *item) const;

    int ComponentId() const { return m_componentId; }
    quint64 SnapshotRevision() const { return m_snapshotRevision; }
    bool Connected() const { return m_connected; }
    bool SnapshotReady() const { return m_snapshotReady; }
    bool Busy() const { return m_batch.active; }
    bool CanEdit() const;
    bool CanWrite() const;
    bool HasDirtyChanges() const { return DirtyFieldCount() != 0; }
    int DirtyFieldCount() const;
    QVariantList DirtyChanges() const;
    qulonglong PendingBatchId() const { return m_batch.batchId; }

    void setConnected(bool connected);
    void setParameterSnapshot(
        const QList<ConfigFriendlyParameterValue> &parameters,
        int preferredComponent = 1);
    void parameterTargetChanged();

    bool selectScreen(int screen);
    bool setItemEnabled(int screen, const QString &name, bool enabled);
    bool setItemX(int screen, const QString &name, int x);
    bool setItemY(int screen, const QString &name, int y);
    bool setItemPosition(int screen, const QString &name, int x, int y);
    bool EnableAll(bool enabled);
    bool Discard();
    bool WriteChanges();

    void setBatchTimeoutForTesting(int milliseconds);
    static int DefaultBatchTimeoutMs() { return 60000; }

public slots:
    void parameterBatchSubmitted(int componentId, qulonglong batchId);
    void parameterBatchProgress(qulonglong batchId, int completed, int total,
                                int succeeded, int failed);
    void parameterWriteFailed(qulonglong batchId, int componentId,
                              const QString &name, const QString &reason);
    void parameterBatchCompleted(qulonglong batchId, int succeeded,
                                 int failed);
    void parameterBatchCancelled(qulonglong batchId);
    void parameterWriteSubmissionFailed(const QString &reason);

signals:
    void screensChanged();
    void selectedScreenChanged(int screen);
    void itemsChanged();
    void itemChanged(int screen, const QString &name);
    void dirtyChanged();
    void statusChanged(const QString &status);
    void stateChanged();
    void writeParamsRequested(int componentId, QVariantList changes);

private:
    struct Batch
    {
        bool active = false;
        qulonglong batchId = 0;
        quint64 revision = 0;
        quint64 serial = 0;
        int total = 0;
        QVariantList changes;
        QSet<QString> failedNames;
        QStringList failureDetails;
        QHash<qulonglong, QSet<QString>> earlyFailedNames;
        QHash<qulonglong, QStringList> earlyFailureDetails;
        bool earlyCompletion = false;
        qulonglong earlyBatchId = 0;
        int earlySucceeded = 0;
        int earlyFailed = 0;
    };

    static QString normalizedName(const QString &name);
    static QString normalizedItemName(const QString &name);
    static int snapshotPosition(const QVariant &value);
    ConfigOSDItem *itemFor(int screen, const QString &name);
    const ConfigOSDItem *itemFor(int screen, const QString &name) const;
    bool setItemCoordinate(int screen, const QString &name, int value,
                           bool horizontal);
    void notifyItemChanged(const ConfigOSDItem &item, int previousDirtyCount);
    bool ownsBatch(qulonglong batchId) const;
    bool batchContains(const QString &parameter) const;
    void finishBatch(int succeeded, int failed);
    void acceptPendingChanges(const QSet<QString> &excludedNames);
    void abandonBatch(const QString &status);
    void invalidateSnapshot(const QString &status);
    void setStatus(const QString &status);
    void updateScreenStatus();

    QList<ConfigOSDItem> m_items;
    QList<int> m_screens;
    QString m_status;
    Batch m_batch;
    int m_selectedScreen = 0;
    int m_componentId = 1;
    quint64 m_snapshotRevision = 0;
    quint64 m_batchSerial = 0;
    int m_batchTimeoutMs = DefaultBatchTimeoutMs();
    bool m_connected = false;
    bool m_snapshotReady = false;
};

#endif // CONFIGOSDVIEWMODEL_H
