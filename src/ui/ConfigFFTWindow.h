#ifndef CONFIGFFTWINDOW_H
#define CONFIGFFTWINDOW_H

#include <QPointer>
#include <QWidget>

class ConfigFFTView;
class ConfigFFTViewModel;

class ConfigFFTWindow final : public QWidget
{
    Q_OBJECT

public:
    explicit ConfigFFTWindow(ConfigFFTViewModel *viewModel = nullptr,
                             QWidget *owner = nullptr);
    ~ConfigFFTWindow() override;

    ConfigFFTView *view() const;
    ConfigFFTViewModel *viewModel() const;

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    QPointer<ConfigFFTView> m_view;
};

#endif // CONFIGFFTWINDOW_H
