#ifndef ILOGDATASINK_H
#define ILOGDATASINK_H

#include <QByteArray>
#include <QList>
#include <QPair>
#include <QSharedPointer>
#include <QString>
#include <QStringList>
#include <QVariant>

class ILogdataSink
{
public:
    using Ptr = QSharedPointer<ILogdataSink>;

    virtual ~ILogdataSink() = default;

    virtual bool addDataType(const QString &typeName, quint32 typeID,
                             int typeLength, const QString &typeFormat,
                             const QStringList &typeLabels, int timeColumn) = 0;
    virtual bool addDataRow(
        const QString &typeName,
        const QList<QPair<QString, QVariant>> &values) = 0;
    virtual void addUnitData(quint8 unitID, const QString &unitName) = 0;
    virtual void addMultiplierData(quint8 multiID, double multiplier) = 0;
    virtual void addMsgToUnitAndMultiplierData(
        quint32 typeID, const QByteArray &multiplierFieldInfo,
        const QByteArray &unitFieldInfo) = 0;
    virtual void setTimeStamp(const QString &timeStampName, double divisor) = 0;
    virtual QStringList setupUnitData(const QString &timeStampName,
                                      double divisor) = 0;
    virtual QString getError() const = 0;
};

#endif // ILOGDATASINK_H
