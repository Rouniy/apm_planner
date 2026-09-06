#include "comm/RemoteDataFlashLogWriter.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>
#include <cstring>

namespace {
QByteArray block(char value) { return QByteArray(RemoteDataFlashLogWriter::BlockBytes, value); }
QByteArray read(const QString &path) {
    QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
bool write(const QString &path, const QByteArray &data) {
    QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}
QStringList entries(const QString &path) {
    return QDir(path).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
}
RemoteDataFlashLogWriter::Result result(const QSignalSpy &spy, int index = 0) {
    return qvariant_cast<RemoteDataFlashLogWriter::Result>(spy.at(index).at(1));
}
}

class RemoteDataFlashLogWriterTest final : public QObject {
    Q_OBJECT
private slots:
    void outOfOrderAndIdenticalDuplicate();
    void conflictingDuplicateNeverPublishes();
    void partialSaveZerosAndReportsHoles();
    void discardKeepsStatsAndRemovesStaging();
    void invalidAdmissionAndLimits_data();
    void invalidAdmissionAndLimits();
    void boundedQueueFailureAndDiscardIdempotence();
    void publicationNeverOverwrites();
    void destructorPreservesPendingCapture();
    void openErrorsAndEmptySave();
    void generationsAndReentrantLifecycle();
    void immutableRawDataInput();
    void unexpectedStopPreservesPartial();
    void deletionDuringNotifications_data();
    void deletionDuringNotifications();
};

void RemoteDataFlashLogWriterTest::outOfOrderAndIdenticalDuplicate()
{
    QTemporaryDir dir; RemoteDataFlashLogWriter writer;
    QSignalSpy opened(&writer,&RemoteDataFlashLogWriter::opened);
    QSignalSpy stored(&writer,&RemoteDataFlashLogWriter::blockStored);
    QSignalSpy finished(&writer,&RemoteDataFlashLogWriter::finished);
    bool ownerThread=true;
    connect(&writer,&RemoteDataFlashLogWriter::blockStored,&writer,[&] { ownerThread &= QThread::currentThread()==writer.thread(); });
    QVERIFY(writer.open(dir.path(),"flight",7)); QVERIFY(writer.busy());
    QVERIFY(!writer.open(dir.path(),"other",8)); QVERIFY(!writer.append(0,block('a')));
    QTRY_COMPARE(opened.size(),1); QCOMPARE(opened[0][0].toULongLong(),quint64(7));
    QVERIFY(QFileInfo(opened[0][1].toString()).exists());
    QVERIFY(writer.append(0,block('a'))); QVERIFY(writer.append(2,block('c')));
    QVERIFY(writer.append(1,block('b'))); QVERIFY(writer.append(2,block('c')));
    QVERIFY(writer.finish(true)); QVERIFY(!writer.append(3,block('d')));
    QTRY_COMPARE(finished.size(),1); QCOMPARE(stored.size(),4); QVERIFY(ownerThread);
    QCOMPARE(stored[0][1].toUInt(),quint32(0)); QCOMPARE(stored[1][1].toUInt(),quint32(2));
    QVERIFY(stored[3][2].toBool());
    const auto r=result(finished); QVERIFY2(r.success,qPrintable(r.error)); QVERIFY(r.published); QVERIFY(!writer.busy());
    QCOMPARE(r.blocks,qint64(3)); QCOMPARE(r.duplicateBlocks,qint64(1)); QCOMPARE(r.bytes,qint64(600));
    QCOMPARE(r.missingBlocks,qint64(0)); QVERIFY(r.missingRanges.isEmpty());
    QVERIFY(!r.path.endsWith(".partial.bin")); QCOMPARE(read(r.path),block('a')+block('b')+block('c'));
    QCOMPARE(entries(dir.path()).size(),1); QVERIFY(!QFileInfo(opened[0][1].toString()).exists());
    QVERIFY(!r.warnings.isEmpty());
}

void RemoteDataFlashLogWriterTest::conflictingDuplicateNeverPublishes()
{
    QTemporaryDir dir; RemoteDataFlashLogWriter writer;
    QSignalSpy opened(&writer,&RemoteDataFlashLogWriter::opened), stored(&writer,&RemoteDataFlashLogWriter::blockStored);
    QSignalSpy failed(&writer,&RemoteDataFlashLogWriter::failed), finished(&writer,&RemoteDataFlashLogWriter::finished);
    QVERIFY(writer.open(dir.path(),"conflict",1)); QTRY_COMPARE(opened.size(),1);
    QVERIFY(writer.append(0,block('a'))); QTRY_COMPARE(stored.size(),1);
    QVERIFY(writer.append(0,block('b'))); QVERIFY(writer.finish(true));
    QTRY_COMPARE(finished.size(),1); QCOMPARE(failed.size(),1);
    const auto r=result(finished); QVERIFY(!r.success); QVERIFY(!r.published); QVERIFY(r.path.endsWith(".part"));
    QVERIFY(r.error.contains("Conflicting")); QCOMPARE(r.blocks,qint64(1)); QCOMPARE(stored.size(),1);
    QCOMPARE(read(r.path),block('a')); QCOMPARE(entries(dir.path()).size(),1);
}

void RemoteDataFlashLogWriterTest::partialSaveZerosAndReportsHoles()
{
    QTemporaryDir dir; RemoteDataFlashLogWriter writer;
    QSignalSpy opened(&writer,&RemoteDataFlashLogWriter::opened), finished(&writer,&RemoteDataFlashLogWriter::finished);
    QVERIFY(writer.open(dir.path(),"gaps",2)); QTRY_COMPARE(opened.size(),1);
    QVERIFY(writer.append(2,block('c'))); QVERIFY(writer.append(4,block('e'))); QVERIFY(writer.finish(true));
    QTRY_COMPARE(finished.size(),1); const auto r=result(finished);
    QVERIFY2(r.success,qPrintable(r.error)); QCOMPARE(r.blocks,qint64(2)); QCOMPARE(r.bytes,qint64(1000));
    QCOMPARE(r.missingBlocks,qint64(3)); QCOMPARE(r.highestSequence,quint32(4)); QCOMPARE(r.missingRanges.size(),2);
    QCOMPARE(r.missingRanges[0].first,quint32(0)); QCOMPARE(r.missingRanges[0].last,quint32(1));
    QCOMPARE(r.missingRanges[1].first,quint32(3)); QCOMPARE(r.missingRanges[1].last,quint32(3));
    QVERIFY(r.path.endsWith(".partial.bin")); QCOMPARE(read(r.path),block(0)+block(0)+block('c')+block(0)+block('e'));
    QVERIFY(r.warnings.join(' ').contains("zero-filled"));
}

void RemoteDataFlashLogWriterTest::discardKeepsStatsAndRemovesStaging()
{
    QTemporaryDir dir; RemoteDataFlashLogWriter writer;
    QSignalSpy opened(&writer,&RemoteDataFlashLogWriter::opened), stored(&writer,&RemoteDataFlashLogWriter::blockStored);
    QSignalSpy finished(&writer,&RemoteDataFlashLogWriter::finished), failed(&writer,&RemoteDataFlashLogWriter::failed);
    QVERIFY(writer.open(dir.path(),"discard",3)); QTRY_COMPARE(opened.size(),1);
    QVERIFY(writer.append(2,block('c'))); QTRY_COMPARE(stored.size(),1);
    QVERIFY(writer.finish(false)); QTRY_COMPARE(finished.size(),1);
    const auto r=result(finished); QVERIFY(r.cancelled); QVERIFY(!r.published); QCOMPARE(failed.size(),0);
    QCOMPARE(r.blocks,qint64(1)); QCOMPARE(r.bytes,qint64(600)); QCOMPARE(r.missingBlocks,qint64(2));
    QVERIFY(entries(dir.path()).isEmpty());
}

void RemoteDataFlashLogWriterTest::invalidAdmissionAndLimits_data()
{
    QTest::addColumn<quint32>("sequence"); QTest::addColumn<int>("size"); QTest::addColumn<bool>("admitted");
    QTest::newRow("short")<<quint32(0)<<199<<false;
    QTest::newRow("long")<<quint32(0)<<201<<false;
    QTest::newRow("forward gap")<<quint32(4097)<<200<<true;
    QTest::newRow("file cap")<<quint32(RemoteDataFlashLogWriter::MaximumFileBytes/200)<<200<<false;
    QTest::newRow("uint32 max")<<quint32(0xffffffffu)<<200<<false;
}
void RemoteDataFlashLogWriterTest::invalidAdmissionAndLimits()
{
    QFETCH(quint32,sequence); QFETCH(int,size); QFETCH(bool,admitted);
    QTemporaryDir dir; RemoteDataFlashLogWriter writer;
    QSignalSpy opened(&writer,&RemoteDataFlashLogWriter::opened), finished(&writer,&RemoteDataFlashLogWriter::finished);
    QSignalSpy failed(&writer,&RemoteDataFlashLogWriter::failed);
    QVERIFY(writer.open(dir.path(),"limits",4)); QTRY_COMPARE(opened.size(),1);
    QCOMPARE(writer.append(sequence,QByteArray(size,'x')),admitted);
    QTRY_COMPARE(finished.size(),1); QCOMPARE(failed.size(),1);
    QVERIFY(!result(finished).published); QVERIFY(!result(finished).error.isEmpty());
    QVERIFY(entries(dir.path()).isEmpty());
}

void RemoteDataFlashLogWriterTest::boundedQueueFailureAndDiscardIdempotence()
{
    QTemporaryDir dir; RemoteDataFlashLogWriter writer;
    QSignalSpy opened(&writer,&RemoteDataFlashLogWriter::opened), finished(&writer,&RemoteDataFlashLogWriter::finished);
    QVERIFY(writer.open(dir.path(),"queue",5)); QTRY_COMPARE(opened.size(),1);
    // No owner-thread event dispatch: even if disk catches up, queued ACKs
    // count against admission, so the 257th outstanding block must fail.
    for(int i=0;i<256;++i) QVERIFY(writer.append(i,block('x')));
    QVERIFY(!writer.append(256,block('x')));
    for(int i=0;i<1000;++i) QVERIFY(writer.finish(false,true)); // Idempotent, no new queued stop jobs.
    QTRY_COMPARE(finished.size(),1); QVERIFY(result(finished).error.contains("queue"));
    QVERIFY(!result(finished).cancelled);
    const auto r=result(finished);
    if(r.bytes) { QVERIFY(r.path.endsWith(".part")); QCOMPARE(entries(dir.path()).size(),1); }
    else QVERIFY(entries(dir.path()).isEmpty());
}

void RemoteDataFlashLogWriterTest::publicationNeverOverwrites()
{
    QTemporaryDir dir; RemoteDataFlashLogWriter writer;
    QSignalSpy opened(&writer,&RemoteDataFlashLogWriter::opened), finished(&writer,&RemoteDataFlashLogWriter::finished);
    QVERIFY(writer.open(dir.path(),"collision",6)); QTRY_COMPARE(opened.size(),1);
    const QFileInfo staging(opened[0][1].toString());
    // Template is .<stem>-<UUID>.<six random chars>.part; reserve the derived
    // final destination after opening to exercise the rename-time guard.
    QString base=staging.fileName().mid(1); base.chop(12);
    const QString destination=staging.dir().filePath(base+".bin");
    QVERIFY(write(destination,"existing data"));
    QVERIFY(writer.append(0,block('a'))); QVERIFY(writer.finish(true));
    QTRY_COMPARE(finished.size(),1); QVERIFY(!result(finished).success);
    QCOMPARE(read(destination),QByteArray("existing data")); QCOMPARE(entries(dir.path()).size(),2);
    QCOMPARE(read(result(finished).path),block('a')); QVERIFY(QFileInfo(staging.absoluteFilePath()).exists());
}

void RemoteDataFlashLogWriterTest::destructorPreservesPendingCapture()
{
    QTemporaryDir dir;
    {
        RemoteDataFlashLogWriter writer; QSignalSpy opened(&writer,&RemoteDataFlashLogWriter::opened);
        QSignalSpy stored(&writer,&RemoteDataFlashLogWriter::blockStored);
        QVERIFY(writer.open(dir.path(),"teardown",8)); QTRY_COMPARE(opened.size(),1);
        QVERIFY(writer.append(0,block('x'))); QTRY_COMPARE(stored.size(),1);
        for(int i=1;i<256;++i) QVERIFY(writer.append(i,block('x')));
    }
    QCOMPARE(entries(dir.path()).size(),1); QVERIFY(entries(dir.path()).first().endsWith(".part"));
    { RemoteDataFlashLogWriter writer; QVERIFY(writer.open(dir.path(),"early",9)); }
    QCOMPARE(entries(dir.path()).size(),1);
}

void RemoteDataFlashLogWriterTest::openErrorsAndEmptySave()
{
    QTemporaryDir dir; RemoteDataFlashLogWriter writer;
    QSignalSpy opened(&writer,&RemoteDataFlashLogWriter::opened), finished(&writer,&RemoteDataFlashLogWriter::finished);
    QVERIFY(!writer.open(dir.path(),"zero",0));
    QVERIFY(writer.open(dir.filePath("missing"),"name",1)); QTRY_COMPARE(finished.size(),1);
    QVERIFY(!QFileInfo(dir.filePath("missing")).exists()); QVERIFY(!writer.busy());
    QVERIFY(writer.open(dir.path(),"../unsafe",2)); QTRY_COMPARE(finished.size(),2);
    QCOMPARE(opened.size(),0);
    QVERIFY(writer.open(dir.path(),"empty",3)); QTRY_COMPARE(opened.size(),1);
    QVERIFY(writer.finish(true)); QTRY_COMPARE(finished.size(),3);
    QVERIFY(result(finished,2).error.contains("empty")); QVERIFY(entries(dir.path()).isEmpty());
}

void RemoteDataFlashLogWriterTest::generationsAndReentrantLifecycle()
{
    QTemporaryDir dir; auto *writer=new RemoteDataFlashLogWriter;
    QPointer<RemoteDataFlashLogWriter> guard(writer); int terminals=0; QVector<quint64> acknowledged;
    connect(writer,&RemoteDataFlashLogWriter::opened,this,[&](quint64 generation,const QString &) {
        QVERIFY(writer->append(0,block(char(generation)))); QVERIFY(writer->finish(true));
    });
    connect(writer,&RemoteDataFlashLogWriter::blockStored,this,[&](quint64 generation,quint32,bool) { acknowledged.append(generation); });
    connect(writer,&RemoteDataFlashLogWriter::finished,this,[&](quint64,const RemoteDataFlashLogWriter::Result &r) {
        QVERIFY(r.success); ++terminals;
        if(terminals==1) QVERIFY(writer->open(dir.path(),"second",10)); // External generation reuse still has a new internal epoch.
        else delete writer;
    });
    QVERIFY(writer->open(dir.path(),"first",10));
    QTRY_VERIFY(!guard); QCOMPARE(terminals,2); QCOMPARE(acknowledged,QVector<quint64>({10,10}));
    QCOMPARE(entries(dir.path()).size(),2);
    auto *failureWriter=new RemoteDataFlashLogWriter;
    QPointer<RemoteDataFlashLogWriter> failureGuard(failureWriter);
    connect(failureWriter,&RemoteDataFlashLogWriter::failed,this,[&] { delete failureWriter; });
    QVERIFY(failureWriter->open(dir.path(),"../bad",12)); QTRY_VERIFY(!failureGuard);
}

void RemoteDataFlashLogWriterTest::immutableRawDataInput()
{
    QTemporaryDir dir; RemoteDataFlashLogWriter writer;
    QSignalSpy opened(&writer,&RemoteDataFlashLogWriter::opened), finished(&writer,&RemoteDataFlashLogWriter::finished);
    QVERIFY(writer.open(dir.path(),"copy",13)); QTRY_COMPARE(opened.size(),1);
    char storage[200]; std::memset(storage,'a',sizeof(storage));
    const auto bytes=QByteArray::fromRawData(storage,200); QVERIFY(writer.append(0,bytes));
    std::memset(storage,'b',sizeof(storage)); QVERIFY(writer.finish(true));
    QTRY_COMPARE(finished.size(),1); QVERIFY(result(finished).success); QCOMPARE(read(result(finished).path),block('a'));
}

void RemoteDataFlashLogWriterTest::unexpectedStopPreservesPartial()
{
    QTemporaryDir dir; RemoteDataFlashLogWriter writer;
    QSignalSpy opened(&writer,&RemoteDataFlashLogWriter::opened), stored(&writer,&RemoteDataFlashLogWriter::blockStored);
    QSignalSpy finished(&writer,&RemoteDataFlashLogWriter::finished);
    QVERIFY(writer.open(dir.path(),"lost_link",14)); QTRY_COMPARE(opened.size(),1);
    QVERIFY(writer.append(2,block('c'))); QTRY_COMPARE(stored.size(),1);
    QVERIFY(writer.finish(false,true)); QTRY_COMPARE(finished.size(),1);
    const auto r=result(finished); QVERIFY(!r.success); QVERIFY(!r.published); QVERIFY(!r.cancelled);
    QVERIFY(r.path.endsWith(".part")); QCOMPARE(r.blocks,qint64(1)); QCOMPARE(r.missingBlocks,qint64(2));
    QCOMPARE(r.bytes,qint64(600)); QCOMPARE(read(r.path).mid(400),block('c')); QVERIFY(!r.warnings.isEmpty());
}

void RemoteDataFlashLogWriterTest::deletionDuringNotifications_data()
{
    QTest::addColumn<bool>("afterBlock");
    QTest::newRow("opened")<<false; QTest::newRow("stored")<<true;
}
void RemoteDataFlashLogWriterTest::deletionDuringNotifications()
{
    QFETCH(bool,afterBlock); QTemporaryDir dir;
    auto *writer=new RemoteDataFlashLogWriter; QPointer<RemoteDataFlashLogWriter> guard(writer);
    connect(writer,&RemoteDataFlashLogWriter::opened,this,[&] {
        if(afterBlock) QVERIFY(writer->append(0,block('x')));
        else delete writer;
    });
    connect(writer,&RemoteDataFlashLogWriter::blockStored,this,[&] { delete writer; });
    QVERIFY(writer->open(dir.path(),"delete_callback",15)); QTRY_VERIFY(!guard);
    if(afterBlock) {
        const auto files=entries(dir.path()); QCOMPARE(files.size(),1);
        QCOMPARE(read(dir.filePath(files.first())),block('x'));
    }
    else QVERIFY(entries(dir.path()).isEmpty());
}

QTEST_GUILESS_MAIN(RemoteDataFlashLogWriterTest)
#include "test_remotedataflashlogwriter.moc"
