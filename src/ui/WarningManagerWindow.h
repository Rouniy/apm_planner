#ifndef WARNINGMANAGERWINDOW_H
#define WARNINGMANAGERWINDOW_H

#include "services/WarningEngine.h"

#include <QHash>
#include <QPointer>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include <functional>

class QLabel;
class QComboBox;
class QLineEdit;
class QPushButton;
class QVBoxLayout;

/** Modeless editor for the application-owned custom-warning engine. */
class WarningManagerWindow final : public QWidget
{
    Q_OBJECT

public:
    explicit WarningManagerWindow(WarningEngine *engine,
                                  const QStringList &fields,
                                  QWidget *parent = nullptr);
    ~WarningManagerWindow() override;

    WarningEngine *engine() const { return m_engine.data(); }
    void setExternalStatus(const QString &status);
    void setTelemetryValues(const QHash<QString, double> &values);

private:
    void rebuildRows();
    void scheduleRebuild();
    void updateStatus();
    void refreshTelemetryRows();
    void applyRuleChange(quint64 id,
                         const std::function<void(CustomWarning &)> &change);
    void applyStructuralChange(
        const std::function<bool(QVector<CustomWarning> *, QString *)> &change);
    void addRootRule();
    void addChildRule(quint64 id);
    void removeRule(quint64 id);
    void saveRules();

    QPointer<WarningEngine> m_engine;
    QStringList m_fields;
    QString m_externalStatus;
    QString m_operationStatus;
    QHash<QString, double> m_telemetryValues;
    QWidget *m_rulesWidget = nullptr;
    QVBoxLayout *m_rulesLayout = nullptr;
    QPushButton *m_addButton = nullptr;
    QPushButton *m_saveButton = nullptr;
    QLabel *m_status = nullptr;
    QHash<quint64, QLabel *> m_valueLabels;
    QHash<quint64, QComboBox *> m_sourceEditors;
    QHash<quint64, QLineEdit *> m_thresholdEditors;
    QVector<quint64> m_displayedIds;
    bool m_localMutation = false;
    bool m_rebuildPending = false;
};

#endif // WARNINGMANAGERWINDOW_H
