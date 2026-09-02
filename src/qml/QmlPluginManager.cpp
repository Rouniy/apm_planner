#include "QmlPluginManager.h"

#include "ApmQmlApi.h"
#include "AppPaths.h"
#include "QmlPluginContext.h"
#include "QmlPluginManifest.h"
#include "ui/flightplanner/MissionCommandCatalog.h"

#include <QAction>
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QIcon>
#include <QMenu>
#include <QPointer>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickWidget>
#include <QScopedValueRollback>
#include <QSet>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <QtQml>

#include <algorithm>
#include <vector>

namespace
{

QPointer<ApmQmlApi> s_api;

QObject *qmlApiProvider(QQmlEngine *engine, QJSEngine *)
{
    if (!s_api) {
        return nullptr;
    }

    Q_ASSERT(s_api->thread() == engine->thread());
    QQmlEngine::setObjectOwnership(s_api, QQmlEngine::CppOwnership);
    return s_api;
}

void registerApi(ApmQmlApi *api)
{
    static const int registration = qmlRegisterSingletonType<ApmQmlApi>(
        "APMPlanner.Core", 1, 0, "APMPlanner", qmlApiProvider);
    Q_UNUSED(registration);
    s_api = api;
}

QStringList defaultSearchRoots()
{
    const QString userRoot = QDir(AppPaths::writableDataDirectory()).filePath(
        QStringLiteral("qml-plugins"));
    AppPaths::ensureDirectory(userRoot);
    return {
        AppPaths::resourcePath(QStringLiteral("qml-plugins")),
        userRoot
    };
}

QString qmlErrors(const QList<QQmlError> &errors)
{
    QStringList messages;
    messages.reserve(errors.size());
    for (const QQmlError &error : errors) {
        messages.append(error.toString());
    }
    return messages.join(QLatin1Char('\n'));
}

} // namespace

class QmlPluginManager::Private
{
public:
    struct Plugin
    {
        QmlPluginManifest manifest;
        QPointer<QAction> action;
        QPointer<QDialog> window;
        bool opening = false;
    };

    Private(QmlPluginManager *owner, QObject *uasManager,
            QObject *linkManager, QObject *settings)
        : q(owner)
        , api(new ApmQmlApi(uasManager, linkManager, settings, owner))
    {
        registerApi(api.get());
        engine.reset(new QQmlEngine);

        const QList<QObject *> coreObjects = {
            owner, api.get(), uasManager, linkManager, settings,
            QCoreApplication::instance(), MissionCommandCatalog::instance()
        };
        for (QObject *object : coreObjects) {
            if (object) {
                QQmlEngine::setObjectOwnership(object,
                                                QQmlEngine::CppOwnership);
            }
        }
    }

    ~Private()
    {
        clear();
        engine.reset();
        if (s_api == api.get()) {
            s_api = nullptr;
        }
    }

    void clear()
    {
        if (clearing) {
            return;
        }
        QScopedValueRollback<bool> clearingGuard(clearing, true);
        for (Plugin &plugin : plugins) {
            QDialog *window = plugin.window.data();
            plugin.window = nullptr;
            delete window;
        }
        plugins.clear();

        api->clearVehicleActions();

        delete pluginMenu.data();
        pluginMenu = nullptr;
        diagnostics.clear();

        if (engine) {
            engine->clearComponentCache();
            engine->collectGarbage();
        }
    }

    void reportError(const QString &pluginId, const QString &message)
    {
        const QString diagnostic = pluginId.isEmpty()
            ? message : QStringLiteral("%1: %2").arg(pluginId, message);
        diagnostics.append(diagnostic);
        emit q->pluginLoadError(pluginId, message);
    }

    bool openPlugin(int index)
    {
        if (clearing || stopped) {
            return false;
        }
        if (index < 0 || index >= static_cast<int>(plugins.size())) {
            return false;
        }

        Plugin &plugin = plugins[static_cast<std::size_t>(index)];
        if (plugin.opening) {
            return false;
        }
        if (plugin.window) {
            plugin.window->show();
            plugin.window->raise();
            plugin.window->activateWindow();
            return true;
        }
        QScopedValueRollback<bool> openingGuard(plugin.opening, true);

        QWidget *parentWidget = qobject_cast<QWidget *>(api->mainWindow());
        auto *window = new QDialog(parentWidget);
        window->setAttribute(Qt::WA_DeleteOnClose, false);
        window->setObjectName(QStringLiteral("QmlPluginWindow_%1")
                                  .arg(plugin.manifest.id()));
        window->setWindowTitle(plugin.manifest.ui().title);
        window->resize(800, 600);

        auto *layout = new QVBoxLayout(window);
        layout->setContentsMargins(0, 0, 0, 0);

        auto *quickWidget = new QQuickWidget(engine.get(), window);
        quickWidget->setObjectName(QStringLiteral("QmlPluginView_%1")
                                       .arg(plugin.manifest.id()));
        quickWidget->setResizeMode(QQuickWidget::SizeRootObjectToView);

        auto *context = new QmlPluginContext(
            plugin.manifest.id(), plugin.manifest.name(),
            plugin.manifest.version(), plugin.manifest.pluginDirectory(),
            quickWidget);
        QQmlEngine::setObjectOwnership(context, QQmlEngine::CppOwnership);
        quickWidget->rootContext()->setContextProperty(
            QStringLiteral("plugin"), context);
        quickWidget->setSource(
            QUrl::fromLocalFile(plugin.manifest.mainFilePath()));

        if (quickWidget->status() == QQuickWidget::Error) {
            const QString message = qmlErrors(quickWidget->errors());
            reportError(plugin.manifest.id(), message);
            delete window;
            emit q->pluginsChanged();
            return false;
        }

        layout->addWidget(quickWidget);
        plugin.window = window;
        window->show();
        return true;
    }

    QmlPluginManager *q = nullptr;
    std::unique_ptr<ApmQmlApi> api;
    std::unique_ptr<QQmlEngine> engine;
    QPointer<QMenu> pluginMenu;
    QPointer<QMenu> toolsMenu;
    std::vector<Plugin> plugins;
    QStringList diagnostics;
    QStringList searchRoots;
    bool reloadPending = false;
    bool stopped = false;
    bool clearing = false;
};

QmlPluginManager::QmlPluginManager(QObject *uasManager, QObject *linkManager,
                                   QObject *settings, QObject *parent)
    : QObject(parent)
    , d(new Private(this, uasManager, linkManager, settings))
{
}

QmlPluginManager::~QmlPluginManager() = default;

QQmlEngine *QmlPluginManager::engine() const
{
    return d->engine.get();
}

ApmQmlApi *QmlPluginManager::api() const
{
    return d->api.get();
}

int QmlPluginManager::pluginCount() const
{
    return static_cast<int>(d->plugins.size());
}

QStringList QmlPluginManager::pluginIds() const
{
    QStringList ids;
    ids.reserve(pluginCount());
    for (const Private::Plugin &plugin : d->plugins) {
        ids.append(plugin.manifest.id());
    }
    return ids;
}

QStringList QmlPluginManager::errors() const
{
    return d->diagnostics;
}

QVariantList QmlPluginManager::plugins() const
{
    QVariantList details;
    details.reserve(pluginCount());
    for (const Private::Plugin &plugin : d->plugins) {
        const QmlPluginManifest &manifest = plugin.manifest;
        QVariantMap item;
        item.insert(QStringLiteral("id"), manifest.id());
        item.insert(QStringLiteral("name"), manifest.name());
        item.insert(QStringLiteral("version"), manifest.version());
        item.insert(QStringLiteral("author"), manifest.author());
        item.insert(QStringLiteral("description"), manifest.description());
        item.insert(QStringLiteral("directory"), manifest.pluginDirectory());
        item.insert(QStringLiteral("title"), manifest.ui().title);
        details.append(item);
    }
    return details;
}

QStringList QmlPluginManager::searchRoots() const
{
    return d->searchRoots;
}

QString QmlPluginManager::writablePluginDirectory() const
{
    return defaultSearchRoots().constLast();
}

void QmlPluginManager::setMainWindow(QObject *mainWindow)
{
    d->api->setMainWindow(mainWindow);
    if (mainWindow) {
        QQmlEngine::setObjectOwnership(mainWindow, QQmlEngine::CppOwnership);
    }
}

void QmlPluginManager::setActiveVehicle(QObject *activeVehicle)
{
    d->api->setActiveVehicle(activeVehicle);
}

void QmlPluginManager::start(QMenu *toolsMenu,
                             const QStringList &searchRoots)
{
    d->clear();
    d->stopped = false;
    d->toolsMenu = toolsMenu;

    QStringList roots = searchRoots.isEmpty()
        ? defaultSearchRoots() : searchRoots;
    for (QString &root : roots) {
        root = QDir::cleanPath(QFileInfo(root).absoluteFilePath());
    }
    roots.removeDuplicates();
    std::sort(roots.begin(), roots.end());
    d->searchRoots = roots;

    std::vector<QmlPluginManifest> candidates;
    QHash<QString, int> idCounts;

    for (const QString &rootPath : roots) {
        const QDir root(rootPath);
        const QFileInfoList directories = root.entryInfoList(
            QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QFileInfo &directory : directories) {
            const QString manifestPath = QDir(directory.absoluteFilePath())
                .filePath(QStringLiteral("apm-plugin.json"));
            if (!QFileInfo::exists(manifestPath)) {
                continue;
            }

            const QmlPluginManifest manifest =
                QmlPluginManifest::load(manifestPath);
            if (!manifest.isValid()) {
                d->reportError(directory.fileName(), manifest.errorString());
                continue;
            }

            candidates.push_back(manifest);
            idCounts[manifest.id()] += 1;
        }
    }

    QSet<QString> duplicateIds;
    for (auto it = idCounts.cbegin(); it != idCounts.cend(); ++it) {
        if (it.value() > 1) {
            duplicateIds.insert(it.key());
            d->reportError(it.key(), tr("Duplicate plugin id in search roots"));
        }
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const QmlPluginManifest &left,
                 const QmlPluginManifest &right) {
        return left.id() < right.id();
    });

    for (const QmlPluginManifest &manifest : candidates) {
        if (!duplicateIds.contains(manifest.id())) {
            Private::Plugin plugin;
            plugin.manifest = manifest;
            d->plugins.push_back(plugin);
        }
    }

    if (toolsMenu && !d->plugins.empty()) {
        d->pluginMenu = toolsMenu->addMenu(tr("QML Plugins"));
        d->pluginMenu->setObjectName(QStringLiteral("menuQmlPlugins"));

        for (int index = 0; index < pluginCount(); ++index) {
            Private::Plugin &plugin =
                d->plugins[static_cast<std::size_t>(index)];
            QAction *action = d->pluginMenu->addAction(
                plugin.manifest.ui().title);
            action->setObjectName(QStringLiteral("actionQmlPlugin_%1")
                                      .arg(plugin.manifest.id()));
            if (!plugin.manifest.ui().iconFilePath.isEmpty()) {
                action->setIcon(QIcon(plugin.manifest.ui().iconFilePath));
            }
            connect(action, &QAction::triggered, this,
                    [this, index]() { d->openPlugin(index); });
            plugin.action = action;
        }
    }

    emit pluginsChanged();
}

void QmlPluginManager::reload()
{
    if (d->reloadPending || d->stopped) {
        return;
    }
    d->reloadPending = true;
    const QPointer<QMenu> toolsMenu = d->toolsMenu;
    const QStringList roots = d->searchRoots;
    // A trusted plugin may call reload() from its own QML callback. Defer the
    // destructive engine/window reset until that callback has unwound.
    QTimer::singleShot(0, this, [this, toolsMenu, roots]() {
        d->reloadPending = false;
        if (!d->stopped) {
            start(toolsMenu.data(), roots);
        }
    });
}

bool QmlPluginManager::openPlugin(const QString &pluginId)
{
    for (int index = 0; index < pluginCount(); ++index) {
        if (d->plugins[static_cast<std::size_t>(index)].manifest.id()
            == pluginId) {
            return d->openPlugin(index);
        }
    }
    return false;
}

void QmlPluginManager::shutdown()
{
    d->stopped = true;
    d->reloadPending = false;
    const bool hadState = !d->plugins.empty() || !d->diagnostics.isEmpty()
        || d->pluginMenu;
    d->clear();
    if (hadState) {
        emit pluginsChanged();
    }
}
