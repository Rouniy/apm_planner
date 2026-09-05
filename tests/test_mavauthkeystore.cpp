#include <QtTest>
#include "services/MavAuthKeyStore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>
#include <openssl/evp.h>
#include <openssl/crypto.h>
#include <array>
#include <memory>

namespace {
const QString Master = QStringLiteral("A strong test master passphrase 2026");

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.flush();
}

void appendU16(QByteArray &bytes, quint16 value)
{
    char encoded[2]; qToBigEndian(value, encoded); bytes.append(encoded, 2);
}
void appendU32(QByteArray &bytes, quint32 value)
{
    char encoded[4]; qToBigEndian(value, encoded); bytes.append(encoded, 4);
}

// Independent fixture construction for authenticated-but-invalid collections.
// It deliberately bypasses the production serializer and its validations.
QByteArray authenticatedFixture(const QByteArray &plain)
{
    QByteArray header("APMMAVK1", 8);
    appendU32(header, 600000);
    const QByteArray salt = QByteArray::fromHex("000102030405060708090a0b0c0d0e0f");
    const QByteArray nonce = QByteArray::fromHex("101112131415161718191a1b");
    header += salt; header += nonce; appendU32(header, quint32(plain.size()));
    std::array<unsigned char,32> key{};
    const auto passphrase = Master.toUtf8();
    if (PKCS5_PBKDF2_HMAC(passphrase.constData(), passphrase.size(),
                         reinterpret_cast<const unsigned char *>(salt.constData()), salt.size(),
                         600000, EVP_sha256(), int(key.size()), key.data()) != 1) return {};
    std::unique_ptr<EVP_CIPHER_CTX,decltype(&EVP_CIPHER_CTX_free)> cipher(EVP_CIPHER_CTX_new(),EVP_CIPHER_CTX_free);
    QByteArray encoded(plain.size()+16,'\0'), tag(16,'\0');
    int count=0,final=0,ignored=0;
    const bool ok = cipher && EVP_EncryptInit_ex(cipher.get(),EVP_aes_256_gcm(),nullptr,nullptr,nullptr)==1
        && EVP_CIPHER_CTX_ctrl(cipher.get(),EVP_CTRL_GCM_SET_IVLEN,nonce.size(),nullptr)==1
        && EVP_EncryptInit_ex(cipher.get(),nullptr,nullptr,key.data(),reinterpret_cast<const unsigned char *>(nonce.constData()))==1
        && EVP_EncryptUpdate(cipher.get(),nullptr,&ignored,reinterpret_cast<const unsigned char *>(header.constData()),header.size())==1
        && EVP_EncryptUpdate(cipher.get(),reinterpret_cast<unsigned char *>(encoded.data()),&count,reinterpret_cast<const unsigned char *>(plain.constData()),plain.size())==1
        && EVP_EncryptFinal_ex(cipher.get(),reinterpret_cast<unsigned char *>(encoded.data())+count,&final)==1
        && EVP_CIPHER_CTX_ctrl(cipher.get(),EVP_CTRL_GCM_GET_TAG,16,tag.data())==1;
    OPENSSL_cleanse(key.data(),key.size());
    if (!ok) return {};
    encoded.resize(count+final);
    return header+encoded+tag;
}
}

class MavAuthKeyStoreTest : public QObject
{
    Q_OBJECT
private slots:
    void derivationAndInputValidation();
    void roundTripPrivateCiphertextAndLock();
    void nonAsciiNamesAndSecrets();
    void duplicateNamesAndTransactionalDeletion();
    void wrongPasswordPreservesFile();
    void tampering_data();
    void tampering();
    void missingCorruptAndOversizedFiles();
    void authenticatedInvalidCollections_data();
    void authenticatedInvalidCollections();
    void writerConflictAndAlias();
    void failedMutationPreservesActiveData();
    void boundsAndMaximumCollection();
    void symlinksAndDirectories();
};

void MavAuthKeyStoreTest::derivationAndInputValidation()
{
    QByteArray key; QString error;
    QVERIFY2(MavAuthKeyStore::deriveSigningKey("abc",&key,&error),qPrintable(error));
    QCOMPARE(key.toHex(),QByteArray("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    const auto reference=key;
    QVERIFY(MavAuthKeyStore::deriveSigningKey(" abc ",&key,&error)); QVERIFY(key!=reference);
    for (const auto &seed : {QString(),QString("   "),QString(4097,'a'),QString(2049,QChar(0x0430)),QString(QChar(0xd800)),QString("a")+QChar(0)+"b"}) {
        key="old secret"; QVERIFY(!MavAuthKeyStore::deriveSigningKey(seed,&key,&error));
        QVERIFY(key.isEmpty()); QVERIFY(!error.isEmpty());
    }
    QVERIFY(!MavAuthKeyStore::deriveSigningKey("abc",nullptr,&error));
    QVERIFY(MavAuthKeyStore::deriveSigningKey(QString(4096,'a'),&key,&error)); QCOMPARE(key.size(),32);
}

void MavAuthKeyStoreTest::roundTripPrivateCiphertextAndLock()
{
    QTemporaryDir dir; QVERIFY(dir.isValid()); const QString path=dir.filePath("vault.keys");
    MavAuthKeyStore store(path); QString error; QByteArray key;
    QVERIFY(!store.isUnlocked()); QVERIFY(!store.addSeed("alpha","abc",&error));
    QVERIFY2(store.create(Master,&error),qPrintable(error));
    QVERIFY(store.isUnlocked()); QVERIFY(store.keyNames().isEmpty()); QVERIFY(QFileInfo::exists(path+".lock"));
    const auto initial=readFile(path); QCOMPARE(initial.left(8),QByteArray("APMMAVK1"));
    QCOMPARE(qFromBigEndian<quint32>(reinterpret_cast<const unsigned char *>(initial.constData())+8),quint32(600000));
    QVERIFY(store.addSeed("alpha private name","abc private seed",&error));
    QVERIFY(store.key("alpha private name",&key,&error)); QCOMPARE(key.size(),32);
    const auto saved=readFile(path);
    QVERIFY(!saved.contains("alpha private name")); QVERIFY(!saved.contains("abc private seed"));
    QVERIFY(!saved.contains(Master.toUtf8())); QVERIFY(!saved.contains(key));
    QCOMPARE(saved.mid(12,16),initial.mid(12,16)); QVERIFY(saved.mid(28,12)!=initial.mid(28,12));
    const auto permissions=QFile::permissions(path);
    QVERIFY(permissions.testFlag(QFileDevice::ReadOwner)); QVERIFY(permissions.testFlag(QFileDevice::WriteOwner));
#ifndef Q_OS_WIN
    QVERIFY(!(permissions & (QFileDevice::ReadGroup|QFileDevice::WriteGroup|QFileDevice::ExeGroup|QFileDevice::ReadOther|QFileDevice::WriteOther|QFileDevice::ExeOther|QFileDevice::ExeOwner)));
#endif
    store.lock(); QVERIFY(!store.isUnlocked()); QVERIFY(store.keyNames().isEmpty()); QVERIFY(!QFileInfo::exists(path+".lock"));
    QByteArray stale="old secret"; QVERIFY(!store.key("alpha private name",&stale,&error)); QVERIFY(stale.isEmpty());
    QVERIFY(!store.removeKey("alpha private name",&error)); QCOMPARE(readFile(path),saved);
    QVERIFY2(store.unlock(Master,&error),qPrintable(error));
    QByteArray loaded; QVERIFY(store.key("alpha private name",&loaded,&error)); QCOMPARE(loaded,key);
    QCOMPARE(store.keyNames(),QStringList({"alpha private name"}));
    const auto files=QDir(dir.path()).entryList(QDir::Files|QDir::Hidden|QDir::NoDotAndDotDot);
    QCOMPARE(files,QStringList({"vault.keys","vault.keys.lock"}));
}

void MavAuthKeyStoreTest::nonAsciiNamesAndSecrets()
{
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const auto path=dir.filePath(QString::fromUtf8("ключи-дрона.keys"));
    const auto password=QString::fromUtf8("секретный мастер-пароль ✈ 2026");
    const auto name=QString::fromUtf8("Дрон ✈️ один"); const auto seed=QString::fromUtf8("непубличное зерно 🔑");
    MavAuthKeyStore store(path); QString error;
    QVERIFY2(store.create(password,&error),qPrintable(error)); QVERIFY(store.addSeed(name,seed,&error));
    QByteArray expected=QCryptographicHash::hash(seed.toUtf8(),QCryptographicHash::Sha256),key;
    QVERIFY(store.key(name,&key,&error)); QCOMPARE(key,expected);
    store.lock(); QVERIFY(store.unlock(password,&error)); QCOMPARE(store.keyNames(),QStringList({name}));
    QVERIFY(store.key(name,&key,&error)); QCOMPARE(key,expected);
}

void MavAuthKeyStoreTest::duplicateNamesAndTransactionalDeletion()
{
    QTemporaryDir dir; QVERIFY(dir.isValid()); const auto path=dir.filePath("vault");
    MavAuthKeyStore store(path); QString error;
    QVERIFY(store.create(Master,&error)); QVERIFY(store.addSeed("alpha","seed one",&error));
    const auto before=readFile(path); QByteArray first; QVERIFY(store.key("alpha",&first,&error));
    QVERIFY(!store.addSeed("alpha","seed two",&error)); QVERIFY(error.contains("already")); QCOMPARE(readFile(path),before);
    QVERIFY(store.addSeed("Alpha","seed two",&error)); QCOMPARE(store.keyNames(),QStringList({"Alpha","alpha"}));
    QVERIFY(!store.removeKey("absent",&error)); QVERIFY(store.removeKey("alpha",&error));
    store.lock(); QVERIFY(store.unlock(Master,&error)); QCOMPARE(store.keyNames(),QStringList({"Alpha"}));
    QByteArray key; QVERIFY(!store.key("alpha",&key,&error)); QVERIFY(key.isEmpty());
    QVERIFY(store.removeKey("Alpha",&error)); store.lock(); QVERIFY(store.unlock(Master,&error)); QVERIFY(store.keyNames().isEmpty());
}

void MavAuthKeyStoreTest::wrongPasswordPreservesFile()
{
    QTemporaryDir dir; QVERIFY(dir.isValid()); const auto path=dir.filePath("vault");
    MavAuthKeyStore store(path); QString error;
    QVERIFY(store.create(Master,&error)); QVERIFY(store.addSeed("important","seed",&error)); store.lock();
    const auto before=readFile(path);
    QVERIFY(!store.unlock("a sufficiently long wrong passphrase",&error)); QVERIFY(error.contains("incorrect"));
    QVERIFY(!store.isUnlocked()); QVERIFY(store.keyNames().isEmpty());
    QVERIFY(!store.addSeed("new","seed",&error)); QVERIFY(!store.create(Master,&error));
    QCOMPARE(readFile(path),before); QVERIFY(!QFileInfo::exists(path+".lock"));
    QVERIFY2(store.unlock(Master,&error),qPrintable(error)); QCOMPARE(store.keyNames(),QStringList({"important"}));
}

void MavAuthKeyStoreTest::tampering_data()
{
    QTest::addColumn<int>("offset");
    QTest::newRow("magic")<<0; QTest::newRow("kdf-cost")<<8;
    QTest::newRow("salt")<<12; QTest::newRow("nonce")<<28;
    QTest::newRow("cipher-length")<<40; QTest::newRow("ciphertext")<<44; QTest::newRow("tag")<<-1;
}

void MavAuthKeyStoreTest::tampering()
{
    QFETCH(int,offset); QTemporaryDir dir; QVERIFY(dir.isValid()); const auto path=dir.filePath("vault");
    MavAuthKeyStore store(path); QString error;
    QVERIFY(store.create(Master,&error)); QVERIFY(store.addSeed("alpha","seed",&error)); store.lock();
    auto encoded=readFile(path); const int actual=offset<0 ? encoded.size()-1 : offset;
    encoded[actual]=char(uchar(encoded[actual])^0x40); QVERIFY(writeFile(path,encoded));
    QVERIFY(!store.unlock(Master,&error)); QVERIFY(!error.isEmpty()); QVERIFY(!store.isUnlocked());
    QVERIFY(!store.create(Master,&error)); QCOMPARE(readFile(path),encoded);
}

void MavAuthKeyStoreTest::missingCorruptAndOversizedFiles()
{
    QTemporaryDir dir; QVERIFY(dir.isValid()); const auto path=dir.filePath("vault");
    MavAuthKeyStore store(path); QString error;
    QVERIFY(!store.unlock(Master,&error)); QVERIFY(!QFileInfo::exists(path)); QVERIFY(!QFileInfo::exists(path+".lock"));
    for (const auto &bytes : {QByteArray(),QByteArray("broken encryption"),QByteArray(65537,'x'),QByteArray(100,'x')}) {
        QVERIFY(writeFile(path,bytes)); QVERIFY(!store.unlock(Master,&error)); QVERIFY(!store.create(Master,&error));
        QCOMPARE(readFile(path),bytes); QVERIFY(!store.isUnlocked());
    }
    MavAuthKeyStore missingDirectory(dir.filePath("not-created/vault"));
    QVERIFY(!missingDirectory.create(Master,&error)); QVERIFY(!QFileInfo::exists(dir.filePath("not-created")));
    MavAuthKeyStore relative("relative-vault"); QVERIFY(!relative.create(Master,&error));
}

void MavAuthKeyStoreTest::authenticatedInvalidCollections_data()
{
    QTest::addColumn<QByteArray>("plain");
    QByteArray zero; appendU16(zero,1); appendU16(zero,1); zero+='A'; zero+=QByteArray(32,'\0');
    QTest::newRow("zero-key")<<zero;
    QByteArray duplicate; appendU16(duplicate,2);
    for (int i=0;i<2;++i) { appendU16(duplicate,1); duplicate+='A'; duplicate+=QByteArray(32,char(i+1)); }
    QTest::newRow("duplicate-name")<<duplicate;
    QByteArray invalid; appendU16(invalid,1); appendU16(invalid,1); invalid+=char(0xff); invalid+=QByteArray(32,'a');
    QTest::newRow("invalid-utf8-name")<<invalid;
    QByteArray many; appendU16(many,129); QTest::newRow("excessive-count")<<many;
    QByteArray tail; appendU16(tail,0); tail+='x'; QTest::newRow("trailing-bytes")<<tail;
    QByteArray shortKey; appendU16(shortKey,1); appendU16(shortKey,1); shortKey+='A'; shortKey+=QByteArray(31,'a');
    QTest::newRow("short-key")<<shortKey;
}

void MavAuthKeyStoreTest::authenticatedInvalidCollections()
{
    QFETCH(QByteArray,plain); QTemporaryDir dir; QVERIFY(dir.isValid()); const auto path=dir.filePath("vault");
    const auto fixture=authenticatedFixture(plain); QVERIFY(!fixture.isEmpty()); QVERIFY(writeFile(path,fixture));
    MavAuthKeyStore store(path); QString error;
    QVERIFY(!store.unlock(Master,&error)); QVERIFY(error.contains("collection")); QVERIFY(!store.isUnlocked());
    QVERIFY(!store.create(Master,&error)); QCOMPARE(readFile(path),fixture);
}

void MavAuthKeyStoreTest::writerConflictAndAlias()
{
    QTemporaryDir dir; QVERIFY(dir.isValid()); const auto path=dir.filePath("vault");
    MavAuthKeyStore first(path),second(path),alias(dir.path()+"/./vault"); QString error;
    QVERIFY(first.create(Master,&error)); QVERIFY(!second.unlock(Master,&error)); QVERIFY(error.contains("locked"));
    QVERIFY(!alias.unlock(Master,&error)); QVERIFY(!second.create(Master,&error));
    first.lock(); QVERIFY(second.unlock(Master,&error)); QVERIFY(!first.unlock(Master,&error));
    second.lock(); QVERIFY(alias.unlock(Master,&error));
}

void MavAuthKeyStoreTest::failedMutationPreservesActiveData()
{
    QTemporaryDir dir; QVERIFY(dir.isValid()); const auto path=dir.filePath("vault");
    MavAuthKeyStore store(path); QString error;
    QVERIFY(store.create(Master,&error)); QVERIFY(store.addSeed("alpha","seed",&error));
    QByteArray key; QVERIFY(store.key("alpha",&key,&error)); const auto encrypted=readFile(path);
    // A non-cooperating external writer is detected before replacement.
    auto changed=encrypted; changed[44]=char(uchar(changed[44])^1); QVERIFY(writeFile(path,changed));
    QVERIFY(!store.addSeed("beta","other seed",&error)); QVERIFY(!store.removeKey("alpha",&error));
    QCOMPARE(store.keyNames(),QStringList({"alpha"})); QByteArray after; QVERIFY(store.key("alpha",&after,&error)); QCOMPARE(after,key);
    QCOMPARE(readFile(path),changed);
    // An unusable destination cannot publish either the file or active changes.
    QVERIFY(QFile::rename(path,dir.filePath("prior-vault"))); QVERIFY(QDir().mkdir(path));
    QVERIFY(!store.addSeed("beta","seed",&error)); QVERIFY(!store.removeKey("alpha",&error));
    QCOMPARE(store.keyNames(),QStringList({"alpha"})); QCOMPARE(readFile(dir.filePath("prior-vault")),changed);
    QVERIFY(QDir().rmdir(path)); QVERIFY(writeFile(path,encrypted));
    QVERIFY(store.addSeed("beta","other seed",&error)); QCOMPARE(store.keyNames(),QStringList({"alpha","beta"}));
}

void MavAuthKeyStoreTest::boundsAndMaximumCollection()
{
    QTemporaryDir dir; QVERIFY(dir.isValid()); const auto path=dir.filePath("vault");
    MavAuthKeyStore store(path); QString error;
    for (const auto &password : {QString(),QString(11,'a'),QString(1025,'a'),QString(513,QChar(0x0430)),QString(20,' ')}) {
        QVERIFY(!store.create(password,&error)); QVERIFY(!QFileInfo::exists(path));
    }
    QVERIFY(store.create(QString(1024,'a'),&error));
    QVERIFY(!store.addSeed("","seed",&error)); QVERIFY(!store.addSeed(QString(129,'a'),"seed",&error));
    QVERIFY(!store.addSeed("bad\nname","seed",&error)); QVERIFY(!store.addSeed("alpha","",&error));
    QVERIFY(store.addSeed(QString(64,QChar(0x0430)),"seed",&error));
    QVERIFY(!store.addSeed(QString(65,QChar(0x0430)),"seed",&error));
    for (int i=1;i<128;++i) QVERIFY2(store.addSeed(QString("key %1").arg(i),"seed",&error),qPrintable(error));
    QCOMPARE(store.keyNames().size(),128); const auto before=readFile(path);
    QVERIFY(!store.addSeed("overflow","seed",&error)); QCOMPARE(readFile(path),before);
    QVERIFY(before.size()<=MavAuthKeyStore::MaximumFileBytes);
    store.lock(); QVERIFY(store.unlock(QString(1024,'a'),&error)); QCOMPARE(store.keyNames().size(),128);
}

void MavAuthKeyStoreTest::symlinksAndDirectories()
{
    QTemporaryDir dir; QVERIFY(dir.isValid()); QString error;
    MavAuthKeyStore directory(dir.path()); QVERIFY(!directory.create(Master,&error)); QVERIFY(!directory.unlock(Master,&error));
#ifndef Q_OS_WIN
    const auto real=dir.filePath("real"), link=dir.filePath("link");
    MavAuthKeyStore store(real); QVERIFY(store.create(Master,&error)); store.lock();
    const auto before=readFile(real); QVERIFY(QFile::link(real,link));
    MavAuthKeyStore symbolic(link); QVERIFY(!symbolic.create(Master,&error)); QVERIFY(!symbolic.unlock(Master,&error));
    QCOMPARE(readFile(real),before);
    const auto dangling=dir.filePath("dangling"); QVERIFY(QFile::link(dir.filePath("missing"),dangling));
    MavAuthKeyStore absent(dangling); QVERIFY(!absent.create(Master,&error)); QVERIFY(!QFileInfo::exists(dir.filePath("missing")));
#endif
}

QTEST_GUILESS_MAIN(MavAuthKeyStoreTest)
#include "test_mavauthkeystore.moc"
