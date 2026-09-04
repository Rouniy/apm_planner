#ifndef CONFIGCOMPASSVIEW_H
#define CONFIGCOMPASSVIEW_H

#include "ConfigCompassViewModel.h"

#include <QHash>
#include <QWidget>

class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QTableView;
class QVBoxLayout;

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
  void updateMoveButtons();
  void confirmAndRequestReboot();
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
};

#endif // CONFIGCOMPASSVIEW_H
