#ifndef SIMPLEACTIONSWIDGET_H
#define SIMPLEACTIONSWIDGET_H

#include <QList>
#include <QString>
#include <QWidget>

class QLabel;
class QPushButton;

class SimpleActionsWidget final : public QWidget
{
    Q_OBJECT

public:
    explicit SimpleActionsWidget(QWidget *parent = nullptr);

    bool actionsAvailable() const;

public slots:
    void setActionsAvailable(bool available);
    void setActionStatus(const QString &message, bool error = false);

signals:
    void quickModeRequested(const QString &mode);

private:
    QList<QPushButton *> m_buttons;
    QLabel *m_status = nullptr;
    bool m_actionsAvailable = false;
};

#endif // SIMPLEACTIONSWIDGET_H
