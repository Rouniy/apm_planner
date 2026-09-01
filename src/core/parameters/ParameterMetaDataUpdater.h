#ifndef PARAMETERMETADATAUPDATER_H
#define PARAMETERMETADATAUPDATER_H

#include "core/parameters/ParameterMetaDataRepository.h"

#include <QMap>
#include <QDateTime>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QUrl>

class QNetworkReply;

class ParameterMetaDataUpdater final : public QObject
{
    Q_OBJECT

public:
    explicit ParameterMetaDataUpdater(
        ParameterMetaDataRepository *repository,
        QObject *parent = nullptr);
    ParameterMetaDataUpdater(
        ParameterMetaDataRepository *repository,
        const QUrl &loopbackEndpointOverride,
        QObject *parent = nullptr);
    ~ParameterMetaDataUpdater() override;

    void requestUpdate(ParameterFirmwareFamily family,
                       const QString &firmwareVersion,
                       bool developmentFirmware);
    void cancel();
    static QList<QUrl> candidateUrls(ParameterFirmwareFamily family,
                                     const QString &firmwareVersion,
                                     bool developmentFirmware);

signals:
    void catalogUpdated(ParameterFirmwareFamily family,
                        const QString &firmwareVersion);
    void updateFailed(ParameterFirmwareFamily family,
                      const QString &reason);

private:
    struct Candidate
    {
        QUrl requestUrl;
        QUrl sourceUrl;
        QString cacheVersion;
    };

    struct UpdateState
    {
        QString requestKey;
        QList<Candidate> candidates;
        int candidateIndex = -1;
        QPointer<QNetworkReply> reply;
        QStringList errors;
        QByteArray payload;
        bool payloadTooLarge = false;
    };

    void startNext(ParameterFirmwareFamily family);
    void requestFinished(ParameterFirmwareFamily family,
                         QNetworkReply *reply);
    QUrl requestUrl(const QUrl &sourceUrl) const;

    ParameterMetaDataRepository *m_repository = nullptr;
    QNetworkAccessManager m_networkAccessManager;
    QMap<ParameterFirmwareFamily, UpdateState> m_updates;
    QMap<QString, QDateTime> m_failedExactAttempts;
    QUrl m_loopbackEndpointOverride;
};

#endif
