#include "MainWindow.h"
#include "LogIndexWindow.h"
#include "Loghandling/LogAnalysis.h"
#include "Loghandling/LogIndexTileCache.h"
#include "globalobject.h"
#include "map/MapTileSourceFactory.h"
#include "pureimagecache.h"

#include <QAction>

void MainWindow::showLogIndex()
{
    if (aboutToCloseFlag) return;
    if (m_logIndexWindow && m_logIndexWindow->isClosing()) closeLogIndex();
    if (!m_logIndexWindow) {
        auto *index = new LogIndexWindow(this);
        m_logIndexWindow = index;
        index->setDefaultDirectories(GlobalObject::sharedInstance()->logDirectory(),
                                    GlobalObject::sharedInstance()->MAVLinkLogDirectory());
        index->setTileReaderFactory([] {
            auto *maps = MapTileSourceFactory::instance();
            const auto type = maps->CurrentMapType();
            if (!MapTileSourceFactory::CacheableMapTypes().contains(type))
                return LogIndex::TileReader{};
            return LogIndexTileCache::reader(core::PureImageCache::sharedCacheRoot(), type);
        });
        connect(index, &LogIndexWindow::openLogRequested, this, [this](const QString &path) {
            if (aboutToCloseFlag) return;
            auto *browser = new LogAnalysis(this);
            browser->setWindowFlags(Qt::Window);
            browser->setAttribute(Qt::WA_DeleteOnClose);
            connect(browser, &LogAnalysis::logIndexRequested,
                    this, &MainWindow::showLogIndex);
            browser->show();
            browser->raise();
            browser->activateWindow();
            browser->loadLog(path);
        });
    }
    m_logIndexWindow->show();
    m_logIndexWindow->raise();
    m_logIndexWindow->activateWindow();
}

void MainWindow::closeLogIndex()
{
    if (m_logIndexWindow) {
        delete m_logIndexWindow.data();
        m_logIndexWindow.clear();
    }
}
