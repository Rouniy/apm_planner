#ifndef CONFIGRAWPARAMS_H
#define CONFIGRAWPARAMS_H

#include "core/parameters/ParameterMetaData.h"
#include "core/parameters/ParameterFileCodec.h"
#include "core/parameters/ParameterStore.h"

#include <QHash>
#include <QMap>
#include <QSet>
#include <QVariantList>
#include <QWidget>

class QCheckBox;
class QIODevice;
class QLabel;
class QLineEdit;
class QPushButton;
class QShortcut;
class QSortFilterProxyModel;
class QSplitter;
class QStandardItem;
class QStandardItemModel;
class QTableView;
class QTimer;
class QToolButton;
class QTreeWidget;

/**
 * Mission Planner-compatible Full Parameter List.
 *
 * The widget deliberately owns only presentation and staged edits. Live
 * values remain in ParameterStore and writes are submitted as one serialized
 * ParameterService batch by ConfigView.
 */
class ConfigRawParams final : public QWidget
{
    Q_OBJECT

public:
    explicit ConfigRawParams(
        const ParameterMetaDataCatalog &catalog = ParameterMetaDataCatalog(),
        QWidget *parent = nullptr, bool enforceMetadataRanges = false);

    void setCatalog(const ParameterMetaDataCatalog &catalog,
                    bool enforceMetadataRanges = false);
    void setParameterSnapshot(const QList<ParameterRecord> &records,
                              int preferredComponent = 1);
    void setConnected(bool connected);

    int selectedComponent() const { return m_componentId; }
    int parameterCount() const { return m_liveRecords.size(); }
    int visibleParameterCount() const;
    int stagedParameterCount() const { return m_stagedValues.size(); }
    QVariant stagedValue(const QString &name) const;
    QString lastStatusText() const;

    bool stageParameter(const QString &name, const QString &expression,
                        bool allowOutOfRange = false,
                        QString *error = nullptr);
    /** Compare an already validated .param file and stage only selected rows. */
    void reviewAndStageParameterFile(const QString &path);
    void clearStagedChanges();

signals:
    void refreshRequested(int componentId);
    void writeRequested(int componentId, QVariantList changes);

public slots:
    void parameterChanged(int componentId, const QString &name,
                          const QVariant &value);
    void parameterWriteAcknowledged(int componentId, const QString &name,
                                    const QVariant &value, int type);
    void parameterWriteFailed(qulonglong transactionId, qulonglong batchId,
                              int componentId, const QString &name,
                              int reason, const QString &message);
    void parameterWriteCancelled(qulonglong transactionId,
                                 qulonglong batchId, int componentId,
                                 const QString &name);
    void parameterBatchSubmitted(qulonglong batchId, int total);
    void parameterBatchProgress(qulonglong batchId, int completed, int total,
                                int succeeded, int failed);
    void parameterBatchCompleted(qulonglong batchId, int succeeded,
                                 int failed);
    void parameterWriteSubmissionFailed(const QString &reason);
    void parameterTargetChanged();

private slots:
    void sourceItemChanged(QStandardItem *item);
    void applySearchFilter();
    void loadFromFile();
    void saveToFile();
    void compareWithFile();
    void writeStagedParameters();
    void toggleTree();

private:
    enum Column
    {
        Command = 0,
        Value,
        DefaultValue,
        Units,
        Options,
        Description,
        Favorite,
        ColumnCount
    };

    static QString normalizedName(const QString &name);
    static QString formattedValue(const QVariant &value);
    static bool equivalent(const QVariant &left, const QVariant &right,
                           ParameterType type);
    static QVariant typedValue(double value, ParameterType type, bool *ok);

    void buildUi();
    void rebuildRows();
    void rebuildPrefixTree();
    void updateRow(const QString &name);
    void updateActionState();
    void setStatus(const QString &message, bool error = false);
    bool stageNumericValue(const QString &name, double value,
                           bool allowOutOfRange, QString *error);
    QMap<QString, QVariant> displayedValues() const;
    bool readParameterFile(const QString &path,
                           QMap<QString, double> *values);
    bool reportUnknownParameters(const QStringList &names,
                                 const QString &prefix);
    QStringList revalidateStagedValues();
    void invalidateOpenBitmaskEditors();
    void loadFavorites();
    void saveFavorites() const;

    ParameterMetaDataCatalog m_catalog;
    QHash<QString, ParameterRecord> m_liveRecords;
    QHash<QString, int> m_sourceRows;
    QMap<QString, QVariant> m_stagedValues;
    QSet<QString> m_rangeOverrides;
    QSet<QString> m_favorites;
    int m_componentId = 1;
    bool m_enforceMetadataRanges = false;
    bool m_connected = false;
    bool m_updatingModel = false;
    bool m_treeCollapsed = false;
    bool m_submissionPending = false;
    qulonglong m_activeBatchId = 0;
    qulonglong m_stateRevision = 0;
    qulonglong m_stagedRevision = 0;

    QSplitter *m_splitter = nullptr;
    QWidget *m_treePanel = nullptr;
    QTreeWidget *m_tree = nullptr;
    QToolButton *m_collapseButton = nullptr;
    QTableView *m_table = nullptr;
    QStandardItemModel *m_model = nullptr;
    QSortFilterProxyModel *m_proxy = nullptr;
    QLineEdit *m_search = nullptr;
    QCheckBox *m_modified = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_loadButton = nullptr;
    QPushButton *m_saveButton = nullptr;
    QPushButton *m_writeButton = nullptr;
    QPushButton *m_refreshButton = nullptr;
    QPushButton *m_compareButton = nullptr;
    QShortcut *m_saveShortcut = nullptr;
    QTimer *m_searchDebounce = nullptr;
};

#endif
