#ifndef CONFIGPLANNERADVVIEWMODEL_H
#define CONFIGPLANNERADVVIEWMODEL_H

#include <QList>
#include <QObject>
#include <QString>
#include <QVariant>

struct PlannerAdvancedSettingRow
{
    QString Name;
    QString Value;
};

class ConfigPlannerAdvViewModel final : public QObject
{
    Q_OBJECT

public:
    explicit ConfigPlannerAdvViewModel(QObject *parent = nullptr);

    QList<PlannerAdvancedSettingRow> Params() const;
    static QString DisplayValue(const QVariant &value);

public slots:
    void Activate();

signals:
    void paramsChanged();

private:
    QList<PlannerAdvancedSettingRow> m_params;
};

#endif
