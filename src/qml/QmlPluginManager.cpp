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
#include <QSet>
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
        for (Plugin &plugin : plugins) {
            delete plugin.window.data();
            plugin.window = nullptr;
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

    void openPlugin(int index)
    {
        if (index < 0 || index >= static_cast<int>(plugins.size())) {
            return;
        }

        Plugin &plugin = plugins[static_cast<std::size_t>(index)];
        if (plugin.window) {
            plugin.window->show();
            plugin.window->raise();
            plugin.window->activateWindow();
            return;
        }

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
            return;
        }

        layout->addWidget(quickWidget);
        plugin.window = window;
        window->show();
    }

    QmlPluginManager *q = nullptr;
    std::unique_ptr<ApmQmlApi> api;
    std::unique_ptr<QQmlEngine> engine;
    QPointer<QMenu> pluginMenu;
    std::vector<Plugin> plugins;
    QStringList diagnostics;
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

    QStringList roots = searchRoots.isEmpty()
        ? defaultSearchRoots() : searchRoots;
    for (QString &root : roots) {
        root = QDir::cleanPath(QFileInfo(root).absoluteFilePath());
    }
    roots.removeDuplicates();
    std::sort(roots.begin(), roots.end());

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

void QmlPluginManager::shutdown()
{
    const bool hadState = !d->plugins.empty() || !d->diagnostics.isEmpty()
        || d->pluginMenu;
    d->clear();
    if (hadState) {
        emit pluginsChanged();
    }
}
