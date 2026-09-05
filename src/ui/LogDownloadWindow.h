#ifndef LOGDOWNLOADWINDOW_H
#define LOGDOWNLOADWINDOW_H

#include <QPointer>
#include <QWidget>

class QCheckBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QTableWidget;
class LogDownloadViewModel;

/** Modeless MP10-style MAVLink DataFlash log download host. */
class LogDownloadWindow final : public QWidget
{
    Q_OBJECT

public:
    explicit LogDownloadWindow(LogDownloadViewModel *viewModel,
                               QWidget *owner = nullptr);
    ~LogDownloadWindow() override;

    LogDownloadViewModel *viewModel() const { return m_viewModel; }

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void rebuildRows();
    void syncState();
    void chooseSelectedDestination();
    void chooseAllDestination();
    void confirmErase();

    QPointer<LogDownloadViewModel> m_viewModel;
    QPushButton *m_refreshButton = nullptr;
    QPushButton *m_downloadSelectedButton = nullptr;
    QPushButton *m_downloadAllButton = nullptr;
    QCheckBox *m_createKmlCheckBox = nullptr;
    QPushButton *m_eraseAllButton = nullptr;
    QPushButton *m_cancelButton = nullptr;
    QTableWidget *m_table = nullptr;
    QProgressBar *m_progress = nullptr;
    QLabel *m_status = nullptr;
};

#endif // LOGDOWNLOADWINDOW_H
