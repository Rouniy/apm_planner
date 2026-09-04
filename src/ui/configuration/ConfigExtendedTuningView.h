#ifndef CONFIGEXTENDEDTUNINGVIEW_H
#define CONFIGEXTENDEDTUNINGVIEW_H

#include "ConfigExtendedTuningViewModel.h"

#include <QVector>
#include <QWidget>

class QAction;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGridLayout;
class QGroupBox;
class QLabel;
class QPushButton;
class QToolButton;

class ConfigExtendedTuningView final : public QWidget
{
    Q_OBJECT

public:
    explicit ConfigExtendedTuningView(QWidget *parent = nullptr);
    ~ConfigExtendedTuningView() override;

    QSize sizeHint() const override;
    ConfigExtendedTuningViewModel *viewModel() const { return m_viewModel; }

    void setCatalog(const ParameterMetaDataCatalog &catalog,
                    bool enforceMetadataRanges = true);
    void setParameterSnapshot(
        const QList<ConfigFriendlyParameterValue> &parameters,
        int preferredComponent = 1, bool completeSnapshot = true,
        bool preserveStagedEdits = false);
    void setConnected(bool connected);
    void setArmed(bool armed);
    void setHeartbeatFresh(bool fresh);
    void setHeartbeat(bool fresh, bool armed);

public slots:
    void parameterChanged(int componentId, const QString &name,
                          const QVariant &value);
    void parameterWriteSubmitted(quint64 requestId, qulonglong batchId);
    void parameterWriteSubmissionFailed(quint64 requestId,
                                        const QString &reason);
    void parameterWriteFailed(qulonglong batchId, int componentId,
                              const QString &name, const QString &reason);
    void parameterWriteCancelled(qulonglong batchId, int componentId,
                                 const QString &name);
    void parameterBatchCompleted(qulonglong batchId,
                                 int succeeded, int failed);
    void refreshFailed(const QString &reason);
    void refreshCanceled();

signals:
    void writeRequested(quint64 requestId, int componentId,
                        QVariantList changes);
    void refreshRequested(int componentId);

private:
    struct RowEditors
    {
        QWidget *row = nullptr;
        QLabel *label = nullptr;
        QComboBox *combo = nullptr;
        QDoubleSpinBox *numeric = nullptr;
        QToolButton *bitmask = nullptr;
        QVector<QAction *> bitActions;
        QLabel *units = nullptr;
        QLabel *status = nullptr;
    };

    void buildUi();
    void rebuildRows();
    RowEditors makeEditors(int rowIndex, const ExtendedTuningRow &row,
                           QWidget *parent);
    void syncRows();
    void syncRow(int rowIndex);
    void syncState();
    void stageBitmask(int rowIndex);
    void confirmLargeIncrease(const QStringList &parameterNames);
    static int optionIndex(const QComboBox *combo, const QVariant &value);
    static int decimalsFor(const ParamField &field);
    static QString bitmaskSummary(const ExtendedTuningRow &row);
    static QString fieldToolTip(const ExtendedTuningRow &row);
    static QString groupObjectSuffix(const QString &title);

    ConfigExtendedTuningViewModel *m_viewModel = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_intro = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_write = nullptr;
    QPushButton *m_refreshParams = nullptr;
    QPushButton *m_refreshScreen = nullptr;
    QCheckBox *m_lockRollPitch = nullptr;
    QWidget *m_cards = nullptr;
    QGridLayout *m_cardsLayout = nullptr;
    QVector<RowEditors> m_rowEditors;
};

#endif // CONFIGEXTENDEDTUNINGVIEW_H
