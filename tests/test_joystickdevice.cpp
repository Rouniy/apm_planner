#include "input/JoystickDevice.h"
#include <QPointer>
#include <QSignalSpy>
#include <QtTest>
#include <SDL.h>

class TestJoystickDevice : public QObject
{
    Q_OBJECT
private slots:
    void noDeviceDoesNotInventInput()
    {
        JoystickDevice device;
        QVERIFY(!device.isOpen()); QVERIFY(!device.snapshot().connected);
        QVERIFY(device.snapshot().axes.isEmpty()); QString error;
        QVERIFY(!device.selectDevice(-999999, &error)); QVERIFY(!error.isEmpty());
        QVERIFY(!device.beginRangeCalibration());
        QVERIFY(!device.setRanges({{0, {-10000, 10000}}}, &error));
        device.clearSelection(); QVERIFY(!device.isOpen());
    }
    void virtualInputCalibrationHotUnplug()
    {
#if SDL_VERSION_ATLEAST(2, 0, 14)
        JoystickDevice device;
        const int index = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_FLIGHT_STICK, 12, 24, 2);
        if (index < 0) QSKIP("SDL virtual joystick unavailable.");
        SDL_Joystick *controller = SDL_JoystickOpen(index); QVERIFY(controller);
        const qint32 id = SDL_JoystickInstanceID(controller);
        QSignalSpy devices(&device, &JoystickDevice::devicesChanged);
        device.refresh(); QVERIFY(devices.count() > 0);
        QString error; QVERIFY2(device.selectDevice(id, &error), qPrintable(error));
        QCOMPARE(device.selectedDevice().axes, 12); QCOMPARE(device.selectedDevice().buttons, 24); QCOMPARE(device.selectedDevice().hats, 2);
        const quint64 generation = device.snapshot().generation; QVERIFY(generation > 0);
        QVERIFY(SDL_JoystickSetVirtualAxis(controller, 0, 32767) == 0);
        QVERIFY(SDL_JoystickSetVirtualAxis(controller, 1, -32768) == 0);
        QVERIFY(SDL_JoystickSetVirtualButton(controller, 23, 1) == 0);
        QVERIFY(SDL_JoystickSetVirtualHat(controller, 1, SDL_HAT_LEFTUP) == 0);
        device.poll(); const auto state = device.snapshot();
        QVERIFY(state.connected); QCOMPARE(state.generation, generation);
        QCOMPARE(state.axes[0], quint16(65535)); QCOMPARE(state.axes[1], quint16(0));
        QVERIFY(state.buttons[23]); QCOMPARE(state.hats[1], quint8(SDL_HAT_LEFTUP));
        QVERIFY(device.beginRangeCalibration());
        SDL_JoystickSetVirtualAxis(controller, 2, -12000); device.poll();
        SDL_JoystickSetVirtualAxis(controller, 2, 14000); device.poll();
        const auto calibrated = device.finishRangeCalibration(); QVERIFY(calibrated.calibratedAxes >= 1);
        QCOMPARE(device.ranges()[2].minimum, -12000); QCOMPARE(device.ranges()[2].maximum, 14000);
        SDL_JoystickSetVirtualAxis(controller, 2, -12000); device.poll(); QCOMPARE(device.snapshot().axes[2], quint16(0));
        device.resetRangeCalibration(); QVERIFY(device.ranges().isEmpty());
        QCOMPARE(device.snapshot().axes[2], quint16(20768));
        QVERIFY(device.beginRangeCalibration()); device.cancelRangeCalibration(); QVERIFY(!device.isCalibrating());
        QSignalSpy lost(&device, &JoystickDevice::disconnected);
        QVERIFY(SDL_JoystickDetachVirtual(index) == 0); device.poll();
        QVERIFY(!device.isOpen()); QVERIFY(device.snapshot().axes.isEmpty()); QVERIFY(device.snapshot().generation > generation);
        QCOMPARE(lost.count(), 1); device.poll(); QCOMPARE(lost.count(), 1);
        SDL_JoystickClose(controller);
#else
        QSKIP("SDL 2.0.14 virtual joystick API required.");
#endif
    }
    void callbackDeletionAndSelectionFence()
    {
#if SDL_VERSION_ATLEAST(2, 0, 14)
        QVERIFY(SDL_InitSubSystem(SDL_INIT_JOYSTICK) == 0);
        auto *device = new JoystickDevice; QPointer<JoystickDevice> guard(device);
        const int index = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 4, 4, 0);
        if (index < 0) { delete device; SDL_QuitSubSystem(SDL_INIT_JOYSTICK); QSKIP("SDL virtual joystick unavailable."); }
        SDL_Joystick *controller = SDL_JoystickOpen(index); QVERIFY(controller);
        const qint32 id = SDL_JoystickInstanceID(controller);
        device->refresh();
        QMetaObject::Connection connection;
        connection = connect(device, &JoystickDevice::selectionChanged, device, [device, &connection] {
            QObject::disconnect(connection); device->clearSelection();
        });
        QVERIFY(!device->selectDevice(id)); QVERIFY(!device->isOpen());
        QVERIFY(device->selectDevice(id));
        connect(device, &JoystickDevice::snapshotChanged, device, [device](const JoystickDevice::Snapshot &) { delete device; });
        device->poll(); QVERIFY(guard.isNull());
        SDL_JoystickClose(controller);
        SDL_JoystickDetachVirtual(index); SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
#else
        QSKIP("SDL 2.0.14 virtual joystick API required.");
#endif
    }
};
QTEST_GUILESS_MAIN(TestJoystickDevice)
#include "test_joystickdevice.moc"
