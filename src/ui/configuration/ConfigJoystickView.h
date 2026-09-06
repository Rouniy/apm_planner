#ifndef CONFIGJOYSTICKVIEW_H
#define CONFIGJOYSTICKVIEW_H

#include "input/JoystickConfiguration.h"
#include "input/JoystickDevice.h"
#include "services/JoystickControlService.h"

#include <QPointer>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QDialog;
class QFileDialog;
class QLabel;
class QMessageBox;
class QPushButton;
class QTableWidget;
class QTimer;

class ConfigJoystickView final : public QWidget
{
    Q_OBJECT

public:
    explicit ConfigJoystickView(QWidget *parent = nullptr);
    ConfigJoystickView(JoystickDevice *device,
                       JoystickControlService *service,
                       QWidget *parent = nullptr);
    ~ConfigJoystickView() override;

    JoystickDevice *device() const noexcept { return m_device; }
    JoystickControlService *service() const noexcept { return m_service; }
    JoystickConfiguration::Profile profile() const { return m_profile; }
    bool detectionActive() const noexcept { return m_detectionKind != DetectionNone; }

private:
    enum DetectionKind { DetectionNone, DetectionAxis, DetectionButton };

    void buildUi();
    void connectSources();
    void loadInitialProfile();
    void populateDevices();
    void populateProfile();
    void populateAxisRows();
    void populateButtonRows();
    void refreshControls();
    void refreshStatus();
    void updatePreview(const JoystickControlService::Preview &preview);
    void updateSnapshot(const JoystickDevice::Snapshot &snapshot);
    void selectDevice(int comboIndex);
    void updateChannelFromRow(int row);
    void updateButtonFromRow(int row);
    bool applyProfileToService(QString *error = nullptr);
    void toggleEnable();
    void showEnableConsent(const JoystickControlService::EnablePlan &plan);
    void dismissEnableConsent();
    void saveConfiguration();
    void beginImport();
    void showImportPicker();
    void beginExport();
    void showButtonSettings(int row);
    void beginAxisDetection(int row);
    void beginButtonDetection(int row);
    void cancelDetection(const QString &status = QString());
    void processDetection(const JoystickDevice::Snapshot &snapshot);
    void toggleRangeCalibration();
    void resetRangeCalibration();
    void setStatus(const QString &status);
    QString rawSnapshotText(const JoystickDevice::Snapshot &snapshot) const;
    QString axisNameForIndex(int index) const;
    int visibleButtonRows() const;
    void dismissFileDialog(QPointer<QFileDialog> &dialog);

    QPointer<JoystickDevice> m_device;
    QPointer<JoystickControlService> m_service;
    JoystickConfiguration::Profile m_profile;
    JoystickControlService::EnablePlan m_enablePlan;
    QPointer<QMessageBox> m_enableConsent;
    QPointer<QMessageBox> m_importConsent;
    QPointer<QFileDialog> m_fileDialog;
    QPointer<QDialog> m_buttonSettings;
    quint64 m_promptRevision = 0;
    bool m_syncing = false;
    bool m_submittingProfile = false;
    bool m_closing = false;

    DetectionKind m_detectionKind = DetectionNone;
    int m_detectionRow = -1;
    JoystickDevice::Snapshot m_detectionBaseline;
    QTimer *m_detectionTimer = nullptr;

    QLabel *m_status = nullptr;
    QComboBox *m_deviceCombo = nullptr;
    QPushButton *m_refresh = nullptr;
    QCheckBox *m_elevons = nullptr;
    QCheckBox *m_manualControl = nullptr;
    QPushButton *m_calibrateRange = nullptr;
    QPushButton *m_resetRange = nullptr;
    QLabel *m_rawInput = nullptr;
    QTableWidget *m_axes = nullptr;
    QTableWidget *m_buttons = nullptr;
    QPushButton *m_enable = nullptr;
    QPushButton *m_save = nullptr;
    QPushButton *m_export = nullptr;
    QPushButton *m_import = nullptr;
    QLabel *m_loadedConfig = nullptr;
};

#endif // CONFIGJOYSTICKVIEW_H
