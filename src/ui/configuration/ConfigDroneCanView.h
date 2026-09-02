#ifndef CONFIGDRONECANVIEW_H
#define CONFIGDRONECANVIEW_H

#include "ConfigDroneCanViewModel.h"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

class ConfigDroneCanView final : public QWidget
{
    Q_OBJECT

public:
    explicit ConfigDroneCanView(QWidget *parent = nullptr);
    ~ConfigDroneCanView() override;

    QSize sizeHint() const override;
    ConfigDroneCanViewModel *viewModel() const { return m_viewModel; }

    void setVehicleConnected(bool connected);
    void setVehicleArmed(bool armed);

public slots:
    void canFrameReceived(int bus, quint32 id, const QByteArray &data,
                          bool canFd, qint64 nowMs);
    void forwardingAckReceived(int result);
    void forwardingSendFailed(const QString &reason);

signals:
    void canForwardingRequested(int componentId, int bus, bool enable);

private:
    void syncFromModel();
    void rebuildNodes();
    void rebuildParameters();
    void applyParameterFilter();
    void syncSelection();

    ConfigDroneCanViewModel *m_viewModel = nullptr;
    QComboBox *m_interfaceSelector = nullptr;
    QPushButton *m_connectButton = nullptr;
    QPushButton *m_refreshButton = nullptr;
    QPushButton *m_filterButton = nullptr;
    QPushButton *m_statsButton = nullptr;
    QPushButton *m_inspectorButton = nullptr;
    QCheckBox *m_logCheckBox = nullptr;
    QLabel *m_statusLabel = nullptr;
    QTableWidget *m_nodesTable = nullptr;
    QLabel *m_nodeStatusLabel = nullptr;
    QPushButton *m_getParametersButton = nullptr;
    QLineEdit *m_parameterSearch = nullptr;
    QCheckBox *m_modifiedOnlyCheckBox = nullptr;
    QTableWidget *m_paramsTable = nullptr;
    QList<QPushButton *> m_nodeOperationButtons;
};

#endif // CONFIGDRONECANVIEW_H
