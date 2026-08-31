#ifndef COMPASSONBOARDCALIBRATIONDIALOG_H
#define COMPASSONBOARDCALIBRATIONDIALOG_H

#include <mavlink_types.h>
extern mavlink_status_t m_mavlink_status[MAVLINK_COMM_NUM_BUFFERS];  // defined in src/main.cc
#include <mavlink.h>

#include <QDialog>
#include <QVector3D>
#ifdef APM_HAS_QT_DATA_VISUALIZATION
#include <QtDataVisualization/Q3DScatter>
#include <QtDataVisualization/QScatter3DSeries>
#include <QtDataVisualization/QScatterDataProxy>

using namespace QtDataVisualization;
#endif

class QCustomPlot;
class UASInterface;
class CompassCalibrationPlot;

namespace Ui {
class CompassOnboardCalibrationDialog;
}

class CompassOnboardCalibrationDialog : public QDialog
{
    Q_OBJECT

public:
    explicit CompassOnboardCalibrationDialog(QWidget *parent = 0);
    ~CompassOnboardCalibrationDialog();

    void closeEvent(QCloseEvent *);

public slots:
    void activeUASSet(UASInterface* uas);
    void compassCalibrationProgress(UASInterface* uas, mavlink_mag_cal_progress_t &mag_cal_progress);
    void compassCalibrationReport(UASInterface* uas, mavlink_mag_cal_report_t &mag_cal_report);
    void textMessageReceived(int uasid, int componentid, int severity, QString text);

    void startCalibration();
    void stopCalibration();
    void okButtonClicked();

    void cancelCalibration();

private:
    void addDataVisualization();
    void updateStatusMessage();

private:
    Ui::CompassOnboardCalibrationDialog *ui;

    UASInterface* m_uasInterface = nullptr;
    bool m_compassCalibrationComplete[2] = {false};

    QMap<int, mavlink_mag_cal_progress_t> m_compassCalibrationProgress;

#ifdef APM_HAS_QT_DATA_VISUALIZATION
    QScatterDataArray* m_pointDataArray = nullptr;
    QScatterDataProxy* m_proxy = nullptr;
    QScatter3DSeries* m_series = nullptr;
#else
    CompassCalibrationPlot* m_plot = nullptr;
#endif
};

#endif // COMPASSONBOARDCALIBRATIONDIALOG_H
