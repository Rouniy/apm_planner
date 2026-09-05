#include "comm/MAVLinkSigningManager.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>
#include <mavlink.h>

#include <atomic>
#include <thread>
#include <type_traits>

namespace {

constexpr qint64 Now = 1700000000000LL;
const QByteArray UnsignedHeartbeat = QByteArray::fromHex(
    "fd0900004d2a010000000000000002030003032eff");

QByteArray key(char seed)
{
    QByteArray result(32, seed);
    result[31] = static_cast<char>(seed + 1);
    return result;
}

QByteArray readAll(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool writeAll(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(bytes) == bytes.size();
}

QByteArray unsignedRadioFrame()
{
    mavlink_message_t message{};
    message.msgid = MAVLINK_MSG_ID_RADIO_STATUS;
    mavlink_status_t status{};
    const mavlink_msg_entry_t *entry = mavlink_get_msg_entry(message.msgid);
    mavlink_finalize_message_buffer(&message, 51, 68, &status,
                                    entry->min_msg_len, entry->max_msg_len,
                                    entry->crc_extra);
    quint8 bytes[MAVLINK_MAX_PACKET_LEN]{};
    const quint16 size = mavlink_msg_to_send_buffer(bytes, &message);
    return QByteArray(reinterpret_cast<const char *>(bytes), size);
}

} // namespace

static_assert(!std::is_copy_constructible<MAVLinkSigningManager>::value,
              "Signing manager must retain unique policy ownership");
static_assert(!std::is_move_constructible<MAVLinkSigningManager>::value,
              "Signing manager thread identity must remain stable");

class MAVLinkSigningManagerTest final : public QObject
{
    Q_OBJECT

private slots:
    void unprotectedEpochPassesThroughWithoutCreatingState();
    void protectedLifecycleSignsAndVerifies();
    void lockedRequirementBlocksUntilMatchingKeyIsAvailable();
    void sameKeyAliasesShareReplayContext();
    void retainedKeyContextsAreBoundedWithoutReplayReset();
    void stableProfileIdsSurviveRestartAndNeverRecycle();
    void profileCannotBackTwoPhysicalBindings();
    void removeRequiresOfflineEpoch();
    void radioExceptionIsAcceptedButNeverAuthenticated();
    void registryBoundsCorruptionAndNoSecret();
    void pathAndInputValidationFailClosed();
    void foreignThreadCallsFailClosed();
};

void MAVLinkSigningManagerTest::unprotectedEpochPassesThroughWithoutCreatingState()
{
    QTemporaryDir parent;
    QVERIFY(parent.isValid());
    const QString absent = parent.filePath(QStringLiteral("not-created"));
    MAVLinkSigningManager manager(absent);
    QVERIFY(manager.beginEpoch(4, 10));
    QVERIFY(manager.rawWritesAllowed(4));
    QVERIFY(!manager.protectedLink(4));

    QByteArray output("old");
    QVERIFY(manager.signFrame(4, 10, UnsignedHeartbeat, Now, &output));
    QCOMPARE(output, UnsignedHeartbeat);
    QVERIFY(manager.signFrame(4, 10, output, Now, &output));
    QCOMPARE(output, UnsignedHeartbeat);
    const auto verification = manager.verifyFrame(
        4, 10, UnsignedHeartbeat, Now);
    QCOMPARE(verification.verdict,
             MAVLinkSigningManager::VerifyVerdict::Unprotected);
    QVERIFY(verification.accepted());
    QVERIFY(!verification.authenticated());

    QVERIFY(!manager.signFrame(4, 11, UnsignedHeartbeat, Now, &output));
    QVERIFY(output.isEmpty());
    QCOMPARE(manager.verifyFrame(4, 11, UnsignedHeartbeat, Now).verdict,
             MAVLinkSigningManager::VerifyVerdict::EpochMismatch);
    QVERIFY(!manager.endEpoch(4, 11));
    QVERIFY(manager.endEpoch(4, 10));
    QVERIFY(!manager.rawWritesAllowed(4));
    QVERIFY(!QFileInfo::exists(absent));
}

void MAVLinkSigningManagerTest::protectedLifecycleSignsAndVerifies()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    MAVLinkSigningManager manager(directory.path());
    const QByteArray secret = key('A');
    QVERIFY(manager.protectLink(5, QStringLiteral("serial:/dev/ttyACM0"),
                                QStringLiteral("Vehicle A"), secret, Now));
    QVERIFY(manager.protectedLink(5));
    QVERIFY(!manager.rawWritesAllowed(5));
    const auto offline = manager.status(5);
    QVERIFY(offline.protectedLink);
    QVERIFY(offline.keyAvailable);
    QCOMPARE(offline.activeEpoch, quint64(0));
    QCOMPARE(offline.signingLinkId, 0);
    QCOMPARE(offline.connectionProfileId,
             QStringLiteral("serial:/dev/ttyACM0"));
    QCOMPARE(offline.keyName, QStringLiteral("Vehicle A"));
    QCOMPARE(offline.keyFingerprint,
             QString::fromLatin1(QCryptographicHash::hash(
                 secret, QCryptographicHash::Sha256).toHex()));

    QVERIFY(manager.protectLink(5, QStringLiteral("serial:/dev/ttyACM0"),
                                QStringLiteral("Vehicle A"), secret, Now));
    QVERIFY(!manager.protectLink(5, QStringLiteral("serial:/dev/ttyACM0"),
                                 QStringLiteral("Renamed"), secret, Now));
    QVERIFY(!manager.protectLink(5, QStringLiteral("serial:/dev/ttyACM0"),
                                 QStringLiteral("Vehicle A"), key('B'), Now));

    QVERIFY(manager.beginEpoch(5, 100));
    QVERIFY(!manager.protectLink(5, QStringLiteral("serial:/dev/ttyACM0"),
                                 QStringLiteral("Vehicle A"), secret, Now));
    QByteArray signedFrame;
    QVERIFY(manager.signFrame(
        5, 100, UnsignedHeartbeat, Now, &signedFrame));
    QVERIFY(signedFrame != UnsignedHeartbeat);
    QCOMPARE(static_cast<quint8>(signedFrame.at(signedFrame.size() - 13)),
             quint8(0));
    const auto verified = manager.verifyFrame(5, 100, signedFrame, Now);
    QCOMPARE(verified.verdict,
             MAVLinkSigningManager::VerifyVerdict::Signed);
    QVERIFY(verified.authenticated());
    QCOMPARE(manager.verifyFrame(5, 100, signedFrame, Now).verdict,
             MAVLinkSigningManager::VerifyVerdict::Replay);
    QCOMPARE(manager.status(5).counters.signedSent, quint64(1));
    QCOMPARE(manager.status(5).counters.signedAccepted, quint64(1));
    QVERIFY(manager.endEpoch(5, 100));
    QVERIFY(manager.protectedLink(5));
    QVERIFY(!manager.rawWritesAllowed(5));
}

void MAVLinkSigningManagerTest::
lockedRequirementBlocksUntilMatchingKeyIsAvailable()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    MAVLinkSigningManager manager(directory.path());
    const QString profile = QStringLiteral("restart-profile");
    const QByteArray secret = key('L');
    const QByteArray fingerprint = QCryptographicHash::hash(
        secret, QCryptographicHash::Sha256);

    QVERIFY(manager.requireSigning(9, profile, fingerprint));
    QVERIFY(manager.requireSigning(9, profile, fingerprint));
    const auto locked = manager.status(9);
    QVERIFY(locked.protectedLink);
    QVERIFY(!locked.keyAvailable);
    QCOMPARE(locked.connectionProfileId, profile);
    QVERIFY(locked.keyName.isEmpty());
    QCOMPARE(locked.keyFingerprint,
             QString::fromLatin1(fingerprint.toHex()));
    QCOMPARE(locked.signingLinkId, -1);
    QVERIFY(!manager.rawWritesAllowed(9));
    QVERIFY(!QFileInfo::exists(directory.filePath(
        QStringLiteral("signing-clock.state"))));
    QVERIFY(!QFileInfo::exists(directory.filePath(
        QStringLiteral("signing-link-ids.state"))));

    QVERIFY(manager.beginEpoch(9, 90));
    QVERIFY(!manager.rawWritesAllowed(9));
    QByteArray output("old");
    QString error;
    QVERIFY(!manager.signFrame(
        9, 90, UnsignedHeartbeat, Now, &output, &error));
    QVERIFY(output.isEmpty());
    QCOMPARE(manager.verifyFrame(9, 90, UnsignedHeartbeat, Now).verdict,
             MAVLinkSigningManager::VerifyVerdict::NotReady);
    QVERIFY(!manager.protectLink(
        9, profile, QStringLiteral("Loaded"), secret, Now, &error));
    QVERIFY(!manager.status(9).keyAvailable);
    QVERIFY(manager.endEpoch(9, 90));

    QVERIFY(!manager.protectLink(
        9, profile, QStringLiteral("Wrong"), key('M'), Now, &error));
    QVERIFY(!manager.status(9).keyAvailable);
    QVERIFY(!manager.rawWritesAllowed(9));
    QVERIFY(manager.protectLink(
        9, profile, QStringLiteral("Loaded"), secret, Now, &error));
    const auto active = manager.status(9);
    QVERIFY(active.protectedLink);
    QVERIFY(active.keyAvailable);
    QCOMPARE(active.keyName, QStringLiteral("Loaded"));
    QCOMPARE(active.signingLinkId, 0);
    QCOMPARE(active.keyFingerprint, locked.keyFingerprint);

    QVERIFY(manager.beginEpoch(9, 91));
    QVERIFY(manager.signFrame(
        9, 91, UnsignedHeartbeat, Now, &output, &error));
    QCOMPARE(manager.verifyFrame(9, 91, output, Now).verdict,
             MAVLinkSigningManager::VerifyVerdict::Signed);
}

void MAVLinkSigningManagerTest::sameKeyAliasesShareReplayContext()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    MAVLinkSigningManager manager(directory.path());
    const QByteArray shared = key('C');
    const QByteArray sharedFingerprint = QCryptographicHash::hash(
        shared, QCryptographicHash::Sha256);
    QVERIFY(manager.requireSigning(
        1, QStringLiteral("udp-client:first"), sharedFingerprint));
    QVERIFY(manager.requireSigning(
        2, QStringLiteral("tcp:second"), sharedFingerprint));
    QVERIFY(manager.protectLink(1, QStringLiteral("udp-client:first"),
                                QStringLiteral("Alias One"), shared, Now));
    QVERIFY(manager.protectLink(2, QStringLiteral("tcp:second"),
                                QStringLiteral("Alias Two"), shared, Now));
    QVERIFY(manager.protectLink(3, QStringLiteral("serial:third"),
                                QStringLiteral("Different"), key('D'), Now));
    QVERIFY(manager.beginEpoch(1, 11));
    QVERIFY(manager.beginEpoch(2, 22));
    QVERIFY(manager.beginEpoch(3, 33));

    QByteArray signedFrame;
    QVERIFY(manager.signFrame(1, 11, UnsignedHeartbeat, Now, &signedFrame));
    QCOMPARE(manager.verifyFrame(1, 11, signedFrame, Now).verdict,
             MAVLinkSigningManager::VerifyVerdict::Signed);
    QCOMPARE(manager.verifyFrame(2, 22, signedFrame, Now).verdict,
             MAVLinkSigningManager::VerifyVerdict::Replay);
    QCOMPARE(manager.verifyFrame(3, 33, signedFrame, Now).verdict,
             MAVLinkSigningManager::VerifyVerdict::BadSignature);
    QCOMPARE(manager.status(1).keyFingerprint,
             manager.status(2).keyFingerprint);
    QCOMPARE(manager.status(1).counters.rejected,
             manager.status(2).counters.rejected);
    QVERIFY(manager.status(1).keyFingerprint
            != manager.status(3).keyFingerprint);

    QVERIFY(manager.endEpoch(1, 11));
    manager.removeLink(1);
    QVERIFY(manager.protectLink(4, QStringLiteral("udp-client:first"),
                                QStringLiteral("Alias Reopened"), shared, Now));
    QCOMPARE(manager.status(4).signingLinkId, 0);
    QVERIFY(manager.beginEpoch(4, 44));
    QCOMPARE(manager.verifyFrame(4, 44, signedFrame, Now).verdict,
             MAVLinkSigningManager::VerifyVerdict::Replay);
}

void MAVLinkSigningManagerTest::
retainedKeyContextsAreBoundedWithoutReplayReset()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    MAVLinkSigningManager manager(directory.path());
    const QString profile = QStringLiteral("rotating-profile");
    const auto contextKey = [](int index) {
        return QCryptographicHash::hash(
            QByteArray("retained-context-") + QByteArray::number(index),
            QCryptographicHash::Sha256);
    };

    QByteArray replayFrame;
    for (int index = 0; index < 256; ++index) {
        const QByteArray secret = contextKey(index);
        QVERIFY2(manager.protectLink(
                     1, profile, QStringLiteral("Key %1").arg(index),
                     secret, Now),
                 qPrintable(QString::number(index)));
        if (index == 0) {
            QVERIFY(manager.beginEpoch(1, 1));
            QVERIFY(manager.signFrame(
                1, 1, UnsignedHeartbeat, Now, &replayFrame));
            QCOMPARE(manager.verifyFrame(1, 1, replayFrame, Now).verdict,
                     MAVLinkSigningManager::VerifyVerdict::Signed);
            QVERIFY(manager.endEpoch(1, 1));
        }
        manager.removeLink(1);
        QVERIFY(!manager.protectedLink(1));
    }

    const QString registryPath = directory.filePath(
        QStringLiteral("signing-link-ids.state"));
    const QString clockPath = directory.filePath(
        QStringLiteral("signing-clock.state"));
    const QByteArray registryBefore = readAll(registryPath);
    const QByteArray clockBefore = readAll(clockPath);
    QString error;
    QVERIFY(!manager.protectLink(
        1, profile, QStringLiteral("Overflow"), contextKey(256), Now,
        &error));
    QVERIFY(error.contains(QStringLiteral("contexts"),
                           Qt::CaseInsensitive));
    QVERIFY(!manager.protectedLink(1));
    QCOMPARE(readAll(registryPath), registryBefore);
    QCOMPARE(readAll(clockPath), clockBefore);

    // A retained context remains usable at capacity and keeps its replay
    // history; making space by eviction would incorrectly accept this frame.
    QVERIFY(manager.protectLink(
        1, profile, QStringLiteral("Key 0"), contextKey(0), Now));
    QVERIFY(manager.beginEpoch(1, 2));
    QCOMPARE(manager.verifyFrame(1, 2, replayFrame, Now).verdict,
             MAVLinkSigningManager::VerifyVerdict::Replay);
}

void MAVLinkSigningManagerTest::stableProfileIdsSurviveRestartAndNeverRecycle()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QByteArray secret = key('E');
    {
        MAVLinkSigningManager first(directory.path());
        QVERIFY(first.protectLink(10, QStringLiteral("profile-a"),
                                  QStringLiteral("A"), secret, Now));
        QVERIFY(first.protectLink(11, QStringLiteral("profile-b"),
                                  QStringLiteral("B"), secret, Now));
        QCOMPARE(first.status(10).signingLinkId, 0);
        QCOMPARE(first.status(11).signingLinkId, 1);
        first.removeLink(10);
        QVERIFY(first.protectLink(12, QStringLiteral("profile-c"),
                                  QStringLiteral("C"), secret, Now));
        QCOMPARE(first.status(12).signingLinkId, 2);
    }
    {
        MAVLinkSigningManager restarted(directory.path());
        QVERIFY(restarted.protectLink(20, QStringLiteral("profile-b"),
                                      QStringLiteral("B"), secret, Now));
        QVERIFY(restarted.protectLink(21, QStringLiteral("profile-a"),
                                      QStringLiteral("A"), secret, Now));
        QCOMPARE(restarted.status(20).signingLinkId, 1);
        QCOMPARE(restarted.status(21).signingLinkId, 0);
        QVERIFY(restarted.protectLink(22, QStringLiteral("profile-d"),
                                      QStringLiteral("D"), secret, Now));
        QCOMPARE(restarted.status(22).signingLinkId, 3);
    }
}

void MAVLinkSigningManagerTest::profileCannotBackTwoPhysicalBindings()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    MAVLinkSigningManager manager(directory.path());
    const QByteArray secret = key('F');
    const QByteArray fingerprint = QCryptographicHash::hash(
        secret, QCryptographicHash::Sha256);
    QVERIFY(manager.requireSigning(
        1, QStringLiteral("same-profile"), fingerprint));
    QVERIFY(!manager.requireSigning(
        2, QStringLiteral("same-profile"), fingerprint));
    QVERIFY(manager.protectLink(1, QStringLiteral("same-profile"),
                                QStringLiteral("First"), secret, Now));
    QVERIFY(!manager.protectLink(2, QStringLiteral("same-profile"),
                                 QStringLiteral("Second"), secret, Now));
    manager.removeLink(1);
    QVERIFY(manager.protectLink(2, QStringLiteral("same-profile"),
                                QStringLiteral("Second"), secret, Now));
    QCOMPARE(manager.status(2).signingLinkId, 0);
}

void MAVLinkSigningManagerTest::removeRequiresOfflineEpoch()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    MAVLinkSigningManager manager(directory.path());
    QVERIFY(manager.protectLink(7, QStringLiteral("profile"),
                                QStringLiteral("Key"), key('G'), Now));
    QVERIFY(manager.beginEpoch(7, 70));
    manager.removeLink(7);
    QVERIFY(manager.protectedLink(7));
    QCOMPARE(manager.status(7).activeEpoch, quint64(70));
    QVERIFY(manager.endEpoch(7, 70));
    manager.removeLink(7);
    QVERIFY(!manager.protectedLink(7));
}

void MAVLinkSigningManagerTest::radioExceptionIsAcceptedButNeverAuthenticated()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    MAVLinkSigningManager manager(directory.path());
    QVERIFY(manager.protectLink(8, QStringLiteral("radio-profile"),
                                QStringLiteral("Key"), key('H'), Now));
    QVERIFY(manager.beginEpoch(8, 80));
    const auto radio = manager.verifyFrame(8, 80, unsignedRadioFrame(), Now);
    QCOMPARE(radio.verdict,
             MAVLinkSigningManager::VerifyVerdict::UnsignedRadio);
    QVERIFY(radio.accepted());
    QVERIFY(!radio.authenticated());
    const auto heartbeat = manager.verifyFrame(
        8, 80, UnsignedHeartbeat, Now);
    QCOMPARE(heartbeat.verdict,
             MAVLinkSigningManager::VerifyVerdict::UnsignedRejected);
    QVERIFY(!heartbeat.accepted());
}

void MAVLinkSigningManagerTest::registryBoundsCorruptionAndNoSecret()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QByteArray secret(32, 'S');
    MAVLinkSigningManager manager(directory.path());
    for (int index = 0; index < 256; ++index) {
        QVERIFY2(manager.protectLink(
                     index, QStringLiteral("profile-%1").arg(index),
                     QStringLiteral("Shared"), secret, Now),
                 qPrintable(QString::number(index)));
        QCOMPARE(manager.status(index).signingLinkId, index);
    }
    QVERIFY(!manager.protectLink(300, QStringLiteral("overflow"),
                                 QStringLiteral("Shared"), secret, Now));
    const QString registry = directory.filePath(
        QStringLiteral("signing-link-ids.state"));
    const QString clock = directory.filePath(QStringLiteral("signing-clock.state"));
    QVERIFY(!readAll(registry).contains(secret));
    QVERIFY(!readAll(clock).contains(secret));

    const QByteArray unexpected(readAll(registry).size(), 'X');
    QVERIFY(writeAll(registry, unexpected));
    QVERIFY(!manager.protectLink(301, QStringLiteral("after-corruption"),
                                 QStringLiteral("Shared"), secret, Now));
    QCOMPARE(readAll(registry), unexpected);
    QVERIFY(!manager.protectedLink(301));
}

void MAVLinkSigningManagerTest::pathAndInputValidationFailClosed()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    MAVLinkSigningManager relative(QStringLiteral("relative-signing-state"));
    QVERIFY(!relative.protectLink(1, QStringLiteral("profile"),
                                  QStringLiteral("Key"), key('I'), Now));

    const QString absent = directory.filePath(QStringLiteral("absent"));
    MAVLinkSigningManager missing(absent);
    QVERIFY(!missing.protectLink(1, QStringLiteral("profile"),
                                 QStringLiteral("Key"), key('I'), Now));
    QVERIFY(!QFileInfo::exists(absent));

    MAVLinkSigningManager valid(directory.path());
    QVERIFY(!valid.protectLink(-1, QStringLiteral("profile"),
                               QStringLiteral("Key"), key('I'), Now));
    QVERIFY(!valid.protectLink(1, QString(), QStringLiteral("Key"),
                               key('I'), Now));
    QVERIFY(!valid.protectLink(1, QStringLiteral(" profile"),
                               QStringLiteral("Key"), key('I'), Now));
    QVERIFY(!valid.protectLink(1, QStringLiteral("profile"), QString(),
                               key('I'), Now));
    QVERIFY(!valid.protectLink(1, QStringLiteral("profile"),
                               QStringLiteral("Key"), QByteArray(32, 0), Now));
    QVERIFY(!valid.protectLink(1, QStringLiteral("profile"),
                               QStringLiteral("Key"), QByteArray(31, 1), Now));
    QVERIFY(!valid.requireSigning(-1, QStringLiteral("profile"),
                                  QByteArray(32, 1)));
    QVERIFY(!valid.requireSigning(1, QString(), QByteArray(32, 1)));
    QVERIFY(!valid.requireSigning(1, QStringLiteral("profile"),
                                  QByteArray(31, 1)));
    QVERIFY(!valid.beginEpoch(-1, 1));
    QVERIFY(!valid.beginEpoch(1, 0));
    QVERIFY(!valid.rawWritesAllowed(-1));
}

void MAVLinkSigningManagerTest::foreignThreadCallsFailClosed()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    MAVLinkSigningManager manager(directory.path());
    std::atomic<bool> beginResult{true};
    std::atomic<bool> rawResult{true};
    std::atomic<bool> requireResult{true};
    const QByteArray fingerprint = QCryptographicHash::hash(
        key('J'), QCryptographicHash::Sha256);
    std::thread worker([&]() {
        beginResult.store(manager.beginEpoch(1, 1));
        rawResult.store(manager.rawWritesAllowed(1));
        requireResult.store(manager.requireSigning(
            2, QStringLiteral("profile"), fingerprint));
    });
    worker.join();
    QVERIFY(!beginResult.load());
    QVERIFY(!rawResult.load());
    QVERIFY(!requireResult.load());
    QVERIFY(manager.beginEpoch(1, 1));
}

QTEST_GUILESS_MAIN(MAVLinkSigningManagerTest)

#include "test_mavlinksigningmanager.moc"
