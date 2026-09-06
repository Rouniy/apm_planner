#ifndef DATAFLASHLOGSWIDGET_H
#define DATAFLASHLOGSWIDGET_H

#include <QWidget>

class QLabel;
class QPushButton;

class DataFlashLogsWidget final : public QWidget
{
    Q_OBJECT

public:
    explicit DataFlashLogsWidget(QWidget *parent = nullptr);

    void setOperationBusy(bool busy);
    void setStatusText(const QString &text);
    QString statusText() const;

signals:
    void downloadRequested();
    void reviewRequested();
    void autoAnalysisRequested();
    void kmlGpxRequested();
    void binToLogRequested();
    void organizeRequested();

private:
    QPushButton *m_download = nullptr;
    QPushButton *m_review = nullptr;
    QPushButton *m_analysis = nullptr;
    QPushButton *m_kmlGpx = nullptr;
    QPushButton *m_binToLog = nullptr;
    QPushButton *m_matlab = nullptr;
    QPushButton *m_geoReference = nullptr;
    QPushButton *m_organize = nullptr;
    QLabel *m_status = nullptr;
};

#endif // DATAFLASHLOGSWIDGET_H
