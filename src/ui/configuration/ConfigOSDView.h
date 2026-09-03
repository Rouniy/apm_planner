#ifndef CONFIGOSDVIEW_H
#define CONFIGOSDVIEW_H

#include "ConfigOSDViewModel.h"

#include <QHash>
#include <QWidget>

class ConfigOSDLayoutCanvas;
class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QScrollArea;
class QSpinBox;

class ConfigOSDView final : public QWidget
{
    Q_OBJECT

public:
    explicit ConfigOSDView(QWidget *parent = nullptr);
    ~ConfigOSDView() override;

    QSize sizeHint() const override;
    ConfigOSDViewModel *viewModel() const { return m_viewModel; }

    void setParameterSnapshot(
        const QList<ConfigFriendlyParameterValue> &parameters,
        int preferredComponent = 1);
    void setConnected(bool connected);

public slots:
    void parameterTargetChanged();
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
    void refreshRequested(int componentId);
    void writeParamsRequested(int componentId, QVariantList changes);

private:
    struct ItemEditors
    {
        QWidget *row = nullptr;
        QCheckBox *enabled = nullptr;
        QLabel *name = nullptr;
        QSpinBox *x = nullptr;
        QSpinBox *y = nullptr;
    };

    void buildUi();
    void rebuildScreens();
    void syncItems();
    void rebuildItemEditors(const QList<ConfigOSDItem> &items);
    void syncItemEditors(const QList<ConfigOSDItem> &items);
    void syncState();
    void syncStatus();
    void requestRefresh();
    void writeChanges();
    void discardChanges();
    void canvasPositionEdited(const QString &key, int x, int y);

    static QString itemKey(int screen, const QString &name);
    static QString itemObjectSuffix(int screen, const QString &name);

    ConfigOSDViewModel *m_viewModel = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_intro = nullptr;
    QPushButton *m_refresh = nullptr;
    QComboBox *m_screen = nullptr;
    QPushButton *m_enableAll = nullptr;
    QPushButton *m_disableAll = nullptr;
    QPushButton *m_tuningSlots = nullptr;
    QLabel *m_status = nullptr;
    ConfigOSDLayoutCanvas *m_canvas = nullptr;
    QScrollArea *m_itemScroll = nullptr;
    QPushButton *m_write = nullptr;
    QPushButton *m_discard = nullptr;
    QHash<QString, ItemEditors> m_itemEditors;
};

#endif // CONFIGOSDVIEW_H
