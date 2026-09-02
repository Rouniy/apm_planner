#include <QtTest>

#include "ui/configuration/ConfigDefaultSettingsView.h"
#include "ui/configuration/ConfigRawParams.h"
#include "ui/configuration/FrameDefaultCatalogService.h"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>

#include <cstring>
#include <functional>

namespace {

const QString kApiRoot = QStringLiteral("http://catalog.test/contents/");
const QString kRawRoot = QStringLiteral("http://raw.test/master/");
const QString kTailsitter = QStringLiteral("Tools/Frame_params/QuadPlanes/Tailsitter.param");
const QString kCopter = QStringLiteral("Tools/Frame_params/Copter.param");

// Minimal in-memory reply: delivers its body on the next event-loop turn or
// never (Hang). The full transport behaviour is covered by the c2 suite.
class FakeReply final : public QNetworkReply
{
public:
    enum Mode { Immediate, Hang };

    FakeReply(const QNetworkRequest &request, int status, const QByteArray &body,
              Mode mode, QObject *parent)
        : QNetworkReply(parent), m_body(body), m_status(status)
    {
        setRequest(request);
        setUrl(request.url());
        setOperation(QNetworkAccessManager::GetOperation);
        QIODevice::open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
        setAttribute(QNetworkRequest::HttpReasonPhraseAttribute,
                     status == 200 ? QStringLiteral("OK") : QStringLiteral("Not Found"));
        setHeader(QNetworkRequest::ContentLengthHeader, body.size());
        if (status >= 400) {
            setError(QNetworkReply::ContentNotFoundError, QStringLiteral("Not Found"));
        }
        if (mode == Immediate) {
            QTimer::singleShot(0, this, &FakeReply::deliver);
        }
    }

    void abort() override
    {
        if (m_finished) {
            return;
        }
        m_finished = true;
        setError(QNetworkReply::OperationCanceledError, QStringLiteral("Operation canceled"));
        emit finished();
    }

    qint64 bytesAvailable() const override
    {
        return static_cast<qint64>(m_delivered - m_offset) + QIODevice::bytesAvailable();
    }

    bool isSequential() const override { return true; }

protected:
    qint64 readData(char *data, qint64 maxSize) override
    {
        const qint64 available = m_delivered - m_offset;
        if (available <= 0) {
            return m_finished ? -1 : 0;
        }
        const qint64 count = qMin(maxSize, available);
        std::memcpy(data, m_body.constData() + m_offset, static_cast<size_t>(count));
        m_offset += static_cast<int>(count);
        return count;
    }

private:
    void deliver()
    {
        if (m_finished) {
            return;
        }
        m_delivered = m_body.size();
        m_finished = true;
        emit readyRead();
        emit finished();
    }

    QByteArray m_body;
    int m_status;
    int m_delivered = 0;
    int m_offset = 0;
    bool m_finished = false;
};

class FakeNetworkAccessManager final : public QNetworkAccessManager
{
public:
    struct Response
    {
        int status = 404;
        QByteArray body;
        FakeReply::Mode mode = FakeReply::Immediate;
    };

    using QNetworkAccessManager::QNetworkAccessManager;

    QHash<QString, Response> responses;
    QVector<QUrl> requests;

    int requestsFor(const QString &fragment) const
    {
        int count = 0;
        for (const QUrl &url : requests) {
            if (url.toString(QUrl::FullyEncoded).contains(fragment)) {
                ++count;
            }
        }
        return count;
    }

protected:
    QNetworkReply *createRequest(Operation operation, const QNetworkRequest &request,
                                 QIODevice *outgoingData) override
    {
        Q_UNUSED(operation);
        Q_UNUSED(outgoingData);
        requests.append(request.url());
        const Response response = responses.value(request.url().toString(QUrl::FullyEncoded));
        return new FakeReply(request, response.status, response.body, response.mode, this);
    }
};

FakeNetworkAccessManager::Response ok(const QByteArray &body,
                                      FakeReply::Mode mode = FakeReply::Immediate)
{
    FakeNetworkAccessManager::Response response;
    response.status = 200;
    response.body = body;
    response.mode = mode;
    return response;
}

void installCatalog(FakeNetworkAccessManager &fake)
{
    fake.responses.insert(kApiRoot + QStringLiteral("Tools/Frame_params"), ok(R"([
        {"name":"Copter.param","path":"Tools/Frame_params/Copter.param","type":"file"},
        {"name":"README.md","path":"Tools/Frame_params/README.md","type":"file"},
        {"name":"QuadPlanes","path":"Tools/Frame_params/QuadPlanes","type":"dir"}
    ])"));
    fake.responses.insert(kApiRoot + QStringLiteral("Tools/Frame_params/QuadPlanes"), ok(R"([
        {"name":"Tailsitter.param","path":"Tools/Frame_params/QuadPlanes/Tailsitter.param","type":"file"}
    ])"));
    fake.responses.insert(kRawRoot + kCopter, ok(QByteArrayLiteral("GAIN,2.5\nUNKNOWN,1\n")));
    fake.responses.insert(kRawRoot + kTailsitter, ok(QByteArrayLiteral("GAIN,7\n")));
}

ParameterRecord record(const QString &name, const QVariant &value, ParameterType type,
                       int component = 1)
{
    ParameterRecord result;
    result.key.componentId = static_cast<quint8>(component);
    result.key.name = name;
    result.value = value;
    result.type = type;
    return result;
}

QList<ParameterRecord> vehicleSnapshot()
{
    return {record(QStringLiteral("GAIN"), 1.0, ParameterType::Real32),
            record(QStringLiteral("MODE"), 2, ParameterType::UInt8)};
}

// Closes ConfigRawParams' modal Compare Params dialog as soon as it appears
// and counts how often it was shown.
class DialogCloser final : public QObject
{
public:
    explicit DialogCloser(const QString &title,
                          std::function<void()> beforeClose = {},
                          QObject *parent = nullptr)
        : QObject(parent), m_title(title), m_beforeClose(std::move(beforeClose))
    {
        m_timer.setInterval(10);
        connect(&m_timer, &QTimer::timeout, this, &DialogCloser::poll);
        m_timer.start();
    }

    int seen() const { return m_seen; }

private:
    void poll()
    {
        const QWidgetList widgets = QApplication::topLevelWidgets();
        for (QWidget *widget : widgets) {
            auto *dialog = qobject_cast<QDialog *>(widget);
            if (dialog && dialog->isVisible() && dialog->windowTitle() == m_title) {
                ++m_seen;
                if (m_beforeClose) {
                    std::function<void()> action = std::move(m_beforeClose);
                    action();
                }
                dialog->reject();
            }
        }
    }

    QString m_title;
    std::function<void()> m_beforeClose;
    QTimer m_timer;
    int m_seen = 0;
};

struct Fixture
{
    FakeNetworkAccessManager network;
    FrameDefaultCatalogService service;
    QTemporaryDir cacheDirectory;
    ConfigDefaultSettingsView page;
    QSignalSpy writeRequested;
    QSignalSpy statusChanged;

    Fixture()
        : service(&network, QUrl(kApiRoot), QUrl(kRawRoot)),
          page(&service),
          writeRequested(page.rawParams(), &ConfigRawParams::writeRequested),
          statusChanged(&page, &ConfigDefaultSettingsView::statusChanged)
    {
        installCatalog(network);
        page.setCacheRoot(cacheDirectory.path());
        page.resize(1000, 700);
    }

    QComboBox *combo() const { return page.findChild<QComboBox *>(QStringLiteral("FrameDefaults")); }
    QPushButton *compare() const
    {
        return page.findChild<QPushButton *>(QStringLiteral("LoadFrameDefaultsBtn"));
    }
    QPushButton *refresh() const
    {
        return page.findChild<QPushButton *>(QStringLiteral("RefreshFrameDefaultsBtn"));
    }
    QString cachePath(const QString &catalogPath) const
    {
        return FrameDefaultCatalogService::GetCachePath(cacheDirectory.path(), catalogPath);
    }
    bool cacheExists(const QString &catalogPath) const
    {
        return QFile::exists(cachePath(catalogPath));
    }
    void connectVehicle()
    {
        page.setConnected(true);
        page.setParameterSnapshot(vehicleSnapshot(), 1);
    }
};

} // namespace

class ConfigDefaultSettingsViewTest final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void inventoryMatchesMissionPlanner();
    void activateAutoLoadsCatalogAndSelectsFirst();
    void busyStateDisablesRowUntilCompletion();
    void reportsCatalogErrorsAndEmptyLists();
    void compareRequiresSelectionAndConnection();
    void downloadWritesCacheAndHandsFileToRawParams();
    void targetChangeDuringReviewDoesNotStageOrReportSuccess();
    void targetChangeDisconnectAndDeactivateDiscardDownloads();
    void foreignServiceResultsAreIgnored();
    void joinedCatalogDetachDoesNotCancelItsOwner();
    void pageCancellationDoesNotCrossOperationKinds();
    void deactivateCancelsOwnedCatalogLoad();
    void serviceDestructionRestoresIdleControls();
    void worksWithoutService();
};

void ConfigDefaultSettingsViewTest::initTestCase()
{
    QCoreApplication::setOrganizationName(QStringLiteral("APMPlannerTests"));
    QCoreApplication::setApplicationName(QStringLiteral("ConfigDefaultSettingsView"));
}

void ConfigDefaultSettingsViewTest::init()
{
    QSettings settings;
    settings.clear();
    settings.sync();
}

void ConfigDefaultSettingsViewTest::inventoryMatchesMissionPlanner()
{
    Fixture fixture;
    ConfigDefaultSettingsView &page = fixture.page;
    QCOMPARE(page.objectName(), QStringLiteral("ConfigDefaultSettingsView"));

    auto *title = page.findChild<QLabel *>(QStringLiteral("DefaultSettingsTitle"));
    QVERIFY(title);
    QCOMPARE(title->text(), QStringLiteral("Default Settings"));
    auto *warning = page.findChild<QLabel *>(QStringLiteral("DefaultSettingsWarning"));
    QVERIFY(warning);
    QCOMPARE(warning->text(), QStringLiteral(
        "Choose an official ArduPilot profile from Tools/Frame_params. The profile is "
        "compared only with the currently selected vehicle. Selected differences are "
        "staged for review and are not sent until you explicitly choose Write Params "
        "and confirm the write."));
    QVERIFY(warning->wordWrap());
    QCOMPARE(warning->maximumWidth(), 900);

    auto *rowLabel = page.findChild<QLabel *>(QStringLiteral("FrameDefaultsLabel"));
    QVERIFY(rowLabel);
    QCOMPARE(rowLabel->text(), QStringLiteral("ArduPilot frame defaults"));
    QVERIFY(fixture.combo());
    QCOMPARE(fixture.combo()->minimumWidth(), 360); // MP10 ColumnDefinition 360
    QCOMPARE(fixture.combo()->maximumWidth(), 360);
    QCOMPARE(fixture.combo()->count(), 0);
    QVERIFY(fixture.compare());
    QCOMPARE(fixture.compare()->text(), QStringLiteral("Compare / Stage…"));
    QVERIFY(fixture.refresh());
    QCOMPARE(fixture.refresh()->text(), QStringLiteral("Load / refresh list"));
    QVERIFY(page.findChild<QLabel *>(QStringLiteral("FrameDefaultsStatus")));

    QVERIFY(page.rawParams());
    QVERIFY(page.rawParams()->parent() == &page);
    QCOMPARE(page.rawParams()->objectName(), QStringLiteral("ConfigRawParams"));
    QVERIFY(page.findChild<QWidget *>(QStringLiteral("ConfigRawParams")));

    QVERIFY(!page.isActive());
    QVERIFY(!page.isBusy());
    QVERIFY(!page.isConnected());
    QVERIFY(page.statusText().isEmpty());
    QCOMPARE(page.cacheRoot(), fixture.cacheDirectory.path());
    QVERIFY(fixture.combo()->isEnabled());
    QVERIFY(fixture.compare()->isEnabled());
    QVERIFY(fixture.refresh()->isEnabled());
    QCOMPARE(fixture.network.requests.size(), 0); // nothing happens before activate()
}

void ConfigDefaultSettingsViewTest::activateAutoLoadsCatalogAndSelectsFirst()
{
    Fixture fixture;
    ConfigDefaultSettingsView &page = fixture.page;

    page.activate();
    QVERIFY(page.isActive());
    QVERIFY(page.isBusy());
    QTRY_VERIFY(!page.isBusy());
    QCOMPARE(page.frameDefaultCount(), 2);
    QCOMPARE(fixture.combo()->count(), 2);
    QCOMPARE(fixture.combo()->currentIndex(), 0);
    QCOMPARE(fixture.combo()->itemText(0), QStringLiteral("Copter.param"));
    QCOMPARE(fixture.combo()->itemText(1), QStringLiteral("QuadPlanes / Tailsitter.param"));
    QCOMPARE(page.selectedFrameDefaultPath(), kCopter);
    QCOMPARE(page.statusText(), QStringLiteral("Loaded 2 ArduPilot frame-default file choices."));
    QCOMPARE(fixture.network.requestsFor(QStringLiteral("/contents/")), 2);

    // A second activation reuses the list without any request.
    page.deactivate();
    page.activate();
    QVERIFY(!page.isBusy());
    QCOMPARE(fixture.network.requestsFor(QStringLiteral("/contents/")), 2);

    // The memoised service catalog is delivered synchronously to a new page.
    ConfigDefaultSettingsView second(&fixture.service);
    second.activate();
    QVERIFY(!second.isBusy());
    QCOMPARE(second.frameDefaultCount(), 2);
    QCOMPARE(fixture.network.requestsFor(QStringLiteral("/contents/")), 2);

    // "Load / refresh list" forces a new walk and keeps the first choice.
    fixture.refresh()->click();
    QVERIFY(page.isBusy());
    QTRY_VERIFY(!page.isBusy());
    QCOMPARE(fixture.network.requestsFor(QStringLiteral("/contents/")), 4);
    QCOMPARE(fixture.combo()->count(), 2);
    QCOMPARE(fixture.combo()->currentIndex(), 0);
    QCOMPARE(page.statusText(), QStringLiteral("Loaded 2 ArduPilot frame-default file choices."));
    QCOMPARE(fixture.writeRequested.count(), 0);
}

void ConfigDefaultSettingsViewTest::busyStateDisablesRowUntilCompletion()
{
    Fixture fixture;
    fixture.network.responses.insert(kApiRoot + QStringLiteral("Tools/Frame_params"),
                                     ok(QByteArrayLiteral("[]"), FakeReply::Hang));
    ConfigDefaultSettingsView &page = fixture.page;

    page.activate();
    QVERIFY(page.isBusy());
    QVERIFY(!fixture.combo()->isEnabled());
    QVERIFY(!fixture.compare()->isEnabled());
    QVERIFY(!fixture.refresh()->isEnabled());
    QCOMPARE(page.statusText(), QStringLiteral("Loading the ArduPilot frame-default list…"));

    // Clicks while busy are ignored.
    fixture.refresh()->click();
    fixture.compare()->click();
    QCOMPARE(fixture.network.requests.size(), 1);

    fixture.service.cancel();
    QVERIFY(!page.isBusy());
    QVERIFY(fixture.combo()->isEnabled());
    QVERIFY(fixture.compare()->isEnabled());
    QVERIFY(fixture.refresh()->isEnabled());
    QCOMPARE(page.statusText(), QStringLiteral("Frame-default list request cancelled."));
}

void ConfigDefaultSettingsViewTest::reportsCatalogErrorsAndEmptyLists()
{
    Fixture fixture;
    ConfigDefaultSettingsView &page = fixture.page;
    FakeNetworkAccessManager::Response notFound;
    fixture.network.responses.insert(kApiRoot + QStringLiteral("Tools/Frame_params"), notFound);

    page.activate();
    QTRY_VERIFY(!page.isBusy());
    QVERIFY2(page.statusText().startsWith(QStringLiteral("Frame-default list failed: ")),
             qPrintable(page.statusText()));
    QVERIFY2(page.statusText().contains(QStringLiteral("HTTP 404")), qPrintable(page.statusText()));
    QCOMPARE(page.frameDefaultCount(), 0);
    QVERIFY(fixture.refresh()->isEnabled());

    fixture.network.responses.insert(kApiRoot + QStringLiteral("Tools/Frame_params"),
                                     ok(QByteArrayLiteral("[]")));
    fixture.refresh()->click();
    QTRY_VERIFY(!page.isBusy());
    QCOMPARE(page.statusText(), QStringLiteral("No ArduPilot frame-default files were found."));
    QCOMPARE(fixture.combo()->count(), 0);
    QCOMPARE(page.selectedFrameDefaultPath(), QString());

    // Like MP10, re-activation reuses the memoised (empty) catalog; only
    // "Load / refresh list" walks the catalog again.
    installCatalog(fixture.network);
    page.deactivate();
    page.activate();
    QVERIFY(!page.isBusy());
    QCOMPARE(page.frameDefaultCount(), 0);
    fixture.refresh()->click();
    QTRY_VERIFY(!page.isBusy());
    QCOMPARE(page.frameDefaultCount(), 2);
    QCOMPARE(fixture.combo()->currentIndex(), 0);
}

void ConfigDefaultSettingsViewTest::compareRequiresSelectionAndConnection()
{
    Fixture fixture;
    ConfigDefaultSettingsView &page = fixture.page;

    fixture.compare()->click();
    QCOMPARE(page.statusText(), QStringLiteral("Select a frame-default file first."));
    QVERIFY(!page.isBusy());

    page.activate();
    QTRY_VERIFY(!page.isBusy());
    fixture.compare()->click();
    QCOMPARE(page.statusText(), QStringLiteral("Connect a vehicle before comparing a default profile."));
    QVERIFY(!page.isBusy());
    QCOMPARE(fixture.network.requestsFor(QStringLiteral("/master/")), 0);
    QVERIFY(!fixture.cacheExists(kCopter));
    QCOMPARE(fixture.writeRequested.count(), 0);
}

void ConfigDefaultSettingsViewTest::downloadWritesCacheAndHandsFileToRawParams()
{
    Fixture fixture;
    ConfigDefaultSettingsView &page = fixture.page;
    page.activate();
    QTRY_VERIFY(!page.isBusy());
    fixture.connectVehicle();
    QVERIFY(page.isConnected());
    QCOMPARE(page.rawParams()->parameterCount(), 2);
    fixture.combo()->setCurrentIndex(1);
    QCOMPARE(page.selectedFrameDefaultPath(), kTailsitter);

    DialogCloser closer(QStringLiteral("Compare Params"));
    fixture.compare()->click();
    QVERIFY(page.isBusy());
    QCOMPARE(page.statusText(), QStringLiteral("Downloading QuadPlanes / Tailsitter.param…"));
    QVERIFY(!fixture.combo()->isEnabled());
    QTRY_VERIFY(!page.isBusy());

    // The profile was written atomically below the page cache root...
    QVERIFY(fixture.cacheExists(kTailsitter));
    QFile file(fixture.cachePath(kTailsitter));
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArrayLiteral("GAIN,7\n"));
    QCOMPARE(fixture.network.requestsFor(QStringLiteral("/master/")), 1);
    QVERIFY(fixture.network.requests.last().toString(QUrl::FullyEncoded).endsWith(kTailsitter));

    // ...and handed to ConfigRawParams' Compare Params dialog (rejected by the
    // test): nothing is staged and nothing is ever written by this page.
    QTRY_VERIFY(closer.seen() >= 1);
    QCOMPARE(closer.seen(), 1);
    QCOMPARE(page.rawParams()->stagedParameterCount(), 0);
    QCOMPARE(fixture.writeRequested.count(), 0);
    QVERIFY2(page.statusText().startsWith(QStringLiteral("QuadPlanes / Tailsitter.param: ")),
             qPrintable(page.statusText()));
    QVERIFY(fixture.combo()->isEnabled());
    QVERIFY(fixture.compare()->isEnabled());
    QCOMPARE(fixture.combo()->currentIndex(), 1);

    // A download HTTP error is reported for the exact profile.
    fixture.network.responses.remove(kRawRoot + kCopter);
    fixture.combo()->setCurrentIndex(0);
    fixture.compare()->click();
    QTRY_VERIFY(!page.isBusy());
    QVERIFY2(page.statusText().startsWith(QStringLiteral("Frame-default download failed: ")),
             qPrintable(page.statusText()));
    QVERIFY2(page.statusText().contains(QStringLiteral("HTTP 404")), qPrintable(page.statusText()));
    QVERIFY(!fixture.cacheExists(kCopter));
    QCOMPARE(closer.seen(), 1);
}

void ConfigDefaultSettingsViewTest::targetChangeDisconnectAndDeactivateDiscardDownloads()
{
    Fixture fixture;
    fixture.network.responses.insert(kRawRoot + kCopter,
                                     ok(QByteArrayLiteral("GAIN,9\n"), FakeReply::Hang));
    ConfigDefaultSettingsView &page = fixture.page;
    page.activate();
    QTRY_VERIFY(!page.isBusy());
    fixture.connectVehicle();
    DialogCloser closer(QStringLiteral("Compare Params"));

    // Target switch during the download.
    const quint64 revision = page.targetRevision();
    fixture.compare()->click();
    QVERIFY(page.isBusy());
    page.parameterTargetChanged();
    QVERIFY(page.targetRevision() > revision);
    QVERIFY(!page.isBusy());
    QCOMPARE(page.statusText(),
             QStringLiteral("Vehicle changed during download; the old profile was discarded."));
    QVERIFY(!fixture.service.isDownloading());
    QCOMPARE(page.rawParams()->parameterCount(), 0); // forwarded to ConfigRawParams

    // A new snapshot invalidates the captured revision before RawParams sees it.
    fixture.connectVehicle();
    fixture.compare()->click();
    QVERIFY(page.isBusy());
    page.setParameterSnapshot(vehicleSnapshot(), 1);
    QVERIFY(!page.isBusy());
    QCOMPARE(page.statusText(), QStringLiteral(
        "Vehicle changed while the profile was downloading; the old result was discarded."));
    QCOMPARE(page.rawParams()->parameterCount(), 2);

    // Disconnect during the download.
    fixture.compare()->click();
    QVERIFY(page.isBusy());
    page.setConnected(false);
    QVERIFY(!page.isBusy());
    QCOMPARE(page.statusText(), QStringLiteral(
        "Frame-default download discarded because the vehicle disconnected."));
    QVERIFY(!page.isConnected());

    // Deactivation during the download.
    page.setConnected(true);
    fixture.compare()->click();
    QVERIFY(page.isBusy());
    page.deactivate();
    QVERIFY(!page.isActive());
    QVERIFY(!page.isBusy());
    QCOMPARE(page.statusText(), QStringLiteral("Frame-default download cancelled."));
    QVERIFY(!fixture.service.isDownloading());

    QTest::qWait(20);
    QVERIFY(!fixture.cacheExists(kCopter));
    QCOMPARE(closer.seen(), 0);
    QCOMPARE(page.rawParams()->stagedParameterCount(), 0);
    QCOMPARE(fixture.writeRequested.count(), 0);
    QVERIFY(fixture.combo()->isEnabled());
    QVERIFY(fixture.compare()->isEnabled());
}

void ConfigDefaultSettingsViewTest::targetChangeDuringReviewDoesNotStageOrReportSuccess()
{
    Fixture fixture;
    ConfigDefaultSettingsView &page = fixture.page;
    page.activate();
    QTRY_VERIFY(!page.isBusy());
    fixture.connectVehicle();

    DialogCloser closer(QStringLiteral("Compare Params"), [&page]() {
        page.parameterTargetChanged();
    });
    fixture.compare()->click();
    QTRY_VERIFY(closer.seen() >= 1);

    QCOMPARE(page.rawParams()->parameterCount(), 0);
    QCOMPARE(page.rawParams()->stagedParameterCount(), 0);
    QCOMPARE(fixture.writeRequested.count(), 0);
    QCOMPARE(page.statusText(), QStringLiteral(
        "Vehicle or page changed while the profile was being reviewed; no "
        "profile values were staged."));
}

void ConfigDefaultSettingsViewTest::foreignServiceResultsAreIgnored()
{
    Fixture fixture;
    ConfigDefaultSettingsView &page = fixture.page;
    page.activate();
    QTRY_VERIFY(!page.isBusy());
    fixture.connectVehicle();
    DialogCloser closer(QStringLiteral("Compare Params"));
    const QString status = page.statusText();
    const int statusCount = fixture.statusChanged.count();

    // Another consumer of the shared service downloads a profile: the page
    // must not react (no busy state, no cache file, no dialog).
    QSignalSpy finished(&fixture.service, &FrameDefaultCatalogService::downloadFinished);
    QVERIFY(fixture.service.download(kCopter));
    QVERIFY(!page.isBusy());
    QVERIFY(fixture.combo()->isEnabled());
    QTRY_COMPARE(finished.count(), 1);
    QTest::qWait(20);
    QCOMPARE(page.statusText(), status);
    QCOMPARE(fixture.statusChanged.count(), statusCount);
    QVERIFY(!fixture.cacheExists(kCopter));
    QCOMPARE(closer.seen(), 0);

    // A foreign catalog refresh updates the list of an active page but a
    // hidden page keeps its state.
    QSignalSpy ready(&fixture.service, &FrameDefaultCatalogService::catalogReady);
    QVERIFY(fixture.service.requestCatalog(true));
    QTRY_COMPARE(ready.count(), 1);
    QCOMPARE(page.frameDefaultCount(), 2);
    page.deactivate();
    fixture.network.responses.insert(kApiRoot + QStringLiteral("Tools/Frame_params"),
                                     ok(QByteArrayLiteral("[]")));
    QVERIFY(fixture.service.requestCatalog(true));
    QTRY_COMPARE(ready.count(), 2);
    QCOMPARE(page.frameDefaultCount(), 2);
    QCOMPARE(fixture.combo()->count(), 2);
}

void ConfigDefaultSettingsViewTest::joinedCatalogDetachDoesNotCancelItsOwner()
{
    FakeNetworkAccessManager network;
    network.responses.insert(kApiRoot + QStringLiteral("Tools/Frame_params"),
                             ok(QByteArrayLiteral("[]"), FakeReply::Hang));
    FrameDefaultCatalogService service(
        &network, QUrl(kApiRoot), QUrl(kRawRoot));
    bool started = false;
    FrameDefaultCatalogService::OperationId ownerId =
        FrameDefaultCatalogService::InvalidOperationId;
    QVERIFY(service.requestCatalog(false, nullptr, &started, &ownerId));
    QVERIFY(started);

    ConfigDefaultSettingsView page(&service);
    page.activate(); // joins the foreign walk
    QVERIFY(page.isBusy());
    page.deactivate();

    QVERIFY(!page.isBusy());
    QVERIFY(service.isCatalogLoading());
    QVERIFY(service.cancelCatalog(ownerId));
}

void ConfigDefaultSettingsViewTest::pageCancellationDoesNotCrossOperationKinds()
{
    Fixture fixture;
    ConfigDefaultSettingsView &page = fixture.page;

    // Hiding a page-owned catalog walk must not cancel a foreign download.
    fixture.network.responses.insert(kApiRoot + QStringLiteral("Tools/Frame_params"),
                                     ok(QByteArrayLiteral("[]"), FakeReply::Hang));
    fixture.network.responses.insert(kRawRoot + kCopter,
                                     ok(QByteArrayLiteral("GAIN,3\n"), FakeReply::Hang));
    page.activate();
    QVERIFY(page.isBusy());
    FrameDefaultCatalogService::OperationId foreignDownload =
        FrameDefaultCatalogService::InvalidOperationId;
    QVERIFY(fixture.service.download(kCopter, nullptr, &foreignDownload));
    page.deactivate();
    QVERIFY(!fixture.service.isCatalogLoading());
    QVERIFY(fixture.service.isDownloading());
    QVERIFY(fixture.service.cancelDownload(foreignDownload));

    // Cancelling a page-owned download on target change must leave a foreign
    // catalog walk alive.
    installCatalog(fixture.network);
    page.activate();
    QTRY_VERIFY(!page.isBusy());
    fixture.connectVehicle();
    fixture.network.responses.insert(kApiRoot + QStringLiteral("Tools/Frame_params"),
                                     ok(QByteArrayLiteral("[]"), FakeReply::Hang));
    bool catalogStarted = false;
    FrameDefaultCatalogService::OperationId foreignCatalog =
        FrameDefaultCatalogService::InvalidOperationId;
    QVERIFY(fixture.service.requestCatalog(
        true, nullptr, &catalogStarted, &foreignCatalog));
    QVERIFY(catalogStarted);
    fixture.network.responses.insert(kRawRoot + kCopter,
                                     ok(QByteArrayLiteral("GAIN,4\n"), FakeReply::Hang));
    fixture.compare()->click();
    QVERIFY(page.isBusy());
    page.parameterTargetChanged();
    QVERIFY(!fixture.service.isDownloading());
    QVERIFY(fixture.service.isCatalogLoading());
    QVERIFY(fixture.service.cancelCatalog(foreignCatalog));
}

void ConfigDefaultSettingsViewTest::deactivateCancelsOwnedCatalogLoad()
{
    Fixture fixture;
    fixture.network.responses.insert(kApiRoot + QStringLiteral("Tools/Frame_params"),
                                     ok(QByteArrayLiteral("[]"), FakeReply::Hang));
    ConfigDefaultSettingsView &page = fixture.page;

    page.activate();
    QVERIFY(page.isBusy());
    QVERIFY(fixture.service.isCatalogLoading());
    page.deactivate();
    QVERIFY(!page.isBusy());
    QVERIFY(!fixture.service.isCatalogLoading());
    QCOMPARE(page.statusText(), QStringLiteral("Frame-default list request cancelled."));
    QCOMPARE(fixture.combo()->count(), 0);

    installCatalog(fixture.network);
    page.activate();
    QVERIFY(page.isBusy());
    QTRY_VERIFY(!page.isBusy());
    QCOMPARE(page.frameDefaultCount(), 2);
    QCOMPARE(fixture.network.requestsFor(QStringLiteral("/contents/")), 3);
}

void ConfigDefaultSettingsViewTest::serviceDestructionRestoresIdleControls()
{
    FakeNetworkAccessManager network;
    installCatalog(network);
    network.responses.insert(kApiRoot + QStringLiteral("Tools/Frame_params"),
                             ok(QByteArrayLiteral("[]"), FakeReply::Hang));
    auto *service = new FrameDefaultCatalogService(
        &network, QUrl(kApiRoot), QUrl(kRawRoot));
    ConfigDefaultSettingsView page(service);

    page.activate();
    QVERIFY(page.isBusy());
    delete service;

    QVERIFY(!page.isBusy());
    QCOMPARE(page.statusText(), QStringLiteral(
        "The frame-default catalog service is unavailable."));
    QVERIFY(page.findChild<QComboBox *>(
        QStringLiteral("FrameDefaults"))->isEnabled());
    QVERIFY(page.findChild<QPushButton *>(
        QStringLiteral("LoadFrameDefaultsBtn"))->isEnabled());
    QVERIFY(page.findChild<QPushButton *>(
        QStringLiteral("RefreshFrameDefaultsBtn"))->isEnabled());
}

void ConfigDefaultSettingsViewTest::worksWithoutService()
{
    ConfigDefaultSettingsView page(nullptr);
    QVERIFY(page.rawParams());
    page.activate();
    QVERIFY(!page.isBusy());
    QCOMPARE(page.statusText(),
             QStringLiteral("Frame-default list failed: the catalog service is unavailable."));
    page.deactivate();
    page.parameterTargetChanged();
    page.setConnected(true);
    page.setParameterSnapshot(vehicleSnapshot(), 1);
    QCOMPARE(page.rawParams()->parameterCount(), 2);
    QVERIFY(!page.isBusy());
}

QTEST_MAIN(ConfigDefaultSettingsViewTest)
#include "test_configdefaultsettingsview.moc"
