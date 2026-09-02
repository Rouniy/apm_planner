#ifndef CONFIGHWBTVIEW_H
#define CONFIGHWBTVIEW_H

#include "ConfigHWBTViewModel.h"

#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTextEdit;

class ConfigHWBTView final : public QWidget
{
    Q_OBJECT

public:
    explicit ConfigHWBTView(QWidget *parent = nullptr);

    QSize sizeHint() const override;
    ConfigHWBTViewModel *viewModel() const { return m_viewModel; }

    void setPorts(const QStringList &ports,
                  const QString &preferredPort = QString());
    void setMainLinkConnected(bool connected);

public slots:
    void operationProgress(quint64 generation, const QString &line);
    void operationFinished(quint64 generation, bool success,
                           bool canceled);

signals:
    void programRequested(quint64 generation, const QString &portName,
                          const QString &name,
                          const QString &selectedBaud,
                          const QString &pin);
    void cancelRequested(quint64 generation);
    void refreshPortsRequested();

protected:
    void hideEvent(QHideEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    void syncPorts();
    void syncSettings();
    void syncOutput();
    void syncState();

    ConfigHWBTViewModel *m_viewModel = nullptr;
    QComboBox *m_portCombo = nullptr;
    QLineEdit *m_nameEdit = nullptr;
    QComboBox *m_baudCombo = nullptr;
    QLineEdit *m_pinEdit = nullptr;
    QPushButton *m_writeButton = nullptr;
    QTextEdit *m_outputEdit = nullptr;
};

#endif
