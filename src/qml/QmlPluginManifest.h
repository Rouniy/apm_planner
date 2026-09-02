#ifndef QMLPLUGINMANIFEST_H
#define QMLPLUGINMANIFEST_H

#include <QByteArray>
#include <QString>

class QmlPluginManifest
{
public:
    static constexpr int SupportedManifestVersion = 1;
    static constexpr int SupportedApiMajor = 1;

    enum class Error
    {
        None,
        CannotOpen,
        InvalidJson,
        RootNotObject,
        MissingField,
        WrongType,
        UnknownField,
        UnsupportedManifestVersion,
        InvalidId,
        InvalidVersion,
        UnsupportedApiMajor,
        InvalidUiType,
        InvalidPath,
        PathEscapesPlugin
    };

    struct UiContribution
    {
        QString type;
        QString title;
        QString icon;
        QString iconFilePath;
    };

    static QmlPluginManifest load(const QString &manifestFilePath);
    static QmlPluginManifest parse(const QByteArray &json,
                                   const QString &pluginDirectory);

    bool isValid() const;
    Error error() const;
    QString errorString() const;

    int manifestVersion() const;
    QString id() const;
    QString name() const;
    QString version() const;
    int apiMajor() const;
    QString main() const;
    QString mainFilePath() const;
    UiContribution ui() const;
    QString description() const;
    QString author() const;
    QString pluginDirectory() const;
    QString manifestFilePath() const;

private:
    static QmlPluginManifest failure(Error error, const QString &message,
                                     const QString &pluginDirectory = QString(),
                                     const QString &manifestFilePath = QString());

    Error m_error = Error::InvalidJson;
    QString m_errorString;
    int m_manifestVersion = 0;
    QString m_id;
    QString m_name;
    QString m_version;
    int m_apiMajor = 0;
    QString m_main;
    QString m_mainFilePath;
    UiContribution m_ui;
    QString m_description;
    QString m_author;
    QString m_pluginDirectory;
    QString m_manifestFilePath;
};

#endif // QMLPLUGINMANIFEST_H
