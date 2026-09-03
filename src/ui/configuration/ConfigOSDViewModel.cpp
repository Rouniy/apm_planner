#include "ConfigOSDViewModel.h"

#include <QMap>
#include <QPair>
#include <QPointer>
#include <QRegularExpression>
#include <QTimer>
#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
struct SnapshotValue
{
    QString originalName;
    QString normalizedName;
    QVariant value;
};

using ComponentValues = QMap<QString, SnapshotValue>;

bool lessSnapshotValue(const ConfigFriendlyParameterValue &left,
                       const ConfigFriendlyParameterValue &right)
{
    if (left.componentId != right.componentId) {
        return left.componentId < right.componentId;
    }
    const QString leftNormalized = left.name.trimmed().toUpper();
    const QString rightNormalized = right.name.trimmed().toUpper();
    const int normalizedOrder = QString::compare(
        leftNormalized, rightNormalized, Qt::CaseSensitive);
    if (normalizedOrder != 0) {
        return normalizedOrder < 0;
    }
    return QString::compare(left.name.trimmed(), right.name.trimmed(),
                            Qt::CaseSensitive) < 0;
}

bool lessItem(const ConfigOSDItem &left, const ConfigOSDItem &right)
{
    if (left.screen != right.screen) {
        return left.screen < right.screen;
    }
    const int insensitive = QString::compare(
        left.name, right.name, Qt::CaseInsensitive);
    if (insensitive != 0) {
        return insensitive < 0;
    }
    return QString::compare(left.name, right.name,
                            Qt::CaseSensitive) < 0;
}

bool parameterValueEnabled(const QVariant &value)
{
    bool ok = false;
    const double numeric = value.toDouble(&ok);
    if (ok && std::isfinite(numeric)) {
        return numeric != 0.0;
    }
    return value.toBool();
}
} // namespace

ConfigOSDViewModel::ConfigOSDViewModel(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<ConfigOSDItem>("ConfigOSDItem");
    qRegisterMetaType<QList<ConfigOSDItem>>("QList<ConfigOSDItem>");
}

// Timers use this object as their context and therefore cannot outlive it.
ConfigOSDViewModel::~ConfigOSDViewModel() = default;

QString ConfigOSDViewModel::Title()
{
    return QStringLiteral("Onboard OSD");
}

QString ConfigOSDViewModel::Intro()
{
    return tr("Drag items on the screen preview, or edit X/Y directly. "
              "Tick Enable to show an item. Changes are staged until "
              "Write Changes is selected.");
}

QString ConfigOSDViewModel::NoParametersStatus()
{
    return tr("No OSD parameters found. Connect and Refresh.");
}

QString ConfigOSDViewModel::ScreenStatus(int screen, int itemCount)
{
    return tr("Screen %1: %2 items.").arg(screen).arg(itemCount);
}

QString ConfigOSDViewModel::OfflineStatus()
{
    return tr("offline — connect first.");
}

QString ConfigOSDViewModel::UnreadyStatus()
{
    return tr("parameters unavailable — refresh the parameter list first.");
}

QString ConfigOSDViewModel::NoChangesStatus()
{
    return tr("No OSD changes to write.");
}

QString ConfigOSDViewModel::WritingStatus()
{
    return tr("Writing OSD changes…");
}

QString ConfigOSDViewModel::SuccessStatus()
{
    return tr("✓ OSD changes written.");
}

QString ConfigOSDViewModel::FailureStatus(const QString &detail)
{
    return detail.isEmpty() ? tr("OSD write failed.")
                            : tr("OSD write failed. %1").arg(detail);
}

QString ConfigOSDViewModel::CancelledStatus()
{
    return tr("One or more OSD writes were cancelled; waiting for the batch to finish.");
}

QString ConfigOSDViewModel::TargetChangedStatus()
{
    return tr("The selected target changed before the OSD writes were confirmed.");
}

QString ConfigOSDViewModel::TimeoutStatus()
{
    return tr("The OSD writes are taking longer than expected; editing remains locked until the batch finishes.");
}

QString ConfigOSDViewModel::normalizedName(const QString &name)
{
    return name.trimmed().toUpper();
}

QString ConfigOSDViewModel::normalizedItemName(const QString &name)
{
    return name.trimmed().toUpper();
}

int ConfigOSDViewModel::snapshotPosition(const QVariant &value)
{
    bool ok = false;
    double numeric = value.toDouble(&ok);
    if (!ok || !std::isfinite(numeric)) {
        numeric = 0.0;
    }
    // Preserve accepted HD coordinates outside the phase-1 30x16 preview.
    // Only user edits are clamped to the currently supported grid.
    numeric = std::max(0.0, std::min(
        numeric, static_cast<double>(std::numeric_limits<int>::max())));
    return static_cast<int>(std::lround(numeric));
}

QList<ConfigOSDItem> ConfigOSDViewModel::Items() const
{
    QList<ConfigOSDItem> result;
    for (const ConfigOSDItem &item : m_items) {
        if (item.screen == m_selectedScreen) {
            result.append(item);
        }
    }
    return result;
}

const ConfigOSDItem *ConfigOSDViewModel::itemFor(
    int screen, const QString &name) const
{
    const QString wanted = normalizedItemName(name);
    for (const ConfigOSDItem &item : m_items) {
        if (item.screen == screen
            && normalizedItemName(item.name) == wanted) {
            return &item;
        }
    }
    return nullptr;
}

ConfigOSDItem *ConfigOSDViewModel::itemFor(
    int screen, const QString &name)
{
    const QString wanted = normalizedItemName(name);
    for (ConfigOSDItem &item : m_items) {
        if (item.screen == screen
            && normalizedItemName(item.name) == wanted) {
            return &item;
        }
    }
    return nullptr;
}

bool ConfigOSDViewModel::Item(
    int screen, const QString &name, ConfigOSDItem *item) const
{
    const ConfigOSDItem *found = itemFor(screen, name);
    if (!found) {
        return false;
    }
    if (item) {
        *item = *found;
    }
    return true;
}

bool ConfigOSDViewModel::CanEdit() const
{
    return m_connected && m_snapshotReady && !m_batch.active
        && !m_items.isEmpty();
}

bool ConfigOSDViewModel::CanWrite() const
{
    return CanEdit() && HasDirtyChanges();
}

int ConfigOSDViewModel::DirtyFieldCount() const
{
    int count = 0;
    for (const ConfigOSDItem &item : m_items) {
        count += item.enableDirty() ? 1 : 0;
        count += item.xDirty() ? 1 : 0;
        count += item.yDirty() ? 1 : 0;
    }
    return count;
}

QVariantList ConfigOSDViewModel::DirtyChanges() const
{
    QVariantList changes;
    for (const ConfigOSDItem &item : m_items) {
        if (item.enableDirty()) {
            changes.append(QVariantMap{
                {QStringLiteral("name"), item.enableParameter},
                {QStringLiteral("value"), item.enabled ? 1 : 0}});
        }
        if (item.xDirty()) {
            changes.append(QVariantMap{
                {QStringLiteral("name"), item.xParameter},
                {QStringLiteral("value"), item.x}});
        }
        if (item.yDirty()) {
            changes.append(QVariantMap{
                {QStringLiteral("name"), item.yParameter},
                {QStringLiteral("value"), item.y}});
        }
    }
    return changes;
}

void ConfigOSDViewModel::setConnected(bool connected)
{
    if (m_connected == connected) {
        return;
    }
    m_connected = connected;
    if (!connected) {
        invalidateSnapshot(OfflineStatus());
        return;
    }
    const QPointer<ConfigOSDViewModel> guard(this);
    if (m_snapshotReady) {
        updateScreenStatus();
    } else {
        setStatus(UnreadyStatus());
    }
    if (!guard) {
        return;
    }
    emit stateChanged();
}

void ConfigOSDViewModel::setParameterSnapshot(
    const QList<ConfigFriendlyParameterValue> &parameters,
    int preferredComponent)
{
    // Replacing the accepted snapshot cannot cancel writes already queued in
    // ParameterService. Keep ownership until its terminal batch completion so
    // Discard or a second Write can never race the old PARAM_SET sequence.
    if (Busy()) {
        setStatus(tr("OSD writes are still pending; refresh after they finish."));
        return;
    }
    const int previouslySelectedScreen = m_selectedScreen;
    const int previousComponent = m_componentId;
    QMap<QString, ConfigOSDItem> previousItems;
    for (const ConfigOSDItem &item : m_items) {
        previousItems.insert(QString::number(item.screen) + QLatin1Char('/')
                                 + normalizedItemName(item.name),
                             item);
    }
    ++m_snapshotRevision;
    abandonBatch(QString());

    QList<ConfigFriendlyParameterValue> ordered = parameters;
    std::sort(ordered.begin(), ordered.end(), lessSnapshotValue);

    QMap<int, ComponentValues> byComponent;
    for (const ConfigFriendlyParameterValue &parameter : ordered) {
        if (parameter.componentId < 0 || parameter.componentId > 255) {
            continue;
        }
        const QString original = parameter.name.trimmed();
        const QString normalized = normalizedName(original);
        if (normalized.isEmpty()
            || byComponent[parameter.componentId].contains(normalized)) {
            continue;
        }
        byComponent[parameter.componentId].insert(
            normalized, SnapshotValue{original, normalized, parameter.value});
    }

    static const QRegularExpression enablePattern(
        QStringLiteral("^OSD([0-9]+)_(.+)_EN$"),
        QRegularExpression::CaseInsensitiveOption);
    QMap<int, QList<ConfigOSDItem>> parsedByComponent;
    for (auto component = byComponent.constBegin();
         component != byComponent.constEnd(); ++component) {
        QSet<QString> seenItems;
        QList<ConfigOSDItem> parsed;
        const ComponentValues &values = component.value();
        for (auto value = values.constBegin(); value != values.constEnd();
             ++value) {
            const QRegularExpressionMatch match =
                enablePattern.match(value.key());
            if (!match.hasMatch()) {
                continue;
            }
            bool screenOk = false;
            const int screen = match.captured(1).toInt(&screenOk);
            const QString itemName = normalizedItemName(match.captured(2));
            // MP10's selected-screen implementation treats zero as no screen;
            // do not create an unselectable OSD0 item.
            if (!screenOk || screen <= 0 || itemName.isEmpty()) {
                continue;
            }
            const QString prefix = QStringLiteral("OSD")
                + match.captured(1) + QLatin1Char('_') + itemName;
            const QString xName = prefix + QStringLiteral("_X");
            const QString yName = prefix + QStringLiteral("_Y");
            if (!values.contains(xName) || !values.contains(yName)) {
                continue;
            }
            const QString itemKey = QString::number(screen)
                + QLatin1Char('/') + itemName;
            if (seenItems.contains(itemKey)) {
                continue;
            }
            seenItems.insert(itemKey);

            ConfigOSDItem item;
            item.screen = screen;
            item.name = itemName;
            item.enableParameter = value.value().originalName;
            item.xParameter = values.value(xName).originalName;
            item.yParameter = values.value(yName).originalName;
            item.acceptedEnabled = parameterValueEnabled(
                value.value().value);
            item.acceptedX = snapshotPosition(values.value(xName).value);
            item.acceptedY = snapshotPosition(values.value(yName).value);
            item.enabled = item.acceptedEnabled;
            item.x = item.acceptedX;
            item.y = item.acceptedY;
            parsed.append(item);
        }
        std::sort(parsed.begin(), parsed.end(), lessItem);
        if (!parsed.isEmpty()) {
            parsedByComponent.insert(component.key(), parsed);
        }
    }

    if (parsedByComponent.contains(preferredComponent)) {
        m_componentId = preferredComponent;
    } else if (parsedByComponent.contains(1)) {
        m_componentId = 1;
    } else if (!parsedByComponent.isEmpty()) {
        m_componentId = parsedByComponent.firstKey();
    } else {
        m_componentId = preferredComponent;
    }

    m_items = parsedByComponent.value(m_componentId);
    if (m_componentId == previousComponent) {
        for (ConfigOSDItem &item : m_items) {
            const QString key = QString::number(item.screen)
                + QLatin1Char('/') + normalizedItemName(item.name);
            const auto previous = previousItems.constFind(key);
            if (previous == previousItems.constEnd()) {
                continue;
            }
            const ConfigOSDItem &old = previous.value();
            const bool sameParameters =
                normalizedName(old.enableParameter)
                    == normalizedName(item.enableParameter)
                && normalizedName(old.xParameter)
                    == normalizedName(item.xParameter)
                && normalizedName(old.yParameter)
                    == normalizedName(item.yParameter);
            const bool sameAccepted =
                old.acceptedEnabled == item.acceptedEnabled
                && old.acceptedX == item.acceptedX
                && old.acceptedY == item.acceptedY;
            if (sameParameters && sameAccepted) {
                item.enabled = old.enabled;
                item.x = old.x;
                item.y = old.y;
            }
        }
    }
    m_screens.clear();
    int lastScreen = -1;
    for (const ConfigOSDItem &item : m_items) {
        if (item.screen != lastScreen) {
            m_screens.append(item.screen);
            lastScreen = item.screen;
        }
    }
    m_snapshotReady = true;
    if (m_screens.contains(previouslySelectedScreen)) {
        m_selectedScreen = previouslySelectedScreen;
    } else {
        m_selectedScreen = m_screens.isEmpty() ? 0 : m_screens.first();
    }

    const QPointer<ConfigOSDViewModel> guard(this);
    const quint64 completedRevision = m_snapshotRevision;
    if (m_screens.isEmpty()) {
        setStatus(NoParametersStatus());
    } else {
        updateScreenStatus();
    }
    if (!guard || m_snapshotRevision != completedRevision) {
        return;
    }
    emit screensChanged();
    if (!guard || m_snapshotRevision != completedRevision) {
        return;
    }
    emit selectedScreenChanged(m_selectedScreen);
    if (!guard || m_snapshotRevision != completedRevision) {
        return;
    }
    emit itemsChanged();
    if (!guard || m_snapshotRevision != completedRevision) {
        return;
    }
    emit dirtyChanged();
    if (!guard || m_snapshotRevision != completedRevision) {
        return;
    }
    emit stateChanged();
}

void ConfigOSDViewModel::parameterTargetChanged()
{
    invalidateSnapshot(TargetChangedStatus());
}

bool ConfigOSDViewModel::selectScreen(int screen)
{
    if (!m_screens.contains(screen) || m_selectedScreen == screen) {
        return false;
    }
    m_selectedScreen = screen;
    const QPointer<ConfigOSDViewModel> guard(this);
    const quint64 revision = m_snapshotRevision;
    if (!Busy()) {
        updateScreenStatus();
    }
    if (!guard || m_snapshotRevision != revision) {
        return true;
    }
    emit selectedScreenChanged(screen);
    if (!guard || m_snapshotRevision != revision) {
        return true;
    }
    emit itemsChanged();
    return true;
}

void ConfigOSDViewModel::notifyItemChanged(
    const ConfigOSDItem &item, int previousDirtyCount)
{
    // Direct receivers may replace the snapshot, so never retain a reference
    // into m_items across a signal emission.
    const int screen = item.screen;
    const QString name = item.name;
    const quint64 revision = m_snapshotRevision;
    const QPointer<ConfigOSDViewModel> guard(this);
    emit itemChanged(screen, name);
    if (!guard || m_snapshotRevision != revision) {
        return;
    }
    if (screen == m_selectedScreen) {
        emit itemsChanged();
        if (!guard || m_snapshotRevision != revision) {
            return;
        }
    }
    if (DirtyFieldCount() != previousDirtyCount) {
        emit dirtyChanged();
        if (!guard || m_snapshotRevision != revision) {
            return;
        }
    }
    emit stateChanged();
}

bool ConfigOSDViewModel::setItemEnabled(
    int screen, const QString &name, bool enabled)
{
    if (!CanEdit()) {
        return false;
    }
    ConfigOSDItem *item = itemFor(screen, name);
    if (!item || item->enabled == enabled) {
        return false;
    }
    const int previousDirtyCount = DirtyFieldCount();
    item->enabled = enabled;
    notifyItemChanged(*item, previousDirtyCount);
    return true;
}

bool ConfigOSDViewModel::setItemCoordinate(
    int screen, const QString &name, int value, bool horizontal)
{
    if (!CanEdit()) {
        return false;
    }
    ConfigOSDItem *item = itemFor(screen, name);
    if (!item) {
        return false;
    }
    const int bounded = qBound(0, value,
        horizontal ? Columns() - 1 : Rows() - 1);
    int &coordinate = horizontal ? item->x : item->y;
    if (coordinate == bounded) {
        return false;
    }
    const int previousDirtyCount = DirtyFieldCount();
    coordinate = bounded;
    notifyItemChanged(*item, previousDirtyCount);
    return true;
}

bool ConfigOSDViewModel::setItemX(
    int screen, const QString &name, int x)
{
    return setItemCoordinate(screen, name, x, true);
}

bool ConfigOSDViewModel::setItemY(
    int screen, const QString &name, int y)
{
    return setItemCoordinate(screen, name, y, false);
}

bool ConfigOSDViewModel::setItemPosition(
    int screen, const QString &name, int x, int y)
{
    if (!CanEdit()) {
        return false;
    }
    ConfigOSDItem *item = itemFor(screen, name);
    if (!item) {
        return false;
    }
    const int boundedX = qBound(0, x, Columns() - 1);
    const int boundedY = qBound(0, y, Rows() - 1);
    if (item->x == boundedX && item->y == boundedY) {
        return false;
    }
    const int previousDirtyCount = DirtyFieldCount();
    item->x = boundedX;
    item->y = boundedY;
    notifyItemChanged(*item, previousDirtyCount);
    return true;
}

bool ConfigOSDViewModel::EnableAll(bool enabled)
{
    if (!CanEdit() || m_selectedScreen <= 0) {
        return false;
    }
    const int previousDirtyCount = DirtyFieldCount();
    QStringList changedItems;
    for (ConfigOSDItem &item : m_items) {
        if (item.screen == m_selectedScreen && item.enabled != enabled) {
            item.enabled = enabled;
            changedItems.append(item.name);
        }
    }
    if (changedItems.isEmpty()) {
        return false;
    }
    const int changedScreen = m_selectedScreen;
    const quint64 revision = m_snapshotRevision;
    const QPointer<ConfigOSDViewModel> guard(this);
    for (const QString &name : changedItems) {
        emit itemChanged(changedScreen, name);
        if (!guard || m_snapshotRevision != revision) {
            return true;
        }
    }
    emit itemsChanged();
    if (!guard || m_snapshotRevision != revision) {
        return true;
    }
    if (DirtyFieldCount() != previousDirtyCount) {
        emit dirtyChanged();
        if (!guard || m_snapshotRevision != revision) {
            return true;
        }
    }
    emit stateChanged();
    return true;
}

bool ConfigOSDViewModel::Discard()
{
    if (Busy() || !m_snapshotReady || !HasDirtyChanges()) {
        return false;
    }
    QList<QPair<int, QString>> changedItems;
    bool selectedChanged = false;
    for (ConfigOSDItem &item : m_items) {
        if (!item.dirty()) {
            continue;
        }
        item.enabled = item.acceptedEnabled;
        item.x = item.acceptedX;
        item.y = item.acceptedY;
        changedItems.append(qMakePair(item.screen, item.name));
        if (item.screen == m_selectedScreen) {
            selectedChanged = true;
        }
    }
    // Emit only after the mutation pass: a direct receiver is allowed to
    // replace the snapshot without invalidating our m_items iteration.
    const quint64 revision = m_snapshotRevision;
    const QPointer<ConfigOSDViewModel> guard(this);
    for (const auto &changed : changedItems) {
        emit itemChanged(changed.first, changed.second);
        if (!guard || m_snapshotRevision != revision) {
            return true;
        }
    }
    if (selectedChanged) {
        emit itemsChanged();
        if (!guard || m_snapshotRevision != revision) {
            return true;
        }
    }
    emit dirtyChanged();
    if (!guard || m_snapshotRevision != revision) {
        return true;
    }
    emit stateChanged();
    if (!guard || m_snapshotRevision != revision) {
        return true;
    }
    updateScreenStatus();
    return true;
}

bool ConfigOSDViewModel::WriteChanges()
{
    if (!m_connected) {
        setStatus(OfflineStatus());
        return false;
    }
    if (!m_snapshotReady) {
        setStatus(UnreadyStatus());
        return false;
    }
    if (Busy()) {
        return false;
    }
    const QVariantList changes = DirtyChanges();
    if (changes.isEmpty()) {
        setStatus(NoChangesStatus());
        return false;
    }

    Batch batch;
    batch.active = true;
    batch.revision = m_snapshotRevision;
    batch.serial = ++m_batchSerial;
    batch.total = changes.size();
    batch.changes = changes;
    m_batch = batch;
    const QPointer<ConfigOSDViewModel> guard(this);
    setStatus(WritingStatus());
    if (!guard || !m_batch.active || m_batch.serial != batch.serial) {
        return false;
    }
    emit stateChanged();
    if (!guard || !m_batch.active || m_batch.serial != batch.serial) {
        return false;
    }

    const quint64 serial = batch.serial;
    QTimer::singleShot(m_batchTimeoutMs, this, [this, serial]() {
        if (m_batch.active && m_batch.serial == serial) {
            // A UI timer cannot cancel the service's queued transactions.
            // Keep Busy true and ownership intact until batch completion.
            setStatus(TimeoutStatus());
        }
    });

    emit writeParamsRequested(m_componentId, changes);
    return !guard.isNull();
}

void ConfigOSDViewModel::setBatchTimeoutForTesting(int milliseconds)
{
    m_batchTimeoutMs = qMax(1, milliseconds);
}

bool ConfigOSDViewModel::ownsBatch(qulonglong batchId) const
{
    return m_batch.active && m_batch.batchId != 0
        && m_batch.batchId == batchId
        && m_batch.revision == m_snapshotRevision;
}

bool ConfigOSDViewModel::batchContains(const QString &parameter) const
{
    const QString wanted = normalizedName(parameter);
    for (const QVariant &change : m_batch.changes) {
        if (normalizedName(change.toMap().value(
                QStringLiteral("name")).toString()) == wanted) {
            return true;
        }
    }
    return false;
}

void ConfigOSDViewModel::parameterBatchSubmitted(
    int componentId, qulonglong batchId)
{
    if (!m_batch.active || m_batch.batchId != 0 || batchId == 0
        || componentId != m_componentId
        || m_batch.revision != m_snapshotRevision) {
        return;
    }
    m_batch.batchId = batchId;
    m_batch.failedNames.unite(m_batch.earlyFailedNames.value(batchId));
    m_batch.failureDetails.append(
        m_batch.earlyFailureDetails.value(batchId));
    m_batch.earlyFailedNames.clear();
    m_batch.earlyFailureDetails.clear();
    if (m_batch.earlyCompletion
        && m_batch.earlyBatchId == batchId) {
        finishBatch(m_batch.earlySucceeded, m_batch.earlyFailed);
    }
}

void ConfigOSDViewModel::parameterBatchProgress(
    qulonglong batchId, int completed, int total,
    int succeeded, int failed)
{
    Q_UNUSED(succeeded)
    Q_UNUSED(failed)
    if (!ownsBatch(batchId)) {
        return;
    }
    setStatus(tr("Writing OSD changes… (%1/%2)")
                  .arg(completed).arg(total));
}

void ConfigOSDViewModel::parameterWriteFailed(
    qulonglong batchId, int componentId,
    const QString &name, const QString &reason)
{
    if (componentId != m_componentId || !batchContains(name)) {
        return;
    }
    const QString normalized = normalizedName(name);
    if (m_batch.active && m_batch.batchId == 0 && batchId != 0
        && m_batch.revision == m_snapshotRevision) {
        QSet<QString> &names = m_batch.earlyFailedNames[batchId];
        if (!names.contains(normalized)) {
            names.insert(normalized);
            if (!reason.trimmed().isEmpty()) {
                m_batch.earlyFailureDetails[batchId].append(
                    QStringLiteral("%1: %2").arg(name.trimmed(),
                                                 reason.trimmed()));
            }
        }
        return;
    }
    if (!ownsBatch(batchId)) {
        return;
    }
    if (!m_batch.failedNames.contains(normalized)) {
        m_batch.failedNames.insert(normalized);
        if (!reason.trimmed().isEmpty()) {
            m_batch.failureDetails.append(
                QStringLiteral("%1: %2").arg(name.trimmed(),
                                             reason.trimmed()));
        }
    }
}

void ConfigOSDViewModel::parameterBatchCompleted(
    qulonglong batchId, int succeeded, int failed)
{
    if (m_batch.active && m_batch.batchId == 0 && batchId != 0
        && m_batch.revision == m_snapshotRevision
        && !m_batch.earlyCompletion) {
        m_batch.earlyCompletion = true;
        m_batch.earlyBatchId = batchId;
        m_batch.earlySucceeded = succeeded;
        m_batch.earlyFailed = failed;
        return;
    }
    if (!ownsBatch(batchId)) {
        return;
    }
    finishBatch(succeeded, failed);
}

void ConfigOSDViewModel::parameterBatchCancelled(qulonglong batchId)
{
    if (!ownsBatch(batchId)) {
        return;
    }
    // QGCUASParamManager reports cancellation per transaction. Other writes
    // in the same batch may still be queued, so only the terminal batch
    // completion is allowed to release Busy/ownership.
    setStatus(CancelledStatus());
}

void ConfigOSDViewModel::parameterWriteSubmissionFailed(
    const QString &reason)
{
    if (!m_batch.active || m_batch.batchId != 0) {
        return;
    }
    const QString detail = reason.trimmed().isEmpty()
        ? tr("The writes could not be submitted; changes remain staged.")
        : tr("%1; changes remain staged.").arg(reason.trimmed());
    const QPointer<ConfigOSDViewModel> guard(this);
    abandonBatch(FailureStatus(detail));
    if (!guard) {
        return;
    }
    emit stateChanged();
}

void ConfigOSDViewModel::acceptPendingChanges(
    const QSet<QString> &excludedNames)
{
    for (const QVariant &changeValue : m_batch.changes) {
        const QVariantMap change = changeValue.toMap();
        const QString parameter = change.value(
            QStringLiteral("name")).toString();
        const QString normalized = normalizedName(parameter);
        if (excludedNames.contains(normalized)) {
            continue;
        }
        for (ConfigOSDItem &item : m_items) {
            if (normalizedName(item.enableParameter) == normalized) {
                item.acceptedEnabled = item.enabled;
                break;
            }
            if (normalizedName(item.xParameter) == normalized) {
                item.acceptedX = item.x;
                break;
            }
            if (normalizedName(item.yParameter) == normalized) {
                item.acceptedY = item.y;
                break;
            }
        }
    }
}

void ConfigOSDViewModel::finishBatch(int succeeded, int failed)
{
    const quint64 revision = m_snapshotRevision;
    const int previousDirtyCount = DirtyFieldCount();
    const int total = m_batch.total;
    const int inferredFailures = qMax(0, total - succeeded);
    const int failedCount = qMax(failed, inferredFailures);
    const bool fullSuccess = failedCount == 0 && succeeded >= total;
    const bool failuresExactlyNamed = failedCount > 0
        && m_batch.failedNames.size() == failedCount
        && succeeded + failedCount >= total;

    if (fullSuccess) {
        acceptPendingChanges({});
    } else if (failuresExactlyNamed) {
        acceptPendingChanges(m_batch.failedNames);
    }

    QStringList failedNames;
    if (failuresExactlyNamed) {
        for (const QVariant &change : m_batch.changes) {
            const QString name = change.toMap().value(
                QStringLiteral("name")).toString();
            if (m_batch.failedNames.contains(normalizedName(name))) {
                failedNames.append(name);
            }
        }
    }
    m_batch = Batch();

    const QPointer<ConfigOSDViewModel> guard(this);
    if (fullSuccess) {
        setStatus(SuccessStatus());
    } else if (failuresExactlyNamed) {
        setStatus(FailureStatus(
            tr("%1 of %2 writes failed: %3. Failed changes remain staged.")
                .arg(failedCount).arg(total)
                .arg(failedNames.join(QStringLiteral(", ")))));
    } else {
        setStatus(FailureStatus(
            tr("%1 of %2 writes were not confirmed; all changes remain staged.")
                .arg(failedCount).arg(total)));
    }
    if (!guard || m_snapshotRevision != revision) {
        return;
    }
    emit itemsChanged();
    if (!guard || m_snapshotRevision != revision) {
        return;
    }
    if (DirtyFieldCount() != previousDirtyCount) {
        emit dirtyChanged();
        if (!guard || m_snapshotRevision != revision) {
            return;
        }
    }
    emit stateChanged();
}

void ConfigOSDViewModel::abandonBatch(const QString &status)
{
    if (!m_batch.active) {
        return;
    }
    m_batch = Batch();
    if (!status.isEmpty()) {
        setStatus(status);
    }
}

void ConfigOSDViewModel::invalidateSnapshot(const QString &status)
{
    ++m_snapshotRevision;
    abandonBatch(QString());
    m_items.clear();
    m_screens.clear();
    m_selectedScreen = 0;
    m_snapshotReady = false;
    const quint64 invalidatedRevision = m_snapshotRevision;
    const QPointer<ConfigOSDViewModel> guard(this);
    setStatus(status);
    if (!guard || m_snapshotRevision != invalidatedRevision) {
        return;
    }
    emit screensChanged();
    if (!guard || m_snapshotRevision != invalidatedRevision) {
        return;
    }
    emit selectedScreenChanged(0);
    if (!guard || m_snapshotRevision != invalidatedRevision) {
        return;
    }
    emit itemsChanged();
    if (!guard || m_snapshotRevision != invalidatedRevision) {
        return;
    }
    emit dirtyChanged();
    if (!guard || m_snapshotRevision != invalidatedRevision) {
        return;
    }
    emit stateChanged();
}

void ConfigOSDViewModel::setStatus(const QString &status)
{
    if (m_status == status) {
        return;
    }
    m_status = status;
    emit statusChanged(status);
}

void ConfigOSDViewModel::updateScreenStatus()
{
    int itemCount = 0;
    for (const ConfigOSDItem &item : m_items) {
        itemCount += item.screen == m_selectedScreen ? 1 : 0;
    }
    setStatus(m_selectedScreen > 0
                  ? ScreenStatus(m_selectedScreen, itemCount)
                  : NoParametersStatus());
}
