#ifndef CONFIGFLIGHTMODESVIEW_H
#define CONFIGFLIGHTMODESVIEW_H

#include "ConfigFlightModesViewModel.h"

#include <QUrl>
#include <QVector>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QFrame;
class QLabel;
class QPushButton;
class QTimer;

class ConfigFlightModesView final : public QWidget
{
    Q_OBJECT

public:
    explicit ConfigFlightModesView(QWidget *parent = nullptr);
    ~ConfigFlightModesView() override;

    QSize sizeHint() const override;
    ConfigFlightModesViewModel *viewModel() const { return m_viewModel; }

    void setFamily(
        ConfigFlightModesViewModel::Family family,
        const QList<ParamOption> &modeOptions = {});
    void setParameterSnapshot(
        const QList<ConfigFriendlyParameterValue> &parameters,
        int preferredComponent = 1, bool completeSnapshot = true,
        bool preserveStagedEdits = false);
    void setConnected(bool connected);
    void setArmed(bool armed);
    void setHeartbeat(quint32 customMode, bool fresh, bool armed);
    void setHeartbeatFresh(bool fresh);
    bool setRcInput(int oneBasedChannel, int pwm);
    void clearRcInput();

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
    void helpRequested(QUrl url);

private:
    struct RowWidgets
    {
        QFrame *frame = nullptr;
        QLabel *label = nullptr;
        QComboBox *mode = nullptr;
        QCheckBox *simple = nullptr;
        QCheckBox *superSimple = nullptr;
        QLabel *pwm = nullptr;
    };

    void buildUi();
    void syncOptions();
    void syncRows();
    void syncState();
    void updateActiveStyle(QFrame *frame, bool active);
    static int optionIndex(const QComboBox *combo,
                           const QVariant &value);

    ConfigFlightModesViewModel *m_viewModel = nullptr;
    QLabel *m_currentMode = nullptr;
    QLabel *m_currentPwm = nullptr;
    QLabel *m_simpleHeader = nullptr;
    QLabel *m_superSimpleHeader = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_save = nullptr;
    QPushButton *m_refresh = nullptr;
    QPushButton *m_help = nullptr;
    QTimer *m_rcFreshnessTimer = nullptr;
    QVector<RowWidgets> m_rows;
};

#endif // CONFIGFLIGHTMODESVIEW_H
