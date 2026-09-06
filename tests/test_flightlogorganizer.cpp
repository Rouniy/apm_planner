#include "ui/Loghandling/FlightLogOrganizer.h"
#include "ui/Loghandling/FlightLogClassifier.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>

namespace {

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(bytes) == bytes.size();
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QString canonicalRoot(const QTemporaryDir &directory)
{
    return QFileInfo(directory.path()).canonicalFilePath();
}

const FlightLogOrganizer::Entry *entryFor(
    const FlightLogOrganizer::Plan &plan, const QString &source)
{
    const QString expected = QFileInfo(source).absoluteFilePath();
    for (const auto &entry : plan.entries()) {
        if (QFileInfo(entry.source).absoluteFilePath() == expected)
            return &entry;
    }
    return nullptr;
}

} // namespace

class FlightLogOrganizerTest final : public QObject
{
    Q_OBJECT

private slots:
    void plansAndExecutesSmallCompanionsAndEmpty();
    void alreadyOrganizedCandidateIsValidNoop();
    void prefixIsLiteralAndCollateralIsExplicit();
    void hiddenCandidateAndCompanionAreIncluded();
    void destinationCollisionFailsBeforeMutation();
    void overlappingCandidateGroupsAreLeftWhileIndependentWorkContinues();
    void sameStemCandidatesUseOneDeterministicClassification();
    void sourceChangeAfterPlanFailsBeforeMutation();
    void destinationAppearanceAfterPlanFailsBeforeMutation();
    void cancellationBeforeAndDuringExecutionIsTruthful();
    void progressCanResetCallerPlan();
    void progressCanReplaceCallerPlanAfterFirstOperation();
    void finalCancellationCallbackCannotDeleteNewContents();
    void finalCancellationCallbackCannotMoveChangedSource();
    void symlinkSourcesAndDirectoriesAreSkipped();
    void symlinkRootAndDestinationAreRefused();
    void broadRootsAndInvalidPlansAreRefused();
    void mutationDuringClassifierProgressInvalidatesAnalysis();
    void malformedCandidateIsLeftWhileOtherWorkIsPlanned();
};

void FlightLogOrganizerTest::plansAndExecutesSmallCompanionsAndEmpty()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString root = canonicalRoot(directory);
    const QString incoming = QDir(root).filePath(QStringLiteral("incoming"));
    QVERIFY(QDir().mkpath(incoming));
    const QString log = QDir(incoming).filePath(QStringLiteral("small.log"));
    const QString companion = QDir(incoming).filePath(
        QStringLiteral("small.log.param"));
    const QString unrelated = QDir(incoming).filePath(
        QStringLiteral("other.param"));
    const QString empty = QDir(incoming).filePath(QStringLiteral("empty.bin"));
    QVERIFY(writeFile(log, QByteArray("small log")));
    QVERIFY(writeFile(companion, QByteArray("companion")));
    QVERIFY(writeFile(unrelated, QByteArray("unrelated")));
    QVERIFY(writeFile(empty, QByteArray()));

    QList<QPair<qint64, qint64>> progress;
    const auto analysis = FlightLogOrganizer::Analyze(
        root, {}, [&progress](qint64 done, qint64 total) {
            progress.append(qMakePair(done, total));
        });
    QVERIFY2(analysis.success, qPrintable(analysis.error));
    QVERIFY(analysis.plan.isValid());
    QCOMPARE(analysis.plan.root(), root);
    QCOMPARE(analysis.plan.candidateCount(), 2);
    QCOMPARE(analysis.plan.entries().size(), 3);
    QVERIFY(!progress.isEmpty());
    qint64 previousProgress = -1;
    for (const auto &sample : progress) {
        QCOMPARE(sample.second, qint64(2000));
        QVERIFY(sample.first >= previousProgress);
        QVERIFY(sample.first <= sample.second);
        previousProgress = sample.first;
    }
    QCOMPARE(previousProgress, qint64(2000));

    const auto *logEntry = entryFor(analysis.plan, log);
    const auto *companionEntry = entryFor(analysis.plan, companion);
    const auto *emptyEntry = entryFor(analysis.plan, empty);
    QVERIFY(logEntry && companionEntry && emptyEntry);
    QCOMPARE(int(logEntry->operation),
             int(FlightLogOrganizer::Operation::Move));
    QCOMPARE(logEntry->destination,
             QDir(root).filePath(QStringLiteral("SMALL/small.log")));
    QCOMPARE(companionEntry->destination,
             QDir(root).filePath(QStringLiteral("SMALL/small.log.param")));
    QCOMPARE(int(emptyEntry->operation),
             int(FlightLogOrganizer::Operation::DeleteEmpty));
    QVERIFY(emptyEntry->destination.isEmpty());
    QCOMPARE(emptyEntry->bytes, qint64(0));
    QVERIFY(std::any_of(analysis.plan.warnings().cbegin(),
                        analysis.plan.warnings().cend(),
                        [](const QString &warning) {
        return warning.contains(QStringLiteral("small.log.param"));
    }));

    const auto result = FlightLogOrganizer::Execute(analysis.plan);
    QVERIFY2(result.success, qPrintable(result.error));
    QVERIFY(!result.cancelled);
    QCOMPARE(result.completed.size(), 3);
    QCOMPARE(result.remaining, 0);
    QCOMPARE(readFile(logEntry->destination), QByteArray("small log"));
    QCOMPARE(readFile(companionEntry->destination), QByteArray("companion"));
    QVERIFY(!QFileInfo::exists(log));
    QVERIFY(!QFileInfo::exists(companion));
    QVERIFY(!QFileInfo::exists(empty));
    QCOMPARE(readFile(unrelated), QByteArray("unrelated"));
}

void FlightLogOrganizerTest::alreadyOrganizedCandidateIsValidNoop()
{
    QTemporaryDir directory;
    const QString root = canonicalRoot(directory);
    const QString small = QDir(root).filePath(QStringLiteral("SMALL"));
    QVERIFY(QDir().mkpath(small));
    const QString path = QDir(small).filePath(QStringLiteral("done.log"));
    QVERIFY(writeFile(path, QByteArray("already organized")));

    const auto analysis = FlightLogOrganizer::Analyze(root);
    QVERIFY2(analysis.success, qPrintable(analysis.error));
    QCOMPARE(analysis.plan.candidateCount(), 1);
    QVERIFY(analysis.plan.entries().isEmpty());
    const auto result = FlightLogOrganizer::Execute(analysis.plan);
    QVERIFY2(result.success, qPrintable(result.error));
    QVERIFY(result.completed.isEmpty());
    QCOMPARE(readFile(path), QByteArray("already organized"));
}

void FlightLogOrganizerTest::prefixIsLiteralAndCollateralIsExplicit()
{
    QTemporaryDir directory;
    const QString root = canonicalRoot(directory);
    const QString incoming = QDir(root).filePath(QStringLiteral("incoming"));
    QVERIFY(QDir().mkpath(incoming));
    const QString log = QDir(incoming).filePath(QStringLiteral("a[1].log"));
    const QString literal = QDir(incoming).filePath(QStringLiteral("a[1].jpg"));
    const QString wildcardLike = QDir(incoming).filePath(QStringLiteral("a1.jpg"));
    const QString collateral = QDir(incoming).filePath(QStringLiteral("a[1]-extra.txt"));
    QVERIFY(writeFile(log, "x"));
    QVERIFY(writeFile(literal, "image"));
    QVERIFY(writeFile(wildcardLike, "not a bracket match"));
    QVERIFY(writeFile(collateral, "prefix collateral"));

    const auto analysis = FlightLogOrganizer::Analyze(root);
    QVERIFY2(analysis.success, qPrintable(analysis.error));
    QCOMPARE(analysis.plan.candidateCount(), 1);
    QCOMPARE(analysis.plan.entries().size(), 3);
    QVERIFY(entryFor(analysis.plan, log));
    QVERIFY(entryFor(analysis.plan, literal));
    QVERIFY(entryFor(analysis.plan, collateral));
    QVERIFY(!entryFor(analysis.plan, wildcardLike));
    QCOMPARE(readFile(wildcardLike), QByteArray("not a bracket match"));
}

void FlightLogOrganizerTest::hiddenCandidateAndCompanionAreIncluded()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString root = canonicalRoot(directory);
    const QString candidate = QDir(root).filePath(QStringLiteral(".flight.log"));
    const QString companion = QDir(root).filePath(QStringLiteral(".flight.param"));
    QVERIFY(writeFile(candidate, "hidden log"));
    QVERIFY(writeFile(companion, "hidden companion"));

    const auto analysis = FlightLogOrganizer::Analyze(root);
    QVERIFY2(analysis.success, qPrintable(analysis.error));
    QCOMPARE(analysis.plan.candidateCount(), 1);
    QVERIFY(entryFor(analysis.plan, candidate));
    QVERIFY(entryFor(analysis.plan, companion));
}

void FlightLogOrganizerTest::destinationCollisionFailsBeforeMutation()
{
    QTemporaryDir directory;
    const QString root = canonicalRoot(directory);
    const QString incoming = QDir(root).filePath(QStringLiteral("incoming"));
    const QString small = QDir(root).filePath(QStringLiteral("SMALL"));
    QVERIFY(QDir().mkpath(incoming));
    QVERIFY(QDir().mkpath(small));
    const QString source = QDir(incoming).filePath(QStringLiteral("same.log"));
    const QString destination = QDir(small).filePath(QStringLiteral("same.log"));
    QVERIFY(writeFile(source, "new"));
    QVERIFY(writeFile(destination, "old"));

    const auto analysis = FlightLogOrganizer::Analyze(root);
    QVERIFY(!analysis.success);
    QVERIFY(!analysis.plan.isValid());
    QVERIFY(analysis.error.contains(QStringLiteral("Destination"),
                                    Qt::CaseInsensitive));
    QCOMPARE(readFile(source), QByteArray("new"));
    QCOMPARE(readFile(destination), QByteArray("old"));
}

void FlightLogOrganizerTest::overlappingCandidateGroupsAreLeftWhileIndependentWorkContinues()
{
    QTemporaryDir directory;
    const QString root = canonicalRoot(directory);
    const QString source = QDir(root).filePath(QStringLiteral("source"));
    QVERIFY(QDir().mkpath(source));
    const QString moving = QDir(source).filePath(QStringLiteral("a.log"));
    const QString emptyCandidate = QDir(source).filePath(QStringLiteral("a.bin"));
    const QString independent = QDir(source).filePath(QStringLiteral("z.log"));
    QVERIFY(writeFile(moving, "x"));
    QVERIFY(writeFile(emptyCandidate, QByteArray()));
    QVERIFY(writeFile(independent, "z"));

    const auto analysis = FlightLogOrganizer::Analyze(root);
    QVERIFY2(analysis.success, qPrintable(analysis.error));
    QVERIFY(!entryFor(analysis.plan, moving));
    QVERIFY(!entryFor(analysis.plan, emptyCandidate));
    QVERIFY(entryFor(analysis.plan, independent));
    QVERIFY(analysis.plan.warnings().join(QLatin1Char('\n')).contains(
        QStringLiteral("empty/non-empty"), Qt::CaseInsensitive));
    QVERIFY(QFileInfo::exists(moving));
    QVERIFY(QFileInfo::exists(emptyCandidate));
}

void FlightLogOrganizerTest::sameStemCandidatesUseOneDeterministicClassification()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString root = canonicalRoot(directory);
    const QString raw = QDir(root).filePath(QStringLiteral("mixed.rlog"));
    const QString dataFlash = QDir(root).filePath(QStringLiteral("mixed.log"));
    const QString independent = QDir(root).filePath(QStringLiteral("other.log"));
    const QByteArray rawBytes(2048, '\x55');
    const QByteArray textBytes =
        QByteArray("FMT,1,31,PARM,QNf,TimeUS,Name,Value\n"
                   "FMT,2,75,MSG,QZ,TimeUS,Message\n"
                   "FMT,3,11,SIM,Q,TimeUS\n"
                   "FMT,4,7,PAD,I,N\n")
        + QByteArray("PAD,0\n").repeated(180)
        + QByteArray("PARM,0,SYSID_THISMAV,7\nMSG,0,ArduPlane\n");
    QVERIFY(rawBytes.size() > 1024);
    QVERIFY(textBytes.size() > 1024);
    QVERIFY(writeFile(raw, rawBytes));
    QVERIFY(writeFile(dataFlash, textBytes));
    QVERIFY(writeFile(independent, "independent"));

    const auto rawClassification = FlightLogClassifier::Classify(raw);
    const auto dataFlashClassification = FlightLogClassifier::Classify(dataFlash);
    QVERIFY2(rawClassification.success, qPrintable(rawClassification.error));
    QVERIFY2(dataFlashClassification.success,
             qPrintable(dataFlashClassification.error));
    QVERIFY(rawClassification.relativeDirectory
            != dataFlashClassification.relativeDirectory);
    QCOMPARE(rawClassification.relativeDirectory, QStringLiteral("BAD"));
    QCOMPARE(dataFlashClassification.relativeDirectory,
             QStringLiteral("FIXED_WING/7"));

    const auto analysis = FlightLogOrganizer::Analyze(root);
    QVERIFY2(analysis.success, qPrintable(analysis.error));
    const auto *rawEntry = entryFor(analysis.plan, raw);
    const auto *dataFlashEntry = entryFor(analysis.plan, dataFlash);
    const auto *independentEntry = entryFor(analysis.plan, independent);
    QVERIFY(rawEntry && dataFlashEntry && independentEntry);
    QCOMPARE(QFileInfo(rawEntry->destination).absolutePath(),
             QDir(root).filePath(QStringLiteral("BAD")));
    QCOMPARE(QFileInfo(dataFlashEntry->destination).absolutePath(),
             QDir(root).filePath(QStringLiteral("BAD")));
    QCOMPARE(QFileInfo(independentEntry->destination).absolutePath(),
             QDir(root).filePath(QStringLiteral("SMALL")));
    QVERIFY(analysis.plan.warnings().join(QLatin1Char('\n')).contains(
        QStringLiteral("deterministic primary"), Qt::CaseInsensitive));
}

void FlightLogOrganizerTest::sourceChangeAfterPlanFailsBeforeMutation()
{
    QTemporaryDir directory;
    const QString root = canonicalRoot(directory);
    const QString first = QDir(root).filePath(QStringLiteral("a.log"));
    const QString second = QDir(root).filePath(QStringLiteral("z.log"));
    QVERIFY(writeFile(first, "one"));
    QVERIFY(writeFile(second, "two"));
    const auto analysis = FlightLogOrganizer::Analyze(root);
    QVERIFY2(analysis.success, qPrintable(analysis.error));
    QVERIFY(writeFile(second, "TWO")); // Same size; only the hash changes.

    const auto result = FlightLogOrganizer::Execute(analysis.plan);
    QVERIFY(!result.success);
    QVERIFY(!result.cancelled);
    QVERIFY(result.completed.isEmpty());
    QCOMPARE(result.remaining, analysis.plan.entries().size());
    QCOMPARE(readFile(first), QByteArray("one"));
    QCOMPARE(readFile(second), QByteArray("TWO"));
    QVERIFY(!QFileInfo::exists(QDir(root).filePath(QStringLiteral("SMALL/a.log"))));
}

void FlightLogOrganizerTest::destinationAppearanceAfterPlanFailsBeforeMutation()
{
    QTemporaryDir directory;
    const QString root = canonicalRoot(directory);
    const QString first = QDir(root).filePath(QStringLiteral("a.log"));
    const QString second = QDir(root).filePath(QStringLiteral("z.log"));
    QVERIFY(writeFile(first, "one"));
    QVERIFY(writeFile(second, "two"));
    const auto analysis = FlightLogOrganizer::Analyze(root);
    QVERIFY2(analysis.success, qPrintable(analysis.error));
    const QString occupied = QDir(root).filePath(QStringLiteral("SMALL/z.log"));
    QVERIFY(QDir().mkpath(QFileInfo(occupied).absolutePath()));
    QVERIFY(writeFile(occupied, "occupied"));

    const auto result = FlightLogOrganizer::Execute(analysis.plan);
    QVERIFY(!result.success);
    QVERIFY(result.completed.isEmpty());
    QCOMPARE(readFile(first), QByteArray("one"));
    QCOMPARE(readFile(second), QByteArray("two"));
    QCOMPARE(readFile(occupied), QByteArray("occupied"));
}

void FlightLogOrganizerTest::cancellationBeforeAndDuringExecutionIsTruthful()
{
    QTemporaryDir directory;
    const QString root = canonicalRoot(directory);
    const QString first = QDir(root).filePath(QStringLiteral("a.log"));
    const QString second = QDir(root).filePath(QStringLiteral("z.log"));
    QVERIFY(writeFile(first, "one"));
    QVERIFY(writeFile(second, "two"));

    auto cancelledAnalysis = FlightLogOrganizer::Analyze(root, [] { return true; });
    QVERIFY(cancelledAnalysis.cancelled);
    QVERIFY(!cancelledAnalysis.success);
    QVERIFY(!cancelledAnalysis.plan.isValid());

    const auto analysis = FlightLogOrganizer::Analyze(root);
    QVERIFY2(analysis.success, qPrintable(analysis.error));
    auto before = FlightLogOrganizer::Execute(analysis.plan, [] { return true; });
    QVERIFY(before.cancelled);
    QVERIFY(before.completed.isEmpty());
    QCOMPARE(before.remaining, 2);

    bool stop = false;
    const auto partial = FlightLogOrganizer::Execute(
        analysis.plan, [&stop] { return stop; },
        [&stop](qint64 completed, qint64) {
            if (completed == 1) stop = true;
        });
    QVERIFY(!partial.success);
    QVERIFY(partial.cancelled);
    QCOMPARE(partial.completed.size(), 1);
    QCOMPARE(partial.remaining, 1);
    const auto &completed = partial.completed.constFirst();
    QVERIFY(!QFileInfo::exists(completed.source));
    QVERIFY(QFileInfo::exists(completed.destination));
    const QString untouched = completed.source == QFileInfo(first).absoluteFilePath()
        ? second : first;
    QVERIFY(QFileInfo::exists(untouched));

    // A partially executed immutable plan is not a resume token.
    const auto retry = FlightLogOrganizer::Execute(analysis.plan);
    QVERIFY(!retry.success);
    QVERIFY(retry.completed.isEmpty());
}

void FlightLogOrganizerTest::finalCancellationCallbackCannotDeleteNewContents()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString root = canonicalRoot(directory);
    const QString source = QDir(root).filePath(QStringLiteral("empty.log"));
    QVERIFY(writeFile(source, {}));
    const auto analysis = FlightLogOrganizer::Analyze(root);
    QVERIFY2(analysis.success, qPrintable(analysis.error));
    QCOMPARE(analysis.plan.entries().size(), 1);
    bool armed = false, changed = false;
    int checks = 0;
    const auto result = FlightLogOrganizer::Execute(analysis.plan,
        [&] {
            // Once execution starts: loop gate, empty-source snapshot gate,
            // then the final cancellation callback after the snapshot.
            if (armed && ++checks == 3)
                changed = writeFile(source, QByteArrayLiteral("must survive"));
            return false;
        }, [&](qint64 completed, qint64) {
            if (completed == 0) armed = true;
        });
    QVERIFY(changed);
    QVERIFY(!result.success);
    QVERIFY(!result.cancelled);
    QVERIFY(result.completed.isEmpty());
    QCOMPARE(result.remaining, 1);
    QCOMPARE(readFile(source), QByteArrayLiteral("must survive"));
}

void FlightLogOrganizerTest::finalCancellationCallbackCannotMoveChangedSource()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString root = canonicalRoot(directory);
    const QString source = QDir(root).filePath(QStringLiteral("small.log"));
    QVERIFY(writeFile(source, QByteArrayLiteral("small")));
    const auto analysis = FlightLogOrganizer::Analyze(root);
    QVERIFY2(analysis.success, qPrintable(analysis.error));
    QCOMPARE(analysis.plan.entries().size(), 1);
    bool armed = false, changed = false;
    int checks = 0;
    const auto result = FlightLogOrganizer::Execute(analysis.plan,
        [&] {
            // Loop, snapshot entry, one read chunk, post-snapshot gate,
            // then the last callback immediately before rename.
            if (armed && ++checks == 5)
                changed = writeFile(source, QByteArrayLiteral("changed contents must stay here"));
            return false;
        }, [&](qint64 completed, qint64) {
            if (completed == 0) armed = true;
        });
    QVERIFY(changed);
    QVERIFY(!result.success);
    QVERIFY(!result.cancelled);
    QVERIFY(result.completed.isEmpty());
    QCOMPARE(result.remaining, 1);
    QCOMPARE(readFile(source), QByteArrayLiteral("changed contents must stay here"));
    QVERIFY(!QFile::exists(analysis.plan.entries().first().destination));
}

void FlightLogOrganizerTest::progressCanResetCallerPlan()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString root = canonicalRoot(directory);
    QVERIFY(writeFile(QDir(root).filePath(QStringLiteral("a.log")), "one"));
    QVERIFY(writeFile(QDir(root).filePath(QStringLiteral("z.log")), "two"));
    const auto analysis = FlightLogOrganizer::Analyze(root);
    QVERIFY2(analysis.success, qPrintable(analysis.error));
    QCOMPARE(analysis.plan.entries().size(), 2);

    FlightLogOrganizer::Plan callerPlan = analysis.plan;
    bool reset = false;
    const auto result = FlightLogOrganizer::Execute(
        callerPlan, {}, [&](qint64 completed, qint64) {
            if (!reset && completed == 0) {
                callerPlan = FlightLogOrganizer::Plan();
                reset = true;
            }
        });
    QVERIFY(reset);
    QVERIFY(!callerPlan.isValid());
    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(result.completed.size(), 2);
    QCOMPARE(result.remaining, 0);
}

void FlightLogOrganizerTest::progressCanReplaceCallerPlanAfterFirstOperation()
{
    QTemporaryDir originalDirectory;
    QTemporaryDir replacementDirectory;
    QVERIFY(originalDirectory.isValid());
    QVERIFY(replacementDirectory.isValid());
    const QString originalRoot = canonicalRoot(originalDirectory);
    const QString replacementRoot = canonicalRoot(replacementDirectory);
    QVERIFY(writeFile(QDir(originalRoot).filePath(QStringLiteral("a.log")), "one"));
    QVERIFY(writeFile(QDir(originalRoot).filePath(QStringLiteral("z.log")), "two"));
    const QString replacementSource = QDir(replacementRoot).filePath(
        QStringLiteral("replacement.log"));
    QVERIFY(writeFile(replacementSource, "replacement"));
    const auto original = FlightLogOrganizer::Analyze(originalRoot);
    const auto replacement = FlightLogOrganizer::Analyze(replacementRoot);
    QVERIFY2(original.success, qPrintable(original.error));
    QVERIFY2(replacement.success, qPrintable(replacement.error));
    QCOMPARE(original.plan.entries().size(), 2);

    FlightLogOrganizer::Plan callerPlan = original.plan;
    bool replaced = false;
    const auto result = FlightLogOrganizer::Execute(
        callerPlan, {}, [&](qint64 completed, qint64) {
            if (!replaced && completed == 1) {
                callerPlan = replacement.plan;
                replaced = true;
            }
        });
    QVERIFY(replaced);
    QVERIFY(callerPlan.isValid());
    QCOMPARE(callerPlan.root(), replacementRoot);
    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(result.completed.size(), 2);
    QCOMPARE(result.remaining, 0);
    QCOMPARE(readFile(replacementSource), QByteArray("replacement"));
}

void FlightLogOrganizerTest::symlinkSourcesAndDirectoriesAreSkipped()
{
    QTemporaryDir directory;
    QTemporaryDir outside;
    const QString root = canonicalRoot(directory);
    const QString outsideRoot = canonicalRoot(outside);
    const QString outsideLog = QDir(outsideRoot).filePath(QStringLiteral("outside.log"));
    QVERIFY(writeFile(outsideLog, "outside"));
    const QString linkedFile = QDir(root).filePath(QStringLiteral("linked.log"));
    const QString linkedDirectory = QDir(root).filePath(QStringLiteral("linked-dir"));
    if (!QFile::link(outsideLog, linkedFile)
        || !QFile::link(outsideRoot, linkedDirectory)) {
        QSKIP("Symbolic links are unavailable on this platform.");
    }

    const auto analysis = FlightLogOrganizer::Analyze(root);
    QVERIFY2(analysis.success, qPrintable(analysis.error));
    QCOMPARE(analysis.plan.candidateCount(), 0);
    QVERIFY(analysis.plan.entries().isEmpty());
    QVERIFY(analysis.plan.warnings().size() >= 2);
    QCOMPARE(readFile(outsideLog), QByteArray("outside"));
}

void FlightLogOrganizerTest::symlinkRootAndDestinationAreRefused()
{
    QTemporaryDir directory;
    QTemporaryDir aliases;
    QTemporaryDir outside;
    const QString root = canonicalRoot(directory);
    const QString alias = QDir(canonicalRoot(aliases)).filePath(QStringLiteral("root-link"));
    if (!QFile::link(root, alias))
        QSKIP("Directory symbolic links are unavailable on this platform.");
    auto analysis = FlightLogOrganizer::Analyze(alias);
    QVERIFY(!analysis.success);
    QVERIFY(!analysis.plan.isValid());

    const QString sourceDir = QDir(root).filePath(QStringLiteral("incoming"));
    QVERIFY(QDir().mkpath(sourceDir));
    const QString source = QDir(sourceDir).filePath(QStringLiteral("small.log"));
    QVERIFY(writeFile(source, "x"));
    const QString destinationAlias = QDir(root).filePath(QStringLiteral("SMALL"));
    QVERIFY(QFile::link(canonicalRoot(outside), destinationAlias));
    analysis = FlightLogOrganizer::Analyze(root);
    QVERIFY(!analysis.success);
    QVERIFY(QFileInfo::exists(source));
    QVERIFY(QDir(canonicalRoot(outside)).entryList(QDir::Files).isEmpty());
}

void FlightLogOrganizerTest::broadRootsAndInvalidPlansAreRefused()
{
    auto filesystem = FlightLogOrganizer::Analyze(QDir::rootPath());
    QVERIFY(!filesystem.success);
    auto home = FlightLogOrganizer::Analyze(QDir::homePath());
    QVERIFY(!home.success);
    FlightLogOrganizer::Plan invalid;
    const auto result = FlightLogOrganizer::Execute(invalid);
    QVERIFY(!result.success);
    QVERIFY(!result.error.isEmpty());
}

void FlightLogOrganizerTest::mutationDuringClassifierProgressInvalidatesAnalysis()
{
    QTemporaryDir directory;
    const QString root = canonicalRoot(directory);
    const QString source = QDir(root).filePath(QStringLiteral("changing.log"));
    const QByteArray original("first");
    QVERIFY(writeFile(source, original));
    bool changed = false;
    int progressCalls = 0;
    const auto analysis = FlightLogOrganizer::Analyze(
        root, {}, [&](qint64 done, qint64 total) {
            // Second callback is the classifier's start, after the original
            // full snapshot, on the directory-wide (not per-file-byte) scale.
            if (++progressCalls == 2 && done == 0 && total == 1000) {
                changed = writeFile(source, QByteArray("other"));
            }
        });
    QVERIFY(changed);
    QVERIFY(!analysis.success);
    QVERIFY(!analysis.plan.isValid());
    QCOMPARE(readFile(source), QByteArray("other"));
    QVERIFY(!QFileInfo::exists(QDir(root).filePath(
        QStringLiteral("SMALL/changing.log"))));
}

void FlightLogOrganizerTest::malformedCandidateIsLeftWhileOtherWorkIsPlanned()
{
    QTemporaryDir directory;
    const QString root = canonicalRoot(directory);
    const QString broken = QDir(root).filePath(QStringLiteral("broken.bin"));
    const QString usable = QDir(root).filePath(QStringLiteral("usable.log"));
    QVERIFY(writeFile(broken, QByteArray(2048, '\x55')));
    QVERIFY(writeFile(usable, QByteArray("small valid log")));
    const auto analysis = FlightLogOrganizer::Analyze(root);
    QVERIFY2(analysis.success, qPrintable(analysis.error));
    QVERIFY(!analysis.cancelled);
    QVERIFY(analysis.plan.isValid());
    QCOMPARE(analysis.plan.candidateCount(), 2);
    QVERIFY(!entryFor(analysis.plan, broken));
    QVERIFY(entryFor(analysis.plan, usable));
    QVERIFY(std::any_of(analysis.plan.warnings().cbegin(),
                        analysis.plan.warnings().cend(),
                        [](const QString &warning) {
        return warning.contains(QStringLiteral("broken.bin"));
    }));
    QCOMPARE(readFile(broken), QByteArray(2048, '\x55'));
}

QTEST_MAIN(FlightLogOrganizerTest)
#include "test_flightlogorganizer.moc"
