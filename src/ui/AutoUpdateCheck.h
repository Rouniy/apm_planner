/*===================================================================
APM_PLANNER Open Source Ground Control Station

This file is part of the APM_PLANNER project and is distributed under
the terms of the GNU General Public License version 3 or later.
======================================================================*/

#pragma once

#include <QByteArray>
#include <QObject>
#include <QScopedPointer>
#include <QString>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

class AutoUpdateCheck : public QObject
{
    Q_OBJECT

public:
    enum ReleaseChannel {
        Stable,
        Beta
    };
    Q_ENUM(ReleaseChannel)

    enum Presentation {
        Background,
        Modal,
        Inline
    };
    Q_ENUM(Presentation)

    struct UpdateSelection
    {
        enum Status {
            Available,
            NoUpdate,
            InvalidManifest
        } status = NoUpdate;

        QString version;
        QString releaseType;
        QString url;
        QString name;
        QString error;
    };

    explicit AutoUpdateCheck(QObject *parent = nullptr);
    ~AutoUpdateCheck() override;

    bool checkForUpdates(ReleaseChannel channel,
                         Presentation presentation,
                         const QUrl &manifestUrl = QUrl());
    bool isChecking() const;

    ReleaseChannel configuredReleaseChannel() const;
    void setConfiguredReleaseChannel(ReleaseChannel channel);
    QString skippedVersion(ReleaseChannel channel) const;
    void setSkippedVersion(ReleaseChannel channel, const QString &version);

    static QString releaseChannelName(ReleaseChannel channel);
    static ReleaseChannel releaseChannelFromString(const QString &value);
    static bool isVersionNewer(const QString &candidate,
                               const QString &current);
    static UpdateSelection selectUpdate(const QByteArray &manifest,
                                        const QString &platform,
                                        ReleaseChannel channel,
                                        const QString &currentVersion,
                                        const QString &skippedVersion = QString());

signals:
    // Compatibility signals used by the existing modal update flow.
    void updateAvailable(QString version, QString releaseType,
                         QString url, QString name);
    void noUpdateAvailable();

    void checkingChanged(bool checking);
    void checkStarted(AutoUpdateCheck::ReleaseChannel channel,
                      AutoUpdateCheck::Presentation presentation);
    void checkNoUpdate(AutoUpdateCheck::ReleaseChannel channel,
                       AutoUpdateCheck::Presentation presentation);
    void checkFailed(AutoUpdateCheck::ReleaseChannel channel,
                     AutoUpdateCheck::Presentation presentation,
                     const QString &reason);
    void checkAvailable(AutoUpdateCheck::ReleaseChannel channel,
                        AutoUpdateCheck::Presentation presentation,
                        const QString &version,
                        const QString &releaseType,
                        const QString &url,
                        const QString &name);

public slots:
    // Existing entry points remain as wrappers for menu/startup callers.
    void forcedAutoUpdateCheck();
    void autoUpdateCheck();
    void autoUpdateCheck(const QString &url);
    void cancelDownload();
    void httpFinished();
    void httpReadyRead();
    void updateDataReadProgress(qint64 bytesRead, qint64 totalBytes);

    void setSkipVersion(const QString &version);
    void setAutoUpdateEnabled(bool enabled);
    bool isUpdateEnabled();
    void suppressNoUpdateSignal();

private:
    void loadSettings();
    void writeSettings();
    void startRequest(const QUrl &url);
    void finishAvailable(const UpdateSelection &selection);
    void finishNoUpdate();
    void finishFailure(const QString &reason);
    void finishChecking();

    QNetworkAccessManager *m_networkAccessManager = nullptr;
    QTimer *m_timeoutTimer = nullptr;
    QScopedPointer<QNetworkReply, QScopedPointerDeleteLater> m_networkReplyPtr;
    QString m_stableSkipVersion;
    QString m_betaSkipVersion;
    ReleaseChannel m_releaseChannel = Stable;
    ReleaseChannel m_activeChannel = Stable;
    Presentation m_activePresentation = Background;
    bool m_isAutoUpdateEnabled = false;
    bool m_checking = false;
    bool m_suppressNextNoUpdateSignal = false;
    bool m_activeSuppressNoUpdateSignal = false;
    int m_redirectCount = 0;
};

Q_DECLARE_METATYPE(AutoUpdateCheck::ReleaseChannel)
Q_DECLARE_METATYPE(AutoUpdateCheck::Presentation)
