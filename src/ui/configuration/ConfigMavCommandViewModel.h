#ifndef CONFIGMAVCOMMANDVIEWMODEL_H
#define CONFIGMAVCOMMANDVIEWMODEL_H

#include "ui/flightplanner/MissionCommandCatalog.h"

#include <QAbstractTableModel>
#include <QVector>

class ConfigMavCommandViewModel final : public QAbstractTableModel
{
    Q_OBJECT
    Q_PROPERTY(QString Status READ Status NOTIFY statusChanged)

public:
    enum Column
    {
        IdColumn,
        NameColumn,
        P1Column,
        P2Column,
        P3Column,
        P4Column,
        P5Column,
        P6Column,
        P7Column,
        ColumnCount
    };
    Q_ENUM(Column)

    explicit ConfigMavCommandViewModel(QObject *parent = nullptr);
    ConfigMavCommandViewModel(MissionCommandCatalog *catalog,
                              QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index,
                  int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex &index, const QVariant &value,
                 int role = Qt::EditRole) override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

    QString Status() const;
    QVector<MissionCommandDefinition> Commands() const;

public slots:
    int AddCommand(int id, const QString &name = QString());
    bool RemoveCommand(int row);
    bool SaveCommand();
    void ReloadCommand();

signals:
    void statusChanged(const QString &status);

private:
    void setStatus(const QString &status);

    MissionCommandCatalog *m_catalog = nullptr;
    QVector<MissionCommandDefinition> m_commands;
    QString m_status;
};

#endif // CONFIGMAVCOMMANDVIEWMODEL_H
