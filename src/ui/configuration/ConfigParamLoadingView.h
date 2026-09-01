#ifndef CONFIGPARAMLOADINGVIEW_H
#define CONFIGPARAMLOADINGVIEW_H

#include <QWidget>

class ConfigParamLoadingViewModel;
class QLabel;
class QProgressBar;

class ConfigParamLoadingView final : public QWidget
{
    Q_OBJECT

public:
    explicit ConfigParamLoadingView(QWidget *parent = nullptr);

    ConfigParamLoadingViewModel *viewModel() const;

signals:
    void stopLoadingRequested();
    void retryLoadingRequested();

private slots:
    void refresh();

private:
    ConfigParamLoadingViewModel *m_viewModel = nullptr;
    QLabel *m_statusLabel = nullptr;
    QProgressBar *m_progress = nullptr;
    QLabel *m_countLabel = nullptr;
};

#endif
