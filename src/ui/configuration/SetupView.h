#ifndef SETUPVIEW_H
#define SETUPVIEW_H

#include <QHash>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QVariant>
#include <QWidget>

class BackstageView;
class QGCUASParamManager;
class UASInterface;

class SetupView final : public QWidget
{
    Q_OBJECT

public:
    explicit SetupView(QWidget *parent = nullptr);

signals:
    void advancedModeChanged(bool advanced);

public slots:
    void advModeChanged(bool advanced);

private slots:
    void activeUASSet(UASInterface *uas);
    void vehicleConnected();
    void vehicleDisconnected();
    void parameterChanged(int uas, int component, int parameterCount,
                          int parameterId, QString parameterName,
                          QVariant value);
    void parameterListUpToDate(int component);
    void parameterListReadyChanged(bool ready);
    void parameterListLoadFailed(const QString &reason);
    void parameterListLoadCanceled();
    void stopParameterLoading();
    void retryParameterLoading();

private:
    void buildPages();
    void refreshPageVisibility();
    void refreshLoadingOverlay();
    void syncConnectionState();
    void resetConnectionPages();
    void resetParameterProgress();
    bool hasConnectedLink() const;
    bool currentPageRequiresParameters() const;

    BackstageView *m_backstage = nullptr;
    QPointer<UASInterface> m_uas;
    QPointer<QGCUASParamManager> m_parameterManager;
    QHash<int, QSet<int>> m_receivedParameterIds;
    QHash<int, int> m_expectedParameterCounts;
    QString m_parameterLoadFailure;
    int m_parameterProgress = -1;
    bool m_connected = false;
    bool m_parametersReady = false;
    bool m_advanced = false;
};

#endif
