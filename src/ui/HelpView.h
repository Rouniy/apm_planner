#ifndef HELPVIEW_H
#define HELPVIEW_H

#include <QWidget>

class QLabel;
class QPushButton;

class HelpView final : public QWidget
{
    Q_OBJECT

public:
    explicit HelpView(QWidget *parent = nullptr);

    bool updateCheckInProgress() const;
    QString updateStatus() const;

public slots:
    void setUpdateCheckInProgress(bool checking,
                                  const QString &status = QString());
    void setUpdateStatus(const QString &status);

signals:
    void checkForUpdatesRequested();
    void checkForBetaUpdatesRequested();

private:
    QPushButton *m_stableUpdateButton = nullptr;
    QPushButton *m_betaUpdateButton = nullptr;
    QLabel *m_updateStatusLabel = nullptr;
    bool m_updateCheckInProgress = false;
};

#endif
