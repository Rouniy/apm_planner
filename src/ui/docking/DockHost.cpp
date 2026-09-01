#include "DockHost.h"

#include <kddockwidgets/DockWidget.h>
#include <kddockwidgets/LayoutSaver.h>
#include <kddockwidgets/MainWindow.h>

#include <QAction>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVariant>
#include <QVBoxLayout>

namespace {
constexpr int kLayoutSchemaVersion = 1;
const char kLayoutSchema[] = "apmplanner-dock-layout";
const char kKddockwidgetsVersion[] = "1.4.0";

KDDockWidgets::Location toKddockLocation(DockHost::Location location)
{
    switch (location) {
    case DockHost::Location::Left:
        return KDDockWidgets::Location_OnLeft;
    case DockHost::Location::Right:
        return KDDockWidgets::Location_OnRight;
    case DockHost::Location::Top:
        return KDDockWidgets::Location_OnTop;
    case DockHost::Location::Bottom:
        return KDDockWidgets::Location_OnBottom;
    case DockHost::Location::Tabbed:
        break;
    }
    return KDDockWidgets::Location_None;
}
}

class DockHost::Private
{
public:
    QString viewId;
    QString affinity;
    KDDockWidgets::MainWindow *mainWindow = nullptr;
    QHash<QString, KDDockWidgets::DockWidget *> docks;
    QStringList dockOrder;
};

DockHost::DockHost(const QString &viewId, QWidget *parent)
    : QWidget(parent),
      d(new Private)
{
    d->viewId = viewId.trimmed();
    Q_ASSERT(!d->viewId.isEmpty());
    d->affinity = QStringLiteral("apmplanner-%1").arg(d->viewId);
    setObjectName(QStringLiteral("%1DockHost").arg(d->viewId));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    d->mainWindow = new KDDockWidgets::MainWindow(
        QStringLiteral("APMPlanner3.%1").arg(d->viewId),
        KDDockWidgets::MainWindowOption_None,
        this);
    d->mainWindow->setObjectName(QStringLiteral("%1DockingArea").arg(d->viewId));
    d->mainWindow->setWindowFlag(Qt::Window, false);
    d->mainWindow->setAffinities(QStringList{d->affinity});
    layout->addWidget(d->mainWindow);
}

DockHost::~DockHost() = default;

QString DockHost::viewId() const
{
    return d->viewId;
}

QString DockHost::affinity() const
{
    return d->affinity;
}

QStringList DockHost::dockIds() const
{
    return d->dockOrder;
}

bool DockHost::addDock(const QString &dockId,
                       const QString &title,
                       QWidget *content,
                       Location location,
                       const QString &relativeToDockId,
                       const QSize &preferredSize)
{
    const QString id = dockId.trimmed();
    if (id.isEmpty() || !content || d->docks.contains(id)) {
        return false;
    }

    KDDockWidgets::DockWidget *relativeDock = nullptr;
    if (!relativeToDockId.isEmpty()) {
        relativeDock = d->docks.value(relativeToDockId, nullptr);
        if (!relativeDock) {
            return false;
        }
    }
    if (location == Location::Tabbed && !relativeDock) {
        return false;
    }

    auto *dock = new KDDockWidgets::DockWidget(
        QStringLiteral("APMPlanner3.%1.%2").arg(d->viewId, id),
        KDDockWidgets::DockWidgetBase::Option_NotClosable);
    dock->setObjectName(id);
    dock->setProperty("dockId", id);
    dock->setAffinityName(d->affinity);
    dock->setTitle(title);
    content->setProperty("dockId", id);
    dock->setWidget(content);

    if (location == Location::Tabbed) {
        relativeDock->addDockWidgetAsTab(dock);
    } else {
        const KDDockWidgets::InitialOption option = preferredSize.isValid()
            ? KDDockWidgets::InitialOption(preferredSize)
            : KDDockWidgets::InitialOption();
        d->mainWindow->addDockWidget(dock,
                                     toKddockLocation(location),
                                     relativeDock,
                                     option);
    }
    d->docks.insert(id, dock);
    d->dockOrder.append(id);
    return true;
}

QAction *DockHost::toggleAction(const QString &dockId) const
{
    KDDockWidgets::DockWidget *dock = d->docks.value(dockId, nullptr);
    return dock ? dock->toggleAction() : nullptr;
}

bool DockHost::isDockOpen(const QString &dockId) const
{
    KDDockWidgets::DockWidget *dock = d->docks.value(dockId, nullptr);
    return dock && dock->isOpen();
}

bool DockHost::setDockVisible(const QString &dockId, bool visible)
{
    KDDockWidgets::DockWidget *dock = d->docks.value(dockId, nullptr);
    if (!dock) {
        return false;
    }
    if (!visible && dock->isOpen()) {
        int openCount = 0;
        for (KDDockWidgets::DockWidget *candidate : d->docks) {
            if (candidate && candidate->isOpen()) {
                ++openCount;
            }
        }
        if (openCount <= 1) {
            return false;
        }
    }
    if (visible) {
        dock->show();
    } else {
        dock->close();
    }
    return true;
}

bool DockHost::hasOpenDock() const
{
    for (KDDockWidgets::DockWidget *dock : d->docks) {
        if (dock && dock->isOpen()) {
            return true;
        }
    }
    return false;
}

void DockHost::showAllDocks()
{
    for (const QString &dockId : d->dockOrder) {
        if (KDDockWidgets::DockWidget *dock = d->docks.value(dockId, nullptr)) {
            dock->show();
        }
    }
}

QByteArray DockHost::saveLayout() const
{
    KDDockWidgets::LayoutSaver saver(
        KDDockWidgets::RestoreOption_RelativeToMainWindow);
    saver.setAffinityNames(QStringList{d->affinity});
    const QByteArray payload = saver.serializeLayout();
    if (payload.isEmpty()) {
        return QByteArray();
    }

    QJsonObject root;
    root.insert(QStringLiteral("schema"), QString::fromLatin1(kLayoutSchema));
    root.insert(QStringLiteral("version"), kLayoutSchemaVersion);
    root.insert(QStringLiteral("viewId"), d->viewId);
    root.insert(QStringLiteral("affinity"), d->affinity);
    root.insert(QStringLiteral("kddockwidgets"),
                QString::fromLatin1(kKddockwidgetsVersion));
    QJsonArray dockIds;
    for (const QString &dockId : d->dockOrder) {
        dockIds.append(dockId);
    }
    root.insert(QStringLiteral("docks"), dockIds);
    root.insert(QStringLiteral("payload"), QString::fromLatin1(payload.toBase64()));
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

bool DockHost::restoreLayout(const QByteArray &envelope)
{
    const auto reject = [this](const QString &reason) {
        // A corrupt or all-closed persisted layout must never leave a core
        // DATA/PLAN page as an empty white docking host.
        showAllDocks();
        emit layoutRestoreRejected(reason);
        return false;
    };
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(envelope, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return reject(tr("Dock layout is not valid JSON"));
    }

    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("schema")).toString()
            != QString::fromLatin1(kLayoutSchema)
        || root.value(QStringLiteral("version")).toInt(-1) != kLayoutSchemaVersion) {
        return reject(tr("Dock layout schema is not supported"));
    }
    if (root.value(QStringLiteral("viewId")).toString() != d->viewId
        || root.value(QStringLiteral("affinity")).toString() != d->affinity) {
        return reject(tr("Dock layout belongs to another view"));
    }
    if (root.value(QStringLiteral("kddockwidgets")).toString()
        != QString::fromLatin1(kKddockwidgetsVersion)) {
        return reject(tr("Dock layout library version is not supported"));
    }

    QStringList savedDockIds;
    const QJsonArray dockIds = root.value(QStringLiteral("docks")).toArray();
    for (const QJsonValue &dockId : dockIds) {
        if (!dockId.isString()) {
            return reject(tr("Dock layout panel list is invalid"));
        }
        savedDockIds.append(dockId.toString());
    }
    if (savedDockIds != d->dockOrder) {
        return reject(tr("Dock layout panel set does not match this view"));
    }

    const QByteArray payload = QByteArray::fromBase64(
        root.value(QStringLiteral("payload")).toString().toLatin1());
    if (payload.isEmpty() || !QJsonDocument::fromJson(payload).isObject()) {
        return reject(tr("Dock layout payload is invalid"));
    }

    KDDockWidgets::LayoutSaver saver(
        KDDockWidgets::RestoreOption_RelativeToMainWindow);
    saver.setAffinityNames(QStringList{d->affinity});
    if (!saver.restoreLayout(payload)) {
        return reject(tr("KDDockWidgets rejected the dock layout"));
    }
    if (!hasOpenDock()) {
        showAllDocks();
    }
    return true;
}

int DockHost::layoutSchemaVersion()
{
    return kLayoutSchemaVersion;
}
