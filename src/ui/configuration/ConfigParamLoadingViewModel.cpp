#include "ConfigParamLoadingViewModel.h"

#include <QtGlobal>

ConfigParamLoadingViewModel::ConfigParamLoadingViewModel(QObject *parent)
    : QObject(parent)
{
    updateDerivedState();
}

int ConfigParamLoadingViewModel::progressPercent() const
{
    return m_progressPercent;
}

QString ConfigParamLoadingViewModel::status() const
{
    return m_status;
}

QString ConfigParamLoadingViewModel::count() const
{
    return m_count;
}

bool ConfigParamLoadingViewModel::parametersReady() const
{
    return hasAllParameters(m_received, m_reported);
}

int ConfigParamLoadingViewModel::received() const
{
    return m_received;
}

int ConfigParamLoadingViewModel::reported() const
{
    return m_reported;
}

bool ConfigParamLoadingViewModel::loadingCancelled() const
{
    return m_loadingCancelled;
}

QString ConfigParamLoadingViewModel::failure() const
{
    return m_failure;
}

bool ConfigParamLoadingViewModel::hasAllParameters(int received, int reported)
{
    return reported > 0 && received >= reported;
}

void ConfigParamLoadingViewModel::reset()
{
    setState(0, 0);
}

void ConfigParamLoadingViewModel::setState(int received, int reported,
                                           bool loadingCancelled,
                                           const QString &failure)
{
    const int normalizedReceived = qMax(0, received);
    const int normalizedReported = qMax(0, reported);
    QString normalizedFailure = failure.trimmed();
    if (hasAllParameters(normalizedReceived, normalizedReported)) {
        loadingCancelled = false;
        normalizedFailure.clear();
    }

    const bool changed = m_received != normalizedReceived
        || m_reported != normalizedReported
        || m_loadingCancelled != loadingCancelled
        || m_failure != normalizedFailure
        || m_transientState != TransientState::None;
    if (!changed) {
        return;
    }

    m_received = normalizedReceived;
    m_reported = normalizedReported;
    m_loadingCancelled = loadingCancelled;
    m_failure = normalizedFailure;
    m_transientState = TransientState::None;
    updateDerivedState();
    emit stateChanged();
}

void ConfigParamLoadingViewModel::setRequesting()
{
    if (m_transientState == TransientState::Requesting) {
        return;
    }
    m_loadingCancelled = false;
    m_failure.clear();
    m_transientState = TransientState::Requesting;
    updateDerivedState();
    emit stateChanged();
}

void ConfigParamLoadingViewModel::setStopping()
{
    if (m_transientState == TransientState::Stopping) {
        return;
    }
    m_transientState = TransientState::Stopping;
    updateDerivedState();
    emit stateChanged();
}

QString ConfigParamLoadingViewModel::computedStatus() const
{
    if (m_transientState == TransientState::Requesting) {
        return tr("Requesting parameters…");
    }
    if (m_transientState == TransientState::Stopping) {
        return tr("Stopping parameter loading; the connection remains active…");
    }
    if (parametersReady()) {
        return tr("All parameters loaded.");
    }
    if (m_loadingCancelled) {
        return tr("Parameter loading stopped at %1 / %2. The connection remains "
                  "active. Received values are available in Full Parameter List; "
                  "select Retry Now for a complete list.")
            .arg(m_received)
            .arg(m_reported > 0 ? QString::number(m_reported) : tr("unknown"));
    }
    if (!m_failure.isEmpty()) {
        return tr("Parameter loading failed: %1 Received values are available in "
                  "Full Parameter List; select Retry Now for a complete list.")
            .arg(m_failure);
    }
    if (m_reported == 0) {
        return tr("Waiting for the first parameter response. Select another device "
                  "or retry; old-device values remain hidden.");
    }
    return tr("Loading parameters (%1 / %2)…")
        .arg(m_received)
        .arg(m_reported);
}

void ConfigParamLoadingViewModel::updateDerivedState()
{
    m_progressPercent = m_reported > 0
        ? qMin(100, static_cast<int>(m_received * 100.0 / m_reported))
        : 0;
    m_count = QStringLiteral("%1 / %2").arg(m_received).arg(m_reported);
    m_status = computedStatus();
}
