#ifndef CONFIGUSERDEFINEDVIEWMODEL_H
#define CONFIGUSERDEFINEDVIEWMODEL_H

#include <QObject>
#include <QStringList>

class ConfigUserDefinedViewModel final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString OptionsText READ OptionsText NOTIFY optionsChanged)

public:
    explicit ConfigUserDefinedViewModel(QObject *parent = nullptr);

    static QStringList DefaultOptions();
    static QStringList ParseOptions(const QString &raw);

    QStringList Options() const;
    QString OptionsText() const;
    void ApplyOptions(const QString &raw);

signals:
    void optionsChanged();

private:
    QStringList m_options;
};

#endif
