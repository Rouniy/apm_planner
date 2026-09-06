#ifndef CONFIGCOMPASSVIEW_H
#define CONFIGCOMPASSVIEW_H

#include "ConfigCompassViewModel.h"
#include "comm/VehicleEndpoint.h"

#include <QHash>
#include <QMetaObject>
#include <QPointer>
#include <QWidget>

class QCheckBox;
class QAction;
class QDoubleSpinBox;
class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QTableView;
class QVBoxLayout;
class CompassCalibrationService;

class ConfigCompassView final : public QWidget {
  Q_OBJECT

public:
  explicit ConfigCompassView(QWidget *parent = nullptr);
  ~ConfigCompassView() override;

  QSize sizeHint() const override;
  ConfigCompassViewModel *viewModel() const { return m_viewModel; }

  void setCatalog(const ParameterMetaDataCatalog &catalog,
                  bool enforceRanges = false);
  void
  setParameterSnapshot(const QList<ConfigFriendlyParameterValue> &parameters,
                       int preferredComponent, bool completeSnapshot);
  void setConnected(bool connected);
  void setArmed(bool armed);
  void setOfflineMagFitAction(QAction *action);
  void setCalibrationContext(CompassCalibrationService *service,
                             const VehicleTargetLease &target);

public slots:
  void parameterChanged(int componentId, const QString &name,
                        const QVariant &value);
  void parameterWriteSubmitted(quint64 requestId, qulonglong batchId);
  void parameterWriteSubmissionFailed(quint64 requestId, const QString &reason);
  void parameterWriteFailed(qulonglong batchId, int componentId,
                            const QString &name, const QString &reason);
  void parameterWriteCancelled(qulonglong batchId, int componentId,
                               const QString &name);
  void parameterBatchCompleted(qulonglong batchId, int succeeded, int failed);
  void refreshFailed(const QString &reason);
  void refreshCanceled();
  void rebootSubmitted(quint64 requestId);
  void rebootSubmissionFailed(quint64 requestId, const QString &reason);
  void rebootAcknowledged(quint64 requestId, bool accepted,
                          const QString &reason = QString());
  void rebootCancelled(quint64 requestId, const QString &reason = QString());

signals:
  void writeRequested(quint64 requestId, int componentId, QVariantList changes);
  void refreshRequested(int componentId);
  void rebootRequested(quint64 requestId);

private:
  void buildUi();
  void rebuildFields();
  void syncFieldValues();
  void syncFlags();
  void syncState();
  void syncCalibrationState();
  void updateMoveButtons();
  bool confirmAndRequestReboot(bool calibrationTriggered = false);
  void startCalibration();
  void acceptCalibration();
  void cancelCalibration();
  void startFixedYawCalibration();
  void showCalibrationRequestResult(const QString &action, int result);
  bool calibrationBaseReady() const;
  bool calibrationTargetMatchesService() const;
  static QString calibrationStateText(int state);
  static bool valuesEqual(const QVariant &left, const QVariant &right);

  ConfigCompassViewModel *m_viewModel = nullptr;
  QTableView *m_table = nullptr;
  QPushButton *m_moveUp = nullptr;
  QPushButton *m_moveDown = nullptr;
  QCheckBox *m_use[3] = {nullptr, nullptr, nullptr};
  QCheckBox *m_learn = nullptr;
  QPushButton *m_removeMissing = nullptr;
  QPushButton *m_reboot = nullptr;
  QLabel *m_compassStatus = nullptr;
  QVBoxLayout *m_fieldLayout = nullptr;
  QHash<QString, QWidget *> m_fieldEditors;
  QHash<QString, QLabel *> m_fieldStatuses;
  QDoubleSpinBox *m_declination = nullptr;
  QPushButton *m_writeDeclination = nullptr;
  QPushButton *m_quickPixhawk = nullptr;
  QPushButton *m_refresh = nullptr;
  QLabel *m_status = nullptr;
  QPushButton *m_calStart = nullptr;
  QPushButton *m_calAccept = nullptr;
  QPushButton *m_calCancel = nullptr;
  QPushButton *m_calFromLog = nullptr;
  QPointer<QAction> m_offlineMagFitAction;
  QPushButton *m_largeVehicleCal = nullptr;
  QProgressBar *m_calProgress[3] = {nullptr, nullptr, nullptr};
  QLabel *m_calibrationTargetStatus = nullptr;
  QPlainTextEdit *m_calibrationResult = nullptr;
  QPointer<CompassCalibrationService> m_calibrationService;
  VehicleTargetLease m_calibrationTarget;
  QMetaObject::Connection m_calibrationChangedConnection;
  QMetaObject::Connection m_calibrationDestroyedConnection;
  QString m_calibrationRequestOutcome;
  quint64 m_calibrationRebootRequestId = 0;
  bool m_recordCalibrationRebootRequest = false;
};

#endif // CONFIGCOMPASSVIEW_H
