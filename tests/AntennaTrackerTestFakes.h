#ifndef ANTENNATRACKERTESTFAKES_H
#define ANTENNATRACKERTESTFAKES_H

// Shared fakes for the antenna tracker view-model and view tests.

#include "ui/configuration/AntennaTrackerSerialService.h"
#include "ui/configuration/AntennaTrackerTelemetrySource.h"
#include "ui/configuration/AntennaTrackerUIViewModel.h"

#include <QByteArray>
#include <QPointer>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>

#include <functional>
#include <memory>

// Observations that outlive the transport object, which the service deletes.
struct FakeTrackerTransportLog
{
    QString portName;
    int baudRate = 0;
    int opens = 0;
    int closes = 0;
    int writeCalls = 0;
    QByteArray wire; // bytes accepted by write(), in order
    bool destroyed = false;
};

// Drains synchronously by default so connect() completes inside the call.
class FakeTrackerTransport final : public AntennaTrackerSerialTransport
{
public:
    FakeTrackerTransport(std::shared_ptr<FakeTrackerTransportLog> log, QObject *parent)
        : AntennaTrackerSerialTransport(parent)
        , m_log(std::move(log))
    {
    }
    ~FakeTrackerTransport() override { m_log->destroyed = true; }

    bool failOpen = false;
    QString openError = QStringLiteral("boom");
    bool syncDrain = true;

    bool open(const QString &portName, int baudRate, QString *error) override
    {
        ++m_log->opens;
        m_log->portName = portName;
        m_log->baudRate = baudRate;
        if (failOpen) {
            if (error) {
                *error = openError;
            }
            return false;
        }
        m_open = true;
        return true;
    }
    void close() override
    {
        if (m_open) {
            m_open = false;
            ++m_log->closes;
        }
    }
    bool isOpen() const override { return m_open; }
    bool discardInput() override { return m_open; }
    qint64 write(const QByteArray &data) override
    {
        ++m_log->writeCalls;
        if (!m_open) {
            return -1;
        }
        m_log->wire.append(data);
        m_pending += data.size();
        if (syncDrain) {
            drain();
        } else {
            QTimer::singleShot(0, this, [this]() { drain(); });
        }
        return data.size();
    }
    qint64 bytesToWrite() const override { return m_pending; }

    void drain()
    {
        const qint64 amount = m_pending;
        m_pending = 0;
        if (amount > 0) {
            emit bytesWritten(amount);
        }
    }
    void unplug(const QString &message) { emit errorOccurred(message); }

private:
    std::shared_ptr<FakeTrackerTransportLog> m_log;
    bool m_open = false;
    qint64 m_pending = 0;
};

class FakeTrackerTelemetry final : public AntennaTrackerTelemetrySource
{
public:
    explicit FakeTrackerTelemetry(QObject *parent = nullptr)
        : AntennaTrackerTelemetrySource(parent)
    {
    }

    AntennaTrackerVehicleFix fix;
    AntennaTrackerPosition tracker;
    bool trackerValid = false;
    double snr = 0.0;
    std::function<double()> snrProvider; // optional: SNR as a function of state

    AntennaTrackerVehicleFix vehicleFix() const override { return fix; }
    AntennaTrackerPosition trackerLocation(bool *valid) const override
    {
        if (valid) {
            *valid = trackerValid;
        }
        return tracker;
    }
    double localSnrDb() const override { return snrProvider ? snrProvider() : snr; }
};

// Everything a view-model test needs, with a private settings file.
struct TrackerFixture
{
    QTemporaryDir dir;
    std::shared_ptr<FakeTrackerTransportLog> log = std::make_shared<FakeTrackerTransportLog>();
    QPointer<FakeTrackerTransport> transport;
    int factoryCalls = 0;
    QStringList ports{QStringLiteral("/dev/ttyTRACKER0"), QStringLiteral("/dev/ttyTRACKER1")};
    AntennaTrackerSerialService *service = nullptr;
    FakeTrackerTelemetry *telemetry = nullptr;
    QSettings *settings = nullptr;
    std::unique_ptr<AntennaTrackerUIViewModel> viewModel;
    std::function<void(FakeTrackerTransport *)> configure;

    QString settingsPath() const { return dir.filePath(QStringLiteral("tracker.ini")); }

    // Builds the view model; call again (after reset) to reload the same settings file.
    AntennaTrackerUIViewModel *build()
    {
        viewModel.reset();
        service = new AntennaTrackerSerialService;
        service->setTransportFactory([this](QObject *parent) -> AntennaTrackerSerialTransport * {
            ++factoryCalls;
            auto *created = new FakeTrackerTransport(log, parent);
            if (configure) {
                configure(created);
            }
            transport = created;
            return created;
        });
        telemetry = new FakeTrackerTelemetry;
        settings = new QSettings(settingsPath(), QSettings::IniFormat);
        viewModel.reset(new AntennaTrackerUIViewModel(
            service, telemetry, settings, [this]() { return ports; }));
        settings->setParent(viewModel.get());
        return viewModel.get();
    }
};

#endif // ANTENNATRACKERTESTFAKES_H
