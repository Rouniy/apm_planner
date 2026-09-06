#include "JoystickDevice.h"

#include <QMap>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <SDL.h>

namespace {
bool sameDevices(const QVector<JoystickDevice::Info> &a, const QVector<JoystickDevice::Info> &b)
{
    if (a.size() != b.size()) return false;
    for (int i = 0; i < a.size(); ++i)
        if (a[i].instanceId != b[i].instanceId || a[i].id != b[i].id || a[i].name != b[i].name
                || a[i].axes != b[i].axes || a[i].buttons != b[i].buttons || a[i].hats != b[i].hats) return false;
    return true;
}
}

JoystickDevice::JoystickDevice(QObject *parent) : QObject(parent), m_timer(new QTimer(this))
{
    qRegisterMetaType<JoystickDevice::Snapshot>();
    m_clock.start();
    // Qt, not SDL, owns the application windows. Input must remain available
    // when the user returns to DATA or another native Qt page.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    m_sdlOwned = SDL_InitSubSystem(SDL_INIT_JOYSTICK) == 0;
    if (!m_sdlOwned) m_error = QString::fromUtf8(SDL_GetError());
    m_timer->setInterval(20);
    connect(m_timer, &QTimer::timeout, this, &JoystickDevice::poll);
    if (m_sdlOwned) { refresh(); m_timer->start(); }
}
JoystickDevice::~JoystickDevice()
{
    m_timer->stop(); closeHandle();
    if (m_sdlOwned) SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
}
bool JoystickDevice::ownerThread() const { return QThread::currentThread() == thread(); }
void JoystickDevice::closeHandle()
{
    if (m_handle) { SDL_JoystickClose(m_handle); m_handle = nullptr; }
    m_calibrating = false; m_observed.clear();
}
void JoystickDevice::refresh()
{
    if (!ownerThread() || !m_sdlOwned) return;
    SDL_JoystickUpdate();
    QVector<Info> found; QMap<QString, int> duplicates;
    const int count = SDL_NumJoysticks();
    if (count < 0) { m_error = QString::fromUtf8(SDL_GetError()); return; }
    // A practical finite device list; every admitted device retains arbitrary
    // physical axes/buttons/hats up to the explicit 128-control profile limit.
    for (int i = 0; i < qMin(count, 128); ++i) {
        SDL_Joystick *handle = SDL_JoystickOpen(i);
        if (!handle) continue;
        Info info; info.instanceId = SDL_JoystickInstanceID(handle);
        const char *name = SDL_JoystickName(handle);
        info.name = name ? QString::fromUtf8(name) : QStringLiteral("Unnamed joystick");
        char guid[33]; SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(handle), guid, sizeof(guid));
        QString key = QString::fromLatin1(guid) + ':' + info.name;
#if SDL_VERSION_ATLEAST(2, 0, 14)
        const char *serial = SDL_JoystickGetSerial(handle);
        if (serial && *serial) key += ':' + QString::fromUtf8(serial);
#endif
        const int duplicate = duplicates[key]++;
        info.id = key + ':' + QString::number(duplicate);
        info.axes = SDL_JoystickNumAxes(handle); info.buttons = SDL_JoystickNumButtons(handle); info.hats = SDL_JoystickNumHats(handle);
        SDL_JoystickClose(handle);
        if (info.instanceId < 0 || info.axes < 0 || info.buttons < 0 || info.hats < 0
                || info.axes > 128 || info.buttons > 128 || info.hats > 128) {
            m_error = QStringLiteral("A joystick exceeds the supported 128 axes/buttons/hats limit."); continue;
        }
        found.append(info);
    }
    m_lastRefresh = m_clock.elapsed();
    if (sameDevices(found, m_devices)) return;
    m_devices = found;
    emit devicesChanged();
}
bool JoystickDevice::selectDevice(qint32 instanceId, QString *error)
{
    if (error) error->clear();
    if (!ownerThread() || !m_sdlOwned) { if (error) *error = m_error.isEmpty() ? QStringLiteral("Joystick backend unavailable on this thread.") : m_error; return false; }
    if (m_handle && m_selected.instanceId == instanceId && SDL_JoystickGetAttached(m_handle)) return true;
    const QPointer<JoystickDevice> guard(this);
    refresh(); if (!guard) return false;
    Info selected; bool found = false;
    for (const auto &info : m_devices) if (info.instanceId == instanceId) { selected = info; found = true; break; }
    if (!found) { m_error = QStringLiteral("Selected joystick is no longer present."); if (error) *error = m_error; return false; }
    SDL_Joystick *handle = nullptr;
    for (int i = 0; i < SDL_NumJoysticks(); ++i)
        if (SDL_JoystickGetDeviceInstanceID(i) == instanceId) { handle = SDL_JoystickOpen(i); break; }
    if (!handle || !SDL_JoystickGetAttached(handle)) {
        if (handle) SDL_JoystickClose(handle);
        m_error = QString::fromUtf8(SDL_GetError()); if (error) *error = m_error; return false;
    }
    closeHandle(); m_handle = handle; m_selected = selected; m_ranges.clear(); m_error.clear();
    m_snapshot = {}; m_snapshot.connected = true; m_snapshot.instanceId = instanceId;
    m_snapshot.generation = ++m_generation;
    const quint64 generation = m_generation;
    // Populate before selection notification, so consumers never see a fake
    // connected snapshot with missing axes. First preview does not enable TX.
    SDL_JoystickUpdate();
    m_snapshot.rawAxes.resize(selected.axes); m_snapshot.axes.resize(selected.axes);
    m_snapshot.buttons.resize(selected.buttons); m_snapshot.hats.resize(selected.hats);
    for (int i = 0; i < selected.axes; ++i) m_snapshot.rawAxes[i] = SDL_JoystickGetAxis(handle, i);
    for (int i = 0; i < selected.buttons; ++i) m_snapshot.buttons[i] = SDL_JoystickGetButton(handle, i) != 0;
    for (int i = 0; i < selected.hats; ++i) m_snapshot.hats[i] = SDL_JoystickGetHat(handle, i);
    rebuildNormalized(); m_snapshot.monotonicMs = m_clock.elapsed();
    emit selectionChanged();
    if (!guard || generation != m_generation) return false;
    const Snapshot snapshot = m_snapshot; emit snapshotChanged(snapshot);
    return guard && generation == m_generation && m_snapshot.connected;
}
void JoystickDevice::clearSelection()
{
    if (!ownerThread()) return;
    closeHandle(); m_selected = {}; m_ranges.clear();
    m_snapshot = {}; m_snapshot.generation = ++m_generation; m_snapshot.monotonicMs = m_clock.elapsed();
    const QPointer<JoystickDevice> guard(this); const quint64 generation = m_generation;
    emit selectionChanged();
    if (!guard || generation != m_generation) return;
    const Snapshot snapshot = m_snapshot; emit snapshotChanged(snapshot);
}
void JoystickDevice::poll()
{
    if (!ownerThread() || !m_sdlOwned || m_polling) return;
    m_polling = true;
    const QPointer<JoystickDevice> guard(this);
    if (m_clock.elapsed() - m_lastRefresh >= 1000) {
        refresh(); if (!guard) return;
    }
    SDL_JoystickUpdate();
    if (!m_handle) { m_polling = false; return; }
    if (!SDL_JoystickGetAttached(m_handle)) {
        m_polling = false;
        const QString reason = QStringLiteral("Joystick disconnected. Refresh, select it and explicitly enable again.");
        m_error = reason;
        const quint64 nextGeneration = m_generation + 1;
        clearSelection();
        if (!guard || nextGeneration != m_generation) return;
        emit disconnected(reason); return;
    }
    for (int i = 0; i < m_snapshot.rawAxes.size(); ++i) {
        const qint16 raw = SDL_JoystickGetAxis(m_handle, i); m_snapshot.rawAxes[i] = raw;
        if (m_calibrating) {
            auto &range = m_observed[i]; range.minimum = qMin(range.minimum, int(raw)); range.maximum = qMax(range.maximum, int(raw));
        }
    }
    for (int i = 0; i < m_snapshot.buttons.size(); ++i) m_snapshot.buttons[i] = SDL_JoystickGetButton(m_handle, i) != 0;
    for (int i = 0; i < m_snapshot.hats.size(); ++i) m_snapshot.hats[i] = SDL_JoystickGetHat(m_handle, i);
    rebuildNormalized(); m_snapshot.monotonicMs = m_clock.elapsed();
    m_polling = false;
    const Snapshot snapshot = m_snapshot; emit snapshotChanged(snapshot);
}
void JoystickDevice::rebuildNormalized()
{
    for (int i = 0; i < m_snapshot.rawAxes.size(); ++i)
        m_snapshot.axes[i] = JoystickConfiguration::normalizeAxis(m_snapshot.rawAxes[i], m_ranges.value(i));
}
bool JoystickDevice::setRanges(const JoystickConfiguration::Ranges &ranges, QString *error)
{
    if (error) error->clear();
    if (!ownerThread() || !m_handle || m_calibrating) { if (error) *error = QStringLiteral("Select a joystick and finish calibration first."); return false; }
    for (auto r = ranges.cbegin(); r != ranges.cend(); ++r)
        if (r.key() < 0 || r.key() >= m_snapshot.rawAxes.size() || !JoystickConfiguration::validRange(r.value())) {
            if (error) *error = QStringLiteral("Invalid joystick axis calibration range."); return false;
        }
    m_ranges = ranges; rebuildNormalized(); emit calibrationChanged(); return true;
}
bool JoystickDevice::beginRangeCalibration()
{
    if (!ownerThread() || !m_handle || m_calibrating) return false;
    m_observed.clear();
    for (int i = 0; i < m_snapshot.rawAxes.size(); ++i)
        m_observed.insert(i, {m_snapshot.rawAxes[i], m_snapshot.rawAxes[i]});
    m_calibrating = true; emit calibrationChanged(); return true;
}
JoystickDevice::CalibrationResult JoystickDevice::finishRangeCalibration()
{
    CalibrationResult result;
    if (!ownerThread() || !m_calibrating) return result;
    for (int i = 0; i < m_snapshot.rawAxes.size(); ++i) {
        const auto range = m_observed.value(i);
        if (JoystickConfiguration::validRange(range)) { m_ranges[i] = range; ++result.calibratedAxes; }
        else ++result.ignoredAxes;
    }
    m_observed.clear(); m_calibrating = false; rebuildNormalized(); emit calibrationChanged(); return result;
}
void JoystickDevice::cancelRangeCalibration()
{
    if (!ownerThread() || !m_calibrating) return;
    m_calibrating = false; m_observed.clear(); emit calibrationChanged();
}
void JoystickDevice::resetRangeCalibration()
{
    if (!ownerThread()) return;
    m_calibrating = false; m_observed.clear(); m_ranges.clear(); rebuildNormalized(); emit calibrationChanged();
}
