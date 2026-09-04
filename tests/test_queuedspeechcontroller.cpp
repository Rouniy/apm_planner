#include <QtTest>

#include "audio/QueuedSpeechController.h"

class QueuedSpeechControllerTest final : public QObject
{
    Q_OBJECT

private slots:
    void normalizesAndCoalescesCaseSensitively();
    void keepsCurrentAndNewestFourPending();
    void floodNeverExceedsPendingBound();
    void readyAdvancesOneUtteranceAtATime();
    void stopAndErrorClearSafely();
    void watchdogWaitsForStopReadyBeforeAdvancing();
    void watchdogFailsClosedWhenStopNeverBecomesReady();
};

namespace
{
int resultValue(QueuedSpeechController::SubmitResult result)
{
    return static_cast<int>(result);
}
}

void QueuedSpeechControllerTest::normalizesAndCoalescesCaseSensitively()
{
    QStringList started;
    QueuedSpeechController controller(
        [&started](const QString &text) { started.append(text); },
        []() {});
    controller.backendStateChanged(
        QueuedSpeechController::BackendState::Ready);

    QCOMPARE(resultValue(controller.submit(QStringLiteral("   \t\n"))),
             resultValue(QueuedSpeechController::SubmitResult::Rejected));

    const QString longText(600, QLatin1Char('x'));
    QCOMPARE(resultValue(controller.submit(
                 QStringLiteral("  ") + longText + QStringLiteral("  "))),
             resultValue(QueuedSpeechController::SubmitResult::Accepted));
    QCOMPARE(controller.currentText(), longText.left(512));
    QCOMPARE(started, QStringList{longText.left(512)});

    QCOMPARE(resultValue(controller.submit(longText)),
             resultValue(QueuedSpeechController::SubmitResult::Coalesced));
    QCOMPARE(resultValue(controller.submit(longText.toUpper())),
             resultValue(QueuedSpeechController::SubmitResult::Accepted));
    QCOMPARE(controller.pendingTexts(),
             QStringList{longText.toUpper().left(512)});
}

void QueuedSpeechControllerTest::keepsCurrentAndNewestFourPending()
{
    QStringList started;
    QueuedSpeechController controller(
        [&started](const QString &text) { started.append(text); },
        []() {});
    controller.backendStateChanged(
        QueuedSpeechController::BackendState::Ready);

    QCOMPARE(resultValue(controller.submit(QStringLiteral("current"))),
             resultValue(QueuedSpeechController::SubmitResult::Accepted));
    for (int index = 1; index <= 4; ++index) {
        QCOMPARE(resultValue(controller.submit(
                     QStringLiteral("pending-%1").arg(index))),
                 resultValue(QueuedSpeechController::SubmitResult::Accepted));
    }
    QCOMPARE(controller.pendingTexts(),
             QStringList({QStringLiteral("pending-1"),
                          QStringLiteral("pending-2"),
                          QStringLiteral("pending-3"),
                          QStringLiteral("pending-4")}));

    QCOMPARE(resultValue(controller.submit(QStringLiteral("pending-5"))),
             resultValue(QueuedSpeechController::SubmitResult::Accepted));
    QCOMPARE(controller.currentText(), QStringLiteral("current"));
    QCOMPARE(started, QStringList{QStringLiteral("current")});
    QCOMPARE(controller.pendingTexts(),
             QStringList({QStringLiteral("pending-2"),
                          QStringLiteral("pending-3"),
                          QStringLiteral("pending-4"),
                          QStringLiteral("pending-5")}));

    QCOMPARE(resultValue(controller.submit(QStringLiteral("current"))),
             resultValue(QueuedSpeechController::SubmitResult::Coalesced));
    QCOMPARE(resultValue(controller.submit(QStringLiteral("pending-3"))),
             resultValue(QueuedSpeechController::SubmitResult::Coalesced));
    QCOMPARE(controller.pendingTexts().size(),
             QueuedSpeechController::MaximumPendingUtterances);
}

void QueuedSpeechControllerTest::floodNeverExceedsPendingBound()
{
    QueuedSpeechController controller([](const QString &) {}, []() {});
    controller.backendStateChanged(
        QueuedSpeechController::BackendState::Ready);
    controller.submit(QStringLiteral("current"));

    for (int index = 0; index < 10000; ++index) {
        QCOMPARE(resultValue(controller.submit(
                     QStringLiteral("message-%1").arg(index))),
                 resultValue(QueuedSpeechController::SubmitResult::Accepted));
        QVERIFY(controller.pendingTexts().size()
                <= QueuedSpeechController::MaximumPendingUtterances);
    }

    QCOMPARE(controller.currentText(), QStringLiteral("current"));
    QCOMPARE(controller.pendingTexts(),
             QStringList({QStringLiteral("message-9996"),
                          QStringLiteral("message-9997"),
                          QStringLiteral("message-9998"),
                          QStringLiteral("message-9999")}));
}

void QueuedSpeechControllerTest::readyAdvancesOneUtteranceAtATime()
{
    QStringList started;
    QueuedSpeechController controller(
        [&started](const QString &text) { started.append(text); },
        []() {});
    controller.backendStateChanged(
        QueuedSpeechController::BackendState::Ready);
    controller.submit(QStringLiteral("one"));
    controller.submit(QStringLiteral("two"));
    controller.submit(QStringLiteral("three"));

    QVERIFY(controller.isAvailable());
    QVERIFY(!controller.isIdle());
    QCOMPARE(started, QStringList{QStringLiteral("one")});

    controller.backendStateChanged(
        QueuedSpeechController::BackendState::Ready);
    QCOMPARE(controller.currentText(), QStringLiteral("two"));
    QCOMPARE(started, QStringList({QStringLiteral("one"),
                                   QStringLiteral("two")}));
    controller.backendStateChanged(
        QueuedSpeechController::BackendState::Ready);
    QCOMPARE(controller.currentText(), QStringLiteral("three"));
    controller.backendStateChanged(
        QueuedSpeechController::BackendState::Ready);

    QVERIFY(controller.isIdle());
    QCOMPARE(started, QStringList({QStringLiteral("one"),
                                   QStringLiteral("two"),
                                   QStringLiteral("three")}));
}

void QueuedSpeechControllerTest::stopAndErrorClearSafely()
{
    int stops = 0;
    QStringList started;
    QueuedSpeechController controller(
        [&started](const QString &text) { started.append(text); },
        [&stops]() { ++stops; });

    QVERIFY(!controller.isAvailable());
    QCOMPARE(resultValue(controller.submit(QStringLiteral("unavailable"))),
             resultValue(QueuedSpeechController::SubmitResult::Rejected));

    controller.backendStateChanged(
        QueuedSpeechController::BackendState::Ready);
    controller.submit(QStringLiteral("current"));
    controller.submit(QStringLiteral("pending"));
    controller.stop();
    QCOMPARE(stops, 1);
    QVERIFY(controller.currentText().isEmpty());
    QVERIFY(controller.pendingTexts().isEmpty());

    controller.backendStateChanged(
        QueuedSpeechController::BackendState::Ready);
    controller.submit(QStringLiteral("after-stop"));
    controller.submit(QStringLiteral("lost-on-error"));
    controller.backendStateChanged(
        QueuedSpeechController::BackendState::Error);
    QVERIFY(!controller.isAvailable());
    QVERIFY(controller.currentText().isEmpty());
    QVERIFY(controller.pendingTexts().isEmpty());
    QCOMPARE(resultValue(controller.submit(QStringLiteral("still-error"))),
             resultValue(QueuedSpeechController::SubmitResult::Rejected));

    controller.backendStateChanged(
        QueuedSpeechController::BackendState::Ready);
    QCOMPARE(resultValue(controller.submit(QStringLiteral("recovered"))),
             resultValue(QueuedSpeechController::SubmitResult::Accepted));
    QCOMPARE(started.last(), QStringLiteral("recovered"));
}

void QueuedSpeechControllerTest::watchdogWaitsForStopReadyBeforeAdvancing()
{
    QCOMPARE(QueuedSpeechController::UtteranceTimeoutMs, 60000);
    int stops = 0;
    QStringList started;
    QueuedSpeechController controller(
        [&started](const QString &text) { started.append(text); },
        [&stops]() { ++stops; },
        100);
    controller.backendStateChanged(
        QueuedSpeechController::BackendState::Ready);
    controller.submit(QStringLiteral("stuck"));
    controller.submit(QStringLiteral("next"));

    QTRY_COMPARE(stops, 1);
    QVERIFY(controller.currentText().isEmpty());
    QCOMPARE(controller.pendingTexts(),
             QStringList{QStringLiteral("next")});
    QCOMPARE(started, QStringList{QStringLiteral("stuck")});

    controller.backendStateChanged(
        QueuedSpeechController::BackendState::Ready);
    QCOMPARE(controller.currentText(), QStringLiteral("next"));
    QVERIFY(controller.pendingTexts().isEmpty());
    QCOMPARE(started, QStringList({QStringLiteral("stuck"),
                                   QStringLiteral("next")}));
}

void QueuedSpeechControllerTest::watchdogFailsClosedWhenStopNeverBecomesReady()
{
    int stops = 0;
    QueuedSpeechController controller(
        [](const QString &) {}, [&stops]() { ++stops; }, 40);
    controller.backendStateChanged(
        QueuedSpeechController::BackendState::Ready);
    controller.submit(QStringLiteral("stuck"));
    controller.submit(QStringLiteral("must-not-overlap"));

    QTRY_COMPARE(stops, 1);
    QTRY_VERIFY(!controller.isAvailable());
    QVERIFY(controller.currentText().isEmpty());
    QVERIFY(controller.pendingTexts().isEmpty());
}

QTEST_GUILESS_MAIN(QueuedSpeechControllerTest)
#include "test_queuedspeechcontroller.moc"
