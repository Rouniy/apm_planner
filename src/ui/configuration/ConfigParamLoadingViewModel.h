#ifndef CONFIGPARAMLOADINGVIEWMODEL_H
#define CONFIGPARAMLOADINGVIEWMODEL_H

#include <QObject>
#include <QString>

class ConfigParamLoadingViewModel final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int progressPercent READ progressPercent NOTIFY stateChanged)
    Q_PROPERTY(QString status READ status NOTIFY stateChanged)
    Q_PROPERTY(QString count READ count NOTIFY stateChanged)
    Q_PROPERTY(bool parametersReady READ parametersReady NOTIFY stateChanged)

public:
    explicit ConfigParamLoadingViewModel(QObject *parent = nullptr);

    int progressPercent() const;
    QString status() const;
    QString count() const;
    bool parametersReady() const;
    int received() const;
    int reported() const;
    bool loadingCancelled() const;
    QString failure() const;

    static bool hasAllParameters(int received, int reported);

public slots:
    void reset();
    void setState(int received, int reported, bool loadingCancelled = false,
                  const QString &failure = QString());
    void setRequesting();
    void setStopping();

signals:
    void stateChanged();

private:
    enum class TransientState
    {
        None,
        Requesting,
        Stopping
    };

    QString computedStatus() const;
    void updateDerivedState();

    int m_received = 0;
    int m_reported = 0;
    int m_progressPercent = 0;
    bool m_loadingCancelled = false;
    QString m_failure;
    QString m_status;
    QString m_count;
    TransientState m_transientState = TransientState::None;
};

#endif
