#ifndef QGCTABBEDINFOVIEW_H
#define QGCTABBEDINFOVIEW_H

#include "ui_QGCTabbedInfoView.h"
#include "MAVLinkDecoder.h"
#include "QGCMessageView.h"
#include "UASActionsWidget.h"
#include "UASQuickView.h"
#include "UASRawStatusView.h"

#include <QPointer>
#include <QWidget>

class FlightDataViewModel;
class PreflightChecklistModel;
class PreflightChecklistWidget;
class SimpleActionsWidget;

class QGCTabbedInfoView : public QWidget
{
    Q_OBJECT
    
public:
    explicit QGCTabbedInfoView(QWidget *parent = 0);
    ~QGCTabbedInfoView();
    void addSource(MAVLinkDecoder *decoder);
    void setFlightDataViewModel(FlightDataViewModel *viewModel);
    void installQuickView(QWidget *view);
    void installDataFlashLogs(QWidget *view);
    PreflightChecklistModel *preflightChecklistModel() const;
    SimpleActionsWidget *simpleActionsWidget() const;

signals:
    void clearTrackRequested();
    void joystickSetupRequested();

private:
    void syncPreflightTelemetry();

    MAVLinkDecoder *m_decoder = nullptr;
    Ui::QGCTabbedInfoView ui;
    QGCMessageView *messageView;
    UASActionsWidget *actionsWidget;
    UASQuickView *quickView;
    UASRawStatusView *rawView;
    PreflightChecklistModel *m_preflightModel = nullptr;
    PreflightChecklistWidget *m_preflightWidget = nullptr;
    SimpleActionsWidget *m_simpleActionsWidget = nullptr;
    QPointer<FlightDataViewModel> m_flightDataViewModel;
};

#endif // QGCTABBEDINFOVIEW_H
