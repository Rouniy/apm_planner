#ifndef CONFIGTRADHELI4VIEW_H
#define CONFIGTRADHELI4VIEW_H

#include "ConfigTradHeli4ViewModel.h"

#include <QHash>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QVBoxLayout;

class ConfigTradHeli4View final : public QWidget
{
    Q_OBJECT

public:
    explicit ConfigTradHeli4View(QWidget *parent = nullptr);
    ~ConfigTradHeli4View() override;

    QSize sizeHint() const override;
    ConfigTradHeli4ViewModel *viewModel() const { return m_viewModel; }

    void setCatalog(const ParameterMetaDataCatalog &catalog,
                    bool enforceMetadataRanges = true);
    void setParameterSnapshot(
        const QList<ConfigFriendlyParameterValue> &parameters,
        int preferredComponent = 1);
    void setConnected(bool connected);
    void setArmed(bool armed);
    void setActive(bool active);

public slots:
    void parameterChanged(int componentId, const QString &name,
                          const QVariant &value);
    void parameterWriteSubmitted(quint64 requestId, qulonglong batchId);
    void parameterWriteSubmissionFailed(quint64 requestId,
                                        const QString &reason);
    void parameterWriteFailed(qulonglong batchId, int componentId,
                              const QString &name, const QString &reason);
    void parameterBatchCompleted(qulonglong batchId,
                                 int succeeded, int failed);
    void refreshFailed(const QString &reason);
    void refreshCanceled();

signals:
    void writeRequested(quint64 requestId, int componentId,
                        const QString &name, const QVariant &value);
    void refreshRequested(int componentId);
    void safetyWarning(const QString &warning);

private:
    struct FieldEditors
    {
        QWidget *row = nullptr;
        QLabel *label = nullptr;
        QCheckBox *check = nullptr;
        QComboBox *combo = nullptr;
        QDoubleSpinBox *numeric = nullptr;
        QLabel *units = nullptr;
        QLabel *status = nullptr;
    };

    void buildUi();
    void rebuildFields();
    FieldEditors makeEditors(const ParamField &field, QWidget *parent,
                             bool compactServoRow);
    void syncField(const QString &name);
    void syncState();
    bool confirmManualOverride(const QVariant &value);
    const ParamField *findField(const QString &name,
                                const QList<ParamField> &fields) const;
    static QString objectSuffix(const QString &name);

    ConfigTradHeli4ViewModel *m_viewModel = nullptr;
    QPushButton *m_refresh = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_safetyWarning = nullptr;
    QWidget *m_sections = nullptr;
    QVBoxLayout *m_sectionsLayout = nullptr;
    QHash<QString, FieldEditors> m_fieldEditors;
};

#endif // CONFIGTRADHELI4VIEW_H
