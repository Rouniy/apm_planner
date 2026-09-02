#ifndef CONFIGGPSINJECTVIEW_H
#define CONFIGGPSINJECTVIEW_H

#include "ConfigGpsInjectViewModel.h"

#include <QList>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QFrame;
class QLabel;
class QLineEdit;
class QPaintEvent;
class QPushButton;
class QSpinBox;
class QTableWidget;

class ConfigGpsInjectView final : public QWidget
{
    Q_OBJECT

public:
    explicit ConfigGpsInjectView(QWidget *parent = nullptr);
    ConfigGpsInjectView(GpsCorrectionSource *source, QSettings *settings,
                        QWidget *parent = nullptr);
    ~ConfigGpsInjectView() override = default;

    QSize sizeHint() const override;
    ConfigGpsInjectViewModel *viewModel() const { return m_viewModel; }

protected:
    void paintEvent(QPaintEvent *event) override;

private slots:
    void syncFromModel();

private:
    void rebuildBasePositions();
    void setFreshIndicator(QLabel *indicator, bool fresh);

    ConfigGpsInjectViewModel *m_viewModel = nullptr;

    QLabel *m_statusLabel = nullptr;
    QComboBox *m_sourceCombo = nullptr;
    QComboBox *m_baudCombo = nullptr;
    QPushButton *m_connectButton = nullptr;
    QPushButton *m_refreshPortsButton = nullptr;

    QFrame *m_ntripPanel = nullptr;
    QLineEdit *m_hostEdit = nullptr;
    QSpinBox *m_casterPortSpin = nullptr;
    QLineEdit *m_mountEdit = nullptr;
    QLineEdit *m_usernameEdit = nullptr;
    QLineEdit *m_passwordEdit = nullptr;

    QCheckBox *m_sendGgaCheck = nullptr;
    QCheckBox *m_ntripV1Check = nullptr;
    QCheckBox *m_autoConfigCheck = nullptr;
    QComboBox *m_receiverTypeCombo = nullptr;

    QLabel *m_inputRateLabel = nullptr;
    QLabel *m_outputRateLabel = nullptr;
    QLabel *m_injectedLabel = nullptr;
    QLabel *m_messagesSeenLabel = nullptr;

    QLabel *m_baseIndicator = nullptr;
    QLabel *m_gpsIndicator = nullptr;
    QLabel *m_glonassIndicator = nullptr;
    QLabel *m_beidouIndicator = nullptr;
    QLabel *m_galileoIndicator = nullptr;
    QLabel *m_rtcmBasePositionLabel = nullptr;

    QFrame *m_autoConfigPanel = nullptr;
    QCheckBox *m_m8p130PlusCheck = nullptr;
    QLineEdit *m_surveyAccuracyEdit = nullptr;
    QLineEdit *m_surveyTimeEdit = nullptr;
    QLabel *m_surveyStatusLabel = nullptr;
    QPushButton *m_restartSurveyButton = nullptr;
    QPushButton *m_savePositionButton = nullptr;

    QFrame *m_septentrioPanel = nullptr;
    QCheckBox *m_septentrioFixedCheck = nullptr;
    QWidget *m_septentrioPositionWidget = nullptr;
    QLineEdit *m_septentrioLatitudeEdit = nullptr;
    QLineEdit *m_septentrioLongitudeEdit = nullptr;
    QLineEdit *m_septentrioAltitudeEdit = nullptr;
    QPushButton *m_setSeptentrioPositionButton = nullptr;
    QComboBox *m_septentrioRtcmLevelCombo = nullptr;
    QLineEdit *m_septentrioIntervalEdit = nullptr;
    QPushButton *m_setSeptentrioIntervalButton = nullptr;
    QCheckBox *m_septentrioGpsCheck = nullptr;
    QCheckBox *m_septentrioGlonassCheck = nullptr;
    QCheckBox *m_septentrioGalileoCheck = nullptr;
    QCheckBox *m_septentrioBeidouCheck = nullptr;

    QTableWidget *m_basePositionsTable = nullptr;
    QList<BasePosRow> m_renderedBasePositions;
};

#endif // CONFIGGPSINJECTVIEW_H
