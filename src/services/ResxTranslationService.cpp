#include "ResxTranslationService.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QResource>
#include <QSaveFile>
#include <QSet>
#include <QBuffer>
#include <QTextStream>
#include <QUuid>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <algorithm>

static void initializeResxCultureResource() { Q_INIT_RESOURCE(resx_cultures); }

namespace {
using Service = ResxTranslationService;
QString normalized(QString value) { return value.replace('\\', '/'); }
QString pathKey(const QString &value) { return normalized(value).toCaseFolded(); }
QString pathOrderKey(const QString &value) {
    // .NET ordinal ignore-case orders ASCII by uppercase, not Qt's lowercase
    // fold (notably letters precede '_'). Unicode identity differences are
    // tracked separately; all reference resource paths are ASCII.
    QString result = pathKey(value);
    for (int i = 0; i < result.size(); ++i)
        if (result.at(i) >= 'a' && result.at(i) <= 'z')
            result[i] = QChar(result.at(i).unicode() - 'a' + 'A');
    return result;
}
bool fail(QString *error, const QString &message) { if (error) *error = message; return false; }
bool cancelled(const Service::Cancel &cancel, bool *flag, QString *error) {
    if (!cancel || !cancel()) return false;
    *flag = true; if (error) *error = QStringLiteral("Translation operation cancelled."); return true;
}
void warning(QStringList *list, const QString &value) {
    if (list->size() < 1000) list->append(value);
    else if (list->size() == 1000) list->append(QStringLiteral("Additional translation warnings omitted."));
}
struct Catalog { QVector<TranslationCulture> values; QHash<QString, QString> names; QString error; };
const Catalog &catalog() {
    static const Catalog value = [] {
        initializeResxCultureResource();
        Catalog result; QFile file(QStringLiteral(":/translations/dotnet10-cultures.json"));
        if (!file.open(QIODevice::ReadOnly) || file.size() > 4 * 1024 * 1024) {
            result.error = QStringLiteral("The bundled .NET culture catalog is unavailable."); return result;
        }
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(file.readAll(), &error);
        if (error.error != QJsonParseError::NoError || !document.isArray() || document.array().size() > 10000) {
            result.error = QStringLiteral("The bundled .NET culture catalog is invalid."); return result;
        }
        for (const auto &item : document.array()) {
            const auto object = item.toObject();
            const auto name = object.value(QStringLiteral("name")).toString();
            const auto display = object.value(QStringLiteral("displayName")).toString();
            if (name.isEmpty() || name.size() > 128 || display.isEmpty() || name.contains('/') || name.contains('\\')) {
                result.error = QStringLiteral("The bundled .NET culture catalog contains an invalid entry.");
                result.values.clear(); result.names.clear(); return result;
            }
            if (!result.names.contains(name.toCaseFolded())) {
                result.values.append({name, display}); result.names.insert(name.toCaseFolded(), name);
            }
        }
        if (result.values.isEmpty()) result.error = QStringLiteral("The bundled .NET culture catalog is empty.");
        return result;
    }();
    return value;
}
QString cultureName(const QString &requested, QString *error) {
    const auto &data = catalog();
    if (!data.error.isEmpty()) { fail(error, data.error); return {}; }
    const auto name = data.names.value(requested.trimmed().toCaseFolded());
    if (name.isEmpty()) fail(error, QStringLiteral("Unknown or invariant target culture: %1").arg(requested));
    return name;
}
bool safeRelative(const QString &requested, QString *error) {
    const auto value = normalized(requested);
    if (value.isEmpty() || value.size() > 4096 || QDir::isAbsolutePath(value))
        return fail(error, QStringLiteral("Resource path must be a bounded safe relative path."));
    const auto parts = value.split('/');
    if (parts.size() > Service::MaximumDepth)
        return fail(error, QStringLiteral("Resource path exceeds the directory depth limit."));
    for (const auto &part : parts) {
        if (part.isEmpty() || part == "." || part == ".." || part.endsWith('.') || part.endsWith(' '))
            return fail(error, QStringLiteral("Resource path contains an unsafe or ambiguous segment."));
        for (const auto character : part)
            if (character.unicode() < 32 || QStringLiteral(":*?\"<>|").contains(character))
                return fail(error, QStringLiteral("Resource path contains a non-portable character."));
        const auto stem = part.section('.', 0, 0).toUpper();
        if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL"
            || (stem.size() == 4 && (stem.startsWith("COM") || stem.startsWith("LPT"))
                && stem.at(3) >= '1' && stem.at(3) <= '9'))
            return fail(error, QStringLiteral("Resource path uses a reserved device name."));
    }
    return true;
}
struct Root { QString path; QDateTime born; };
bool samePath(const QString &left, const QString &right) {
#ifdef Q_OS_WIN
    return left.compare(right, Qt::CaseInsensitive) == 0;
#else
    return left == right;
#endif
}
bool rootCurrent(const Root &root, QString *error) {
    const QFileInfo info(root.path);
    if (!info.isDir() || info.isSymLink() || !samePath(info.canonicalFilePath(), root.path)
        || (root.born.isValid() && info.birthTime() != root.born))
        return fail(error, QStringLiteral("The selected directory changed or became linked: %1").arg(root.path));
    return true;
}
bool admitRoot(const QString &requested, bool create, Root *root, QString *error) {
    if (requested.trimmed().isEmpty() || requested.contains(QChar(0)))
        return fail(error, QStringLiteral("Choose a resource directory."));
    QFileInfo info(QDir::cleanPath(QFileInfo(requested).absoluteFilePath()));
    if (info.isSymLink()) return fail(error, QStringLiteral("A symbolic-link selected directory is not allowed."));
    QStringList missing;
    while (!info.exists()) {
        if (!create || missing.size() >= Service::MaximumDepth)
            return fail(error, QStringLiteral("Resource directory does not exist or exceeds the depth limit."));
        missing.prepend(info.fileName());
        const QString parent = info.absolutePath();
        if (parent == info.absoluteFilePath()) return fail(error, QStringLiteral("Cannot resolve the output directory."));
        info.setFile(parent);
    }
    if (!info.isDir()) return fail(error, QStringLiteral("The resource directory path is a file."));
    QString path = info.canonicalFilePath();
    if (path.isEmpty()) return fail(error, QStringLiteral("Cannot resolve the resource directory."));
    for (const auto &part : missing) {
        path = QDir(path).filePath(part);
        if (!QDir().mkdir(path)) return fail(error, QStringLiteral("Cannot create output directory: %1").arg(path));
        const QFileInfo created(path);
        if (!created.isDir() || created.isSymLink() || !samePath(created.canonicalFilePath(), path))
            return fail(error, QStringLiteral("The new output directory changed during creation."));
    }
    root->path = QDir::cleanPath(path); root->born = QFileInfo(path).birthTime();
    return rootCurrent(*root, error);
}
bool contained(const Root &root, const QString &relative, bool createParents, QString *path, QString *error) {
    if (!safeRelative(relative, error) || !rootCurrent(root, error)) return false;
    const auto parts = normalized(relative).split('/'); QString current = root.path;
    for (int i = 0; i < parts.size(); ++i) {
        const QDir parent(current);
        // Reject case aliases on case-sensitive hosts as well: exports must
        // remain a single Windows-compatible identity on every platform.
        QDirIterator children(current, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
        int count = 0;
        while (children.hasNext()) {
            children.next(); const auto child = children.fileName();
            if (++count > 200000) return fail(error, QStringLiteral("Directory entry safety limit exceeded."));
            if (child != parts.at(i) && child.compare(parts.at(i), Qt::CaseInsensitive) == 0)
                return fail(error, QStringLiteral("Case-colliding output path: %1").arg(relative));
        }
        current = parent.filePath(parts.at(i)); QFileInfo info(current);
        if (info.isSymLink() || (info.exists() && !samePath(info.canonicalFilePath(), current)))
            return fail(error, QStringLiteral("Linked resource path refused: %1").arg(current));
        if (i + 1 < parts.size()) {
            if (!info.exists() && createParents) {
                if (!QDir().mkdir(current)) return fail(error, QStringLiteral("Cannot create resource subdirectory: %1").arg(current));
                info.refresh();
            }
            if (info.exists() && !info.isDir()) return fail(error, QStringLiteral("Resource parent is not a directory."));
        } else if (info.exists() && !info.isFile()) return fail(error, QStringLiteral("Resource destination is not a regular file."));
    }
    *path = current; return true;
}
struct Stamp { bool exists = false; qint64 size = 0; QDateTime modified, born; QByteArray hash; };
Stamp stamp(const QFileInfo &info) { return {info.exists(), info.size(), info.lastModified(), info.birthTime(), {}}; }
bool sameStamp(const Stamp &left, const Stamp &right) {
    return left.exists == right.exists && (!left.exists || (left.size == right.size
        && left.modified == right.modified && left.born == right.born));
}
bool readFile(const QString &path, qint64 limit, QByteArray *bytes, Stamp *snapshot,
              const Service::Cancel &cancel, bool *wasCancelled, QString *error) {
    const QFileInfo info(path);
    if (!info.isFile() || info.isSymLink() || !samePath(info.canonicalFilePath(), path))
        return fail(error, QStringLiteral("Resource source must be an ordinary unlinked file: %1").arg(path));
    const Stamp before = stamp(info);
    if (before.size > limit) return fail(error, QStringLiteral("File exceeds the %1-byte safety limit: %2").arg(limit).arg(path));
    QFile file(path); if (!file.open(QIODevice::ReadOnly)) return fail(error, file.errorString());
    bytes->clear(); QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        if (cancelled(cancel, wasCancelled, error)) return false;
        const auto block = file.read(qMin<qint64>(65536, limit + 1 - bytes->size()));
        if (file.error() != QFile::NoError) return fail(error, file.errorString());
        bytes->append(block); hash.addData(block);
        if (bytes->size() > limit) return fail(error, QStringLiteral("File grew beyond the safety limit."));
        if (block.isEmpty() && !file.atEnd()) return fail(error, QStringLiteral("Resource read made no progress."));
    }
    if (cancelled(cancel, wasCancelled, error)) return false;
    const QFileInfo after(path);
    if (!sameStamp(before, stamp(after)) || after.isSymLink() || !samePath(after.canonicalFilePath(), path)
        || bytes->size() != before.size)
        return fail(error, QStringLiteral("Resource file changed while being read: %1").arg(path));
    *snapshot = before; snapshot->hash = hash.result(); return true;
}
bool verify(const Root &root, const QString &relative, const Stamp &expected,
            const Service::Cancel &cancel, bool *wasCancelled, QString *error) {
    QString path; if (!contained(root, relative, false, &path, error)) return false;
    if (!sameStamp(expected, stamp(QFileInfo(path)))) return fail(error, QStringLiteral("Destination changed after export preparation: %1").arg(path));
    if (!expected.exists) return true;
    QByteArray bytes; Stamp actual;
    if (!readFile(path, Service::MaximumTextBytes, &bytes, &actual, cancel, wasCancelled, error)) return false;
    return actual.hash == expected.hash || fail(error, QStringLiteral("Destination content changed after export preparation: %1").arg(path));
}
bool writeAtomic(const Root &root, const QString &relative, const Stamp &expected, const QByteArray &bytes,
                 const Service::Cancel &cancel, bool *wasCancelled, QString *error) {
    QString path;
    if (!contained(root, relative, true, &path, error) || !verify(root, relative, expected, cancel, wasCancelled, error)) return false;
    QSaveFile output(path); output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly)) return fail(error, output.errorString());
    for (int position = 0; position < bytes.size();) {
        if (cancelled(cancel, wasCancelled, error)) return false;
        const auto count = qMin(65536, bytes.size() - position);
        if (output.write(bytes.constData() + position, count) != count) return fail(error, output.errorString());
        position += count;
    }
    if (cancelled(cancel, wasCancelled, error) || !verify(root, relative, expected, cancel, wasCancelled, error)) return false;
    // No application callback between the last path/stamp check and commit.
    if (!rootCurrent(root, error) || !contained(root, relative, false, &path, error)) return false;
    return output.commit() || fail(error, output.errorString());
}
struct ResxValue { QString value, comment; };
bool parseResx(const QByteArray &bytes, QMap<QString, ResxValue> *values, QString *error,
               const Service::Cancel &cancel, bool *wasCancelled) {
    QXmlStreamReader xml(bytes); bool root = false; int depth = 0;
    while (!xml.atEnd()) {
        if (cancelled(cancel, wasCancelled, error)) return false;
        const auto token = xml.readNext();
        if (token == QXmlStreamReader::DTD) return fail(error, QStringLiteral("DTD declarations are prohibited in RESX files."));
        if (token == QXmlStreamReader::StartElement) {
            ++depth;
            if (!root) {
                if (xml.name() != QLatin1String("root")) return fail(error, QStringLiteral("The file is not a RESX resource document."));
                root = true; continue;
            }
            if (depth != 2 || xml.name() != QLatin1String("data")) continue;
            const auto attributes = xml.attributes();
            const QString name = attributes.value(QStringLiteral("name")).toString();
            const QString type = attributes.value(QStringLiteral("type")).toString();
            const QString mime = attributes.value(QStringLiteral("mimetype")).toString();
            QString value, comment; bool hasValue = false, hasComment = false;
            while (xml.readNextStartElement()) {
                if (xml.name() == QLatin1String("value") && !hasValue) {
                    value = xml.readElementText(QXmlStreamReader::IncludeChildElements); hasValue = true;
                } else if (xml.name() == QLatin1String("comment") && !hasComment) {
                    comment = xml.readElementText(QXmlStreamReader::IncludeChildElements);
                    if (comment.isNull()) comment = QStringLiteral("");
                    hasComment = true;
                } else xml.skipCurrentElement();
            }
            --depth;
            if (!name.isEmpty() && hasValue && (type.isEmpty() || type.startsWith("System.String")) && mime.isEmpty())
                values->insert(name, {value, comment});
            if (values->size() > Service::MaximumEntries) return fail(error, QStringLiteral("RESX entry safety limit exceeded."));
        } else if (token == QXmlStreamReader::EndElement) --depth;
    }
    if (xml.hasError()) return fail(error, xml.errorString());
    return root || fail(error, QStringLiteral("The file has no RESX root element."));
}
bool validXmlString(const QString &value) {
    for (int i = 0; i < value.size(); ++i) {
        const ushort c = value.at(i).unicode();
        if (c == 9 || c == 10 || c == 13) continue;
        if (c < 32 || c == 0xfffe || c == 0xffff) return false;
        if (QChar::isHighSurrogate(c)) {
            if (++i >= value.size() || !value.at(i).isLowSurrogate()) return false;
        } else if (QChar::isLowSurrogate(c)) return false;
    }
    return true;
}
bool checkEntries(const QVector<ResxTranslationEntry> &entries, QString *error,
                  const Service::Cancel &cancel, bool *wasCancelled) {
    if (entries.size() > Service::MaximumEntries) return fail(error, QStringLiteral("Translation entry safety limit exceeded."));
    qint64 bytes = 0; QSet<TranslationIdentity> identities;
    for (const auto &entry : entries) {
        if (cancelled(cancel, wasCancelled, error)) return false;
        if (!safeRelative(entry.relativePath, error) || !entry.relativePath.endsWith(".resx", Qt::CaseInsensitive)
            || entry.key.isEmpty()) return fail(error, QStringLiteral("Every entry needs a safe .resx path and a nonempty key."));
        const TranslationIdentity identity{normalized(entry.relativePath), entry.key};
        if (identities.contains(identity)) return fail(error, QStringLiteral("Duplicate translation identity: %1 / %2").arg(entry.relativePath, entry.key));
        identities.insert(identity);
        for (const auto &text : {entry.relativePath, entry.key, entry.sourceText, entry.translation, entry.comment}) {
            bytes += qint64(text.size()) * 2;
            if (bytes > Service::MaximumAggregateBytes || text.size() > Service::MaximumResourceBytes)
                return fail(error, QStringLiteral("Translation text exceeds the per-resource or aggregate safety limit."));
            if (!validXmlString(text)) return fail(error, QStringLiteral("An entry contains characters forbidden by XML 1.0."));
        }
        if (bytes > Service::MaximumAggregateBytes) return fail(error, QStringLiteral("Translation text exceeds the aggregate safety limit."));
    }
    return true;
}
QByteArray createResx(const QVector<ResxTranslationEntry> &entries, int begin, int end,
                     const Service::Cancel &cancel, bool *wasCancelled, QString *error) {
    QByteArray bytes; QXmlStreamWriter xml(&bytes); xml.setAutoFormatting(true); xml.setAutoFormattingIndent(2); xml.writeStartDocument(); xml.writeStartElement("root");
    const auto header = [&](const QString &name, const QString &value) {
        xml.writeStartElement("resheader"); xml.writeAttribute("name", name); xml.writeTextElement("value", value); xml.writeEndElement();
    };
    header("resmimetype", "text/microsoft-resx"); header("version", "2.0");
    header("reader", "System.Resources.ResXResourceReader, System.Windows.Forms, Version=4.0.0.0, Culture=neutral, PublicKeyToken=b77a5c561934e089");
    header("writer", "System.Resources.ResXResourceWriter, System.Windows.Forms, Version=4.0.0.0, Culture=neutral, PublicKeyToken=b77a5c561934e089");
    for (int index = begin; index < end; ++index) {
        if (cancelled(cancel, wasCancelled, error)) return {};
        const auto &entry = entries[index]; if (entry.sourceText == entry.translation) continue;
        xml.writeStartElement("data"); xml.writeAttribute("name", entry.key); xml.writeAttribute("xml:space", "preserve");
        QString value = entry.translation, comment = entry.comment;
        value.replace("\r\n", "\n"); value.replace('\r', '\n');
        comment.replace("\r\n", "\n"); comment.replace('\r', '\n');
        xml.writeTextElement("value", value);
        if (!comment.trimmed().isEmpty()) xml.writeTextElement("comment", comment);
        xml.writeEndElement();
        if (bytes.size() > Service::MaximumResourceBytes) {
            fail(error, QStringLiteral("Generated RESX exceeds its file safety limit.")); return {};
        }
    }
    xml.writeEndElement(); xml.writeEndDocument();
    if (bytes.endsWith('\n')) bytes.chop(1);
    const QByteArray declaration("<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
    if (bytes.startsWith(declaration))
        bytes.replace(0, declaration.size(), "<?xml version=\"1.0\" encoding=\"utf-8\"?>");
    return bytes;
}
QString htmlEncode(const QString &value, const Service::Cancel &cancel = {},
                   bool *wasCancelled = nullptr, QString *error = nullptr) {
    QString result;
    for (int i = 0; i < value.size(); ++i) {
        if ((i & 65535) == 0 && wasCancelled && cancelled(cancel, wasCancelled, error)) return {};
        const auto character = value.at(i); uint scalar = character.unicode();
        if (character.isHighSurrogate() && i + 1 < value.size() && value.at(i + 1).isLowSurrogate())
            scalar = QChar::surrogateToUcs4(character, value.at(++i));
        if (scalar == '&') result += "&amp;";
        else if (scalar == '<') result += "&lt;";
        else if (scalar == '>') result += "&gt;";
        else if (scalar == '"') result += "&quot;";
        else if (scalar == '\'' || (scalar >= 160 && scalar <= 255) || scalar > 0xffff)
            result += "&#" + QString::number(scalar) + ';';
        else result += character;
    }
    return result;
}
QString htmlDecode(const QString &input, const Service::Cancel &cancel, bool *wasCancelled, QString *error) {
    // WebUtility uses the HTML4 named-entity set, not the HTML5 DOM parser
    // rules (which would also strip literal markup from legacy cells).
    static const QHash<QString, uint> entities = [] {
        const QByteArray table(
            "AElig=198 Aacute=193 Acirc=194 Agrave=192 Alpha=913 Aring=197 Atilde=195 Auml=196 Beta=914 Ccedil=199 Chi=935 Dagger=8225 Delta=916 ETH=208 Eacute=201 Ecirc=202 Egrave=200 Epsilon=917 Eta=919 Euml=203 Gamma=915 Iacute=205 Icirc=206 Igrave=204 Iota=921 Iuml=207 Kappa=922 Lambda=923 Mu=924 Ntilde=209 Nu=925 OElig=338 Oacute=211 Ocirc=212 Ograve=210 Omega=937 Omicron=927 Oslash=216 Otilde=213 Ouml=214 Phi=934 Pi=928 Prime=8243 Psi=936 Rho=929 Scaron=352 Sigma=931 THORN=222 Tau=932 Theta=920 Uacute=218 Ucirc=219 Ugrave=217 Upsilon=933 Uuml=220 Xi=926 Yacute=221 Yuml=376 Zeta=918 "
            "aacute=225 acirc=226 acute=180 aelig=230 agrave=224 alefsym=8501 alpha=945 amp=38 and=8743 ang=8736 apos=39 aring=229 asymp=8776 atilde=227 auml=228 bdquo=8222 beta=946 brvbar=166 bull=8226 cap=8745 ccedil=231 cedil=184 cent=162 chi=967 circ=710 clubs=9827 cong=8773 copy=169 crarr=8629 cup=8746 curren=164 dArr=8659 dagger=8224 darr=8595 deg=176 delta=948 diams=9830 divide=247 eacute=233 ecirc=234 egrave=232 empty=8709 emsp=8195 ensp=8194 epsilon=949 equiv=8801 eta=951 eth=240 euml=235 euro=8364 exist=8707 fnof=402 forall=8704 frac12=189 frac14=188 frac34=190 frasl=8260 gamma=947 ge=8805 gt=62 hArr=8660 harr=8596 hearts=9829 hellip=8230 iacute=237 icirc=238 iexcl=161 igrave=236 image=8465 infin=8734 int=8747 iota=953 iquest=191 isin=8712 iuml=239 kappa=954 lArr=8656 lambda=955 lang=9001 laquo=171 larr=8592 lceil=8968 ldquo=8220 le=8804 lfloor=8970 lowast=8727 loz=9674 lrm=8206 lsaquo=8249 lsquo=8216 lt=60 macr=175 mdash=8212 micro=181 middot=183 minus=8722 mu=956 nabla=8711 nbsp=160 ndash=8211 ne=8800 ni=8715 not=172 notin=8713 nsub=8836 ntilde=241 nu=957 oacute=243 ocirc=244 oelig=339 ograve=242 oline=8254 omega=969 omicron=959 oplus=8853 or=8744 ordf=170 ordm=186 oslash=248 otilde=245 otimes=8855 ouml=246 para=182 part=8706 permil=8240 perp=8869 phi=966 pi=960 piv=982 plusmn=177 pound=163 prime=8242 prod=8719 prop=8733 psi=968 quot=34 rArr=8658 radic=8730 rang=9002 raquo=187 rarr=8594 rceil=8969 rdquo=8221 real=8476 reg=174 rfloor=8971 rho=961 rlm=8207 rsaquo=8250 rsquo=8217 sbquo=8218 scaron=353 sdot=8901 sect=167 shy=173 sigma=963 sigmaf=962 sim=8764 spades=9824 sub=8834 sube=8838 sum=8721 sup=8835 sup1=185 sup2=178 sup3=179 supe=8839 szlig=223 tau=964 there4=8756 theta=952 thetasym=977 thinsp=8201 thorn=254 tilde=732 times=215 trade=8482 uArr=8657 uacute=250 uarr=8593 ucirc=251 ugrave=249 uml=168 upsih=978 upsilon=965 uuml=252 weierp=8472 xi=958 yacute=253 yen=165 yuml=255 zeta=950 zwj=8205 zwnj=8204");
        QHash<QString, uint> result;
        for (const auto &pair : table.split(' ')) {
            const int split = pair.indexOf('=');
            result.insert(QString::fromLatin1(pair.left(split)), pair.mid(split + 1).toUInt());
        }
        return result;
    }();
    QString output; output.reserve(input.size());
    for (int index = 0; index < input.size(); ++index) {
        if ((index & 65535) == 0 && cancelled(cancel, wasCancelled, error)) return {};
        if (input.at(index) != '&') { output += input.at(index); continue; }
        // The longest supported numeric or named entity fits this bounded
        // look-ahead; malformed ampersands cannot cause quadratic scans.
        int end = index + 1;
        while (end < input.size() && end - index <= 32 && input.at(end) != ';' && input.at(end) != '&') ++end;
        if (end >= input.size() || input.at(end) != ';' || end - index > 32) { output += '&'; continue; }
        const QString name = input.mid(index + 1, end - index - 1);
        bool valid = false; uint scalar = 0;
        if (name.startsWith('#')) {
            const bool hexadecimal = name.size() > 1 && (name.at(1) == 'x' || name.at(1) == 'X');
            const auto digits = name.mid(hexadecimal ? 2 : 1);
            scalar = digits.toUInt(&valid, hexadecimal ? 16 : 10);
            valid = valid && !digits.isEmpty() && scalar <= 0x10ffff && !(scalar >= 0xd800 && scalar <= 0xdfff);
        } else {
            const auto found = entities.constFind(name); valid = found != entities.cend();
            if (valid) scalar = *found;
        }
        if (!valid) { output += '&'; continue; }
        output += QString::fromUcs4(&scalar, 1); index = end;
    }
    return output;
}
int openingTag(const QString &html, const QString &name, int from, int *after) {
    const QString start = '<' + name;
    int position = from;
    while ((position = html.indexOf(start, position, Qt::CaseInsensitive)) >= 0) {
        const int boundary = position + start.size();
        if (boundary < html.size() && !html.at(boundary).isLetterOrNumber() && html.at(boundary) != '_') {
            const int close = html.indexOf('>', boundary);
            if (close < 0) return -1;
            *after = close + 1; return position;
        }
        position = boundary;
    }
    return -1;
}
}

bool operator==(const TranslationIdentity &a, const TranslationIdentity &b) noexcept {
    return pathKey(a.relativePath) == pathKey(b.relativePath) && a.key == b.key;
}
uint qHash(const TranslationIdentity &value, uint seed) noexcept {
    return qHash(value.key, qHash(pathKey(value.relativePath), seed));
}
QVector<TranslationCulture> ResxTranslationService::cultures(QString *error) {
    if (error) *error = catalog().error; return catalog().values;
}
QString ResxTranslationService::localizedRelativePath(const QString &path, const QString &culture, QString *error) {
    if (error) error->clear();
    const QString relative = normalized(path);
    if (!safeRelative(relative, error) || !relative.endsWith(".resx", Qt::CaseInsensitive)) {
        if (error && error->isEmpty()) *error = QStringLiteral("Resource path must end in .resx."); return {};
    }
    const auto name = cultureName(culture, error); if (name.isEmpty()) return {};
    return relative.left(relative.size() - 5) + '.' + name + QStringLiteral(".resx");
}
bool ResxTranslationService::isTranslatable(const QString &fileName, const QString &key) {
    return fileName.compare("Strings.resx", Qt::CaseInsensitive) == 0
        || key.endsWith(".ToolTip") || key.endsWith(".Text") || key.endsWith("HeaderText") || key.endsWith("ToolTipText");
}
bool ResxTranslationService::resumeFileMatches(const QString &importedFile, const QString &sourceRelativePath) {
    const auto imported = normalized(importedFile), source = normalized(sourceRelativePath);
    if (imported.compare(source, Qt::CaseInsensitive) == 0
        || imported.section('/', -1).compare(source.section('/', -1), Qt::CaseInsensitive) == 0) return true;
    if (!source.endsWith(".resx", Qt::CaseInsensitive)) return false;
    QString name = source.left(source.size() - 5); name.replace('/', '.'); name += ".resources";
    return imported.compare(name, Qt::CaseInsensitive) == 0 || imported.endsWith('.' + name, Qt::CaseInsensitive);
}

ResxTranslationService::LoadResult ResxTranslationService::load(const QString &sourceRoot,
    const QString &culture, Cancel cancel, Progress progress)
{
    LoadResult result;
    if (cancelled(cancel, &result.cancelled, &result.error)) return result;
    Root root; if (!admitRoot(sourceRoot, false, &root, &result.error)) return result;
    result.project.sourceRoot = root.path;
    result.project.culture = cultureName(culture, &result.error);
    if (result.project.culture.isEmpty()) return result;
    const QSet<QString> ignored{".git", ".backup", "bin", "obj", "translation"};
    QVector<QPair<QString, int>> pending{{root.path, 0}};
    QStringList files; int directories = 0, discoveredEntries = 0;
    while (!pending.isEmpty()) {
        if (cancelled(cancel, &result.cancelled, &result.error)) return result;
        const auto directory = pending.takeLast();
        if (++directories > MaximumDirectories || directory.second > MaximumDepth) {
            result.error = QStringLiteral("RESX discovery exceeded its directory/depth safety limit."); return result;
        }
        if (!rootCurrent(root, &result.error)) return result;
        const QFileInfo dirInfo(directory.first);
        if (!dirInfo.isDir() || dirInfo.isSymLink() || !samePath(dirInfo.canonicalFilePath(), directory.first)) {
            result.error = QStringLiteral("A source directory changed or became linked during discovery."); return result;
        }
        QDirIterator iterator(directory.first, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
        while (iterator.hasNext()) {
            if (cancelled(cancel, &result.cancelled, &result.error)) return result;
            iterator.next(); const auto info = iterator.fileInfo();
            if (++discoveredEntries > 200000) { result.error = QStringLiteral("Directory entry safety limit exceeded."); return result; }
            if (info.isDir()) {
                if (!info.isSymLink() && !ignored.contains(info.fileName().toCaseFolded())) {
                    if (pending.size() + directories >= MaximumDirectories) {
                        result.error = QStringLiteral("RESX directory safety limit exceeded."); return result;
                    }
                    pending.append({info.absoluteFilePath(), directory.second + 1});
                }
            } else if (info.suffix().compare("resx", Qt::CaseInsensitive) == 0) {
                if (!info.isFile() || info.isSymLink()) {
                    warning(&result.project.warnings, QStringLiteral("Linked or non-regular resource ignored: %1").arg(info.fileName())); continue;
                }
                if (files.size() >= MaximumFiles) { result.error = QStringLiteral("RESX file safety limit exceeded."); return result; }
                files.append(QDir(root.path).relativeFilePath(info.absoluteFilePath()));
            }
        }
    }
    std::sort(files.begin(), files.end());
    QHash<QString, QString> discovered;
    QStringList neutral;
    for (const auto &relative : files) {
        const QString key = pathKey(relative);
        if (discovered.contains(key)) {
            warning(&result.project.warnings, QStringLiteral("Case-colliding resource path ignored: %1").arg(relative)); continue;
        }
        discovered.insert(key, relative);
        const QString stem = QFileInfo(relative).completeBaseName();
        const auto suffix = stem.section('.', -1);
        // Culture names themselves may contain hyphens, never dots.
        const bool localized = stem.contains('.') && catalog().names.contains(suffix.toCaseFolded());
        if (!localized) neutral.append(relative);
    }
    std::sort(neutral.begin(), neutral.end(), [](const QString &a, const QString &b) {
        const auto compare = pathOrderKey(a).compare(pathOrderKey(b)); return compare == 0 ? a < b : compare < 0;
    });
    qint64 bytesRead = 0, textBytes = 0;
    for (int index = 0; index < neutral.size(); ++index) {
        if (progress) progress(index, neutral.size());
        if (cancelled(cancel, &result.cancelled, &result.error) || !rootCurrent(root, &result.error)) return result;
        const QString relative = neutral.at(index); QString error;
        const QString sourcePath = QDir(root.path).filePath(relative);
        QByteArray bytes; Stamp snapshot; QMap<QString, ResxValue> source, translated;
        const bool sourceRead = readFile(sourcePath, qMin(MaximumResourceBytes, MaximumAggregateBytes - bytesRead),
                                        &bytes, &snapshot, cancel, &result.cancelled, &error);
        bytesRead += bytes.size();
        if (bytesRead >= MaximumAggregateBytes) { result.error = QStringLiteral("RESX input exceeds the aggregate safety limit."); return result; }
        if (!sourceRead || !parseResx(bytes, &source, &error, cancel, &result.cancelled)) {
            if (result.cancelled) { result.error = error; return result; }
            warning(&result.project.warnings, relative + ": " + error); continue;
        }
        const QString desired = relative.left(relative.size() - 5) + '.' + result.project.culture + ".resx";
        const QString target = discovered.value(pathKey(desired), desired);
        const QFileInfo targetInfo(QDir(root.path).filePath(target));
        if (targetInfo.exists() || targetInfo.isSymLink()) {
            bytes.clear();
            const bool targetRead = readFile(targetInfo.absoluteFilePath(), qMin(MaximumResourceBytes, MaximumAggregateBytes - bytesRead),
                                            &bytes, &snapshot, cancel, &result.cancelled, &error);
            bytesRead += bytes.size();
            if (bytesRead >= MaximumAggregateBytes) { result.error = QStringLiteral("RESX input exceeds the aggregate safety limit."); return result; }
            if (!targetRead || !parseResx(bytes, &translated, &error, cancel, &result.cancelled)) {
                if (result.cancelled) { result.error = error; return result; }
                translated.clear(); warning(&result.project.warnings, target + ": " + error);
            }
        }
        if (bytesRead > MaximumAggregateBytes) { result.error = QStringLiteral("RESX input exceeds the aggregate safety limit."); return result; }
        const int before = result.project.entries.size();
        for (auto value = source.cbegin(); value != source.cend(); ++value) {
            if (cancelled(cancel, &result.cancelled, &result.error)) return result;
            if (!isTranslatable(QFileInfo(relative).fileName(), value.key())) continue;
            const auto targetValue = translated.constFind(value.key());
            const bool existing = targetValue != translated.cend();
            ResxTranslationEntry entry{relative, value.key(), value->value,
                existing ? targetValue->value : value->value,
                existing && !targetValue->comment.isNull() ? targetValue->comment : value->comment, existing};
            textBytes += 2 * (qint64(entry.relativePath.size()) + entry.key.size() + entry.sourceText.size()
                             + entry.translation.size() + entry.comment.size());
            if (textBytes > MaximumAggregateBytes || result.project.entries.size() >= MaximumEntries) {
                result.error = QStringLiteral("Loaded translation entries exceed the count/text safety limit."); return result;
            }
            result.project.entries.append(entry);
        }
        if (result.project.entries.size() != before) ++result.project.resourceFiles;
    }
    if (progress) progress(neutral.size(), neutral.size());
    if (cancelled(cancel, &result.cancelled, &result.error) || !rootCurrent(root, &result.error)) return result;
    if (!result.project.resourceFiles) {
        result.error = QStringLiteral("No neutral .resx files with Mission Planner translatable string keys were found."); return result;
    }
    result.success = true; return result;
}

ResxTranslationService::ExportResult ResxTranslationService::exportTranslations(const QString &outputRoot,
    const QString &culture, const QVector<ResxTranslationEntry> &requested, Cancel cancel, Progress progress)
{
    ExportResult result; auto entries = requested; // Immutable across trusted callback delivery.
    if (cancelled(cancel, &result.cancelled, &result.error)) return result;
    const QString language = cultureName(culture, &result.error);
    if (language.isEmpty() || !checkEntries(entries, &result.error, cancel, &result.cancelled)) return result;
    std::stable_sort(entries.begin(), entries.end(), [](const ResxTranslationEntry &a, const ResxTranslationEntry &b) {
        const int compare = pathOrderKey(a.relativePath).compare(pathOrderKey(b.relativePath));
        return compare == 0 ? a.key < b.key : compare < 0;
    });
    struct Output { QString relative; QByteArray bytes; Stamp old; int translations = 0; bool resource = false; };
    QVector<Output> outputs; qint64 generatedBytes = 0;
    for (int begin = 0; begin < entries.size();) {
        if (cancelled(cancel, &result.cancelled, &result.error)) return result;
        int end = begin + 1;
        while (end < entries.size() && pathKey(entries[end].relativePath) == pathKey(entries[begin].relativePath)) ++end;
        Output output; output.relative = localizedRelativePath(entries[begin].relativePath, language, &result.error);
        if (output.relative.isEmpty()) return result;
        output.bytes = createResx(entries, begin, end, cancel, &result.cancelled, &result.error);
        if (output.bytes.isEmpty()) return result;
        output.resource = true;
        for (int index = begin; index < end; ++index) output.translations += entries[index].sourceText != entries[index].translation;
        generatedBytes += output.bytes.size();
        if (output.bytes.size() > MaximumResourceBytes || generatedBytes > MaximumAggregateBytes || outputs.size() >= MaximumFiles) {
            result.error = QStringLiteral("Generated RESX output exceeds the file/count/aggregate safety limit."); return result;
        }
        outputs.append(std::move(output)); begin = end;
    }
    QString html = QStringLiteral("<!doctype html>\n<html><head><meta charset=\"utf-8\"><title>Mission Planner ")
        + htmlEncode(language) + QStringLiteral(" translation</title></head><body><table>\n<tr><th>File</th><th>Key</th><th>Translation</th></tr>\n");
    for (const auto &entry : entries) {
        if (cancelled(cancel, &result.cancelled, &result.error)) return result;
        html += "<tr><td>" + htmlEncode(normalized(entry.relativePath), cancel, &result.cancelled, &result.error)
            + "</td><td>" + htmlEncode(entry.key, cancel, &result.cancelled, &result.error)
            + "</td><td>" + htmlEncode(entry.translation, cancel, &result.cancelled, &result.error) + "</td></tr>\n";
        if (result.cancelled) return result;
        if (qint64(html.size()) * 2 > MaximumTextBytes) { result.error = QStringLiteral("Resume HTML exceeds the text safety limit."); return result; }
    }
    html += "</table></body></html>\n";
    Output resume; resume.relative = QStringLiteral("output.html"); resume.bytes = html.toUtf8();
    generatedBytes += resume.bytes.size();
    if (resume.bytes.size() > MaximumTextBytes || generatedBytes > MaximumAggregateBytes) {
        result.error = QStringLiteral("Generated output exceeds the aggregate safety limit."); return result;
    }
    outputs.append(std::move(resume));
    Root root; if (!admitRoot(outputRoot, true, &root, &result.error)) return result;
    result.outputRoot = root.path;
    qint64 existingBytes = 0;
    for (auto &output : outputs) {
        if (cancelled(cancel, &result.cancelled, &result.error)) return result;
        QString path;
        if (!contained(root, output.relative, false, &path, &result.error)) return result;
        const QFileInfo info(path); output.old = stamp(info);
        if (!output.old.exists) continue;
        QByteArray bytes;
        if (!readFile(path, MaximumTextBytes, &bytes, &output.old, cancel, &result.cancelled, &result.error)) return result;
        existingBytes += bytes.size();
        if (existingBytes > MaximumAggregateBytes) { result.error = QStringLiteral("Existing outputs exceed the backup safety limit."); return result; }
    }
    const qint64 total = outputs.size() * 2; qint64 completed = 0;
    if (progress) progress(0, total);
    QString backupRelative;
    // ALL original destinations, including output.html, are backed up before
    // the first destination is replaced. A failure leaves these receipts.
    for (const auto &output : outputs) {
        if (cancelled(cancel, &result.cancelled, &result.error)) return result;
        if (!verify(root, output.relative, output.old, cancel, &result.cancelled, &result.error)) return result;
        if (output.old.exists) {
            if (backupRelative.isEmpty()) {
                backupRelative = ".backup/" + QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss")
                    + '-' + QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-').left(8);
                result.backupDirectory = QDir(root.path).filePath(backupRelative);
            }
            const QString original = QDir(root.path).filePath(output.relative);
            QByteArray bytes; Stamp current;
            if (!readFile(original, MaximumTextBytes, &bytes, &current, cancel, &result.cancelled, &result.error)) return result;
            if (!sameStamp(output.old, current) || current.hash != output.old.hash) {
                result.error = QStringLiteral("An original output changed before backup."); return result;
            }
            const QString backup = backupRelative + '/' + output.relative;
            if (!writeAtomic(root, backup, {}, bytes, cancel, &result.cancelled, &result.error)) return result;
            result.backupPaths.append(QDir(root.path).filePath(backup));
        }
        if (progress) progress(++completed, total);
    }
    for (const auto &output : outputs) {
        if (cancelled(cancel, &result.cancelled, &result.error)) return result;
        if (!writeAtomic(root, output.relative, output.old, output.bytes, cancel, &result.cancelled, &result.error)) return result;
        const QString path = QDir(root.path).filePath(output.relative);
        result.publishedPaths.append(path);
        if (output.old.exists) ++result.overwrittenFiles;
        if (output.resource) { ++result.resourceFiles; result.translatedEntries += output.translations; }
        else result.resumeHtmlPath = path;
        if (progress) progress(++completed, total);
    }
    // All files have already been committed: a final progress observer cannot
    // retroactively turn the truthful completed receipt into cancellation.
    result.success = true; return result;
}

ResxTranslationService::TextResult ResxTranslationService::buildCsv(
    const QVector<ResxTranslationEntry> &requested, Cancel cancel, Progress progress)
{
    const auto entries = requested; TextResult result;
    if (entries.size() > MaximumEntries) { result.error = QStringLiteral("CSV entry safety limit exceeded."); return result; }
    result.text = QStringLiteral("File,Key,English,Translation\r\n");
    for (int i = 0; i < entries.size(); ++i) {
        if (progress) progress(i, entries.size());
        if (cancelled(cancel, &result.cancelled, &result.error)) { result.text.clear(); return result; }
        const auto &entry = entries[i]; QStringList cells;
        for (QString cell : {entry.relativePath, entry.key, entry.sourceText, entry.translation}) {
            cell.replace('"', "\"\""); cells.append('"' + cell + '"');
        }
        result.text += cells.join(',') + "\r\n";
        if (qint64(result.text.size()) * 2 > MaximumTextBytes) {
            result.error = QStringLiteral("CSV exceeds the text safety limit."); result.text.clear(); return result;
        }
    }
    if (progress) progress(entries.size(), entries.size());
    if (cancelled(cancel, &result.cancelled, &result.error)) { result.text.clear(); return result; }
    result.success = true; return result;
}

ResxTranslationService::ImportResult ResxTranslationService::importResumeHtml(
    const QString &requested, Cancel cancel, Progress progress)
{
    ImportResult result;
    if (cancelled(cancel, &result.cancelled, &result.error)) return result;
    const QFileInfo chosen(requested);
    if (chosen.isSymLink() || chosen.canonicalFilePath().isEmpty()) {
        result.error = QStringLiteral("Translation resume HTML must be an existing ordinary file."); return result;
    }
    QByteArray bytes; Stamp snapshot;
    if (!readFile(chosen.canonicalFilePath(), MaximumResumeBytes, &bytes, &snapshot,
                  cancel, &result.cancelled, &result.error)) return result;
    QBuffer buffer(&bytes); buffer.open(QIODevice::ReadOnly);
    QTextStream text(&buffer); text.setCodec("UTF-8"); text.setAutoDetectUnicode(true);
    const QString html = text.readAll(); int position = 0; qint64 textBytes = 0;
    while (position < html.size()) {
        if (progress) progress(position, html.size());
        if (cancelled(cancel, &result.cancelled, &result.error)) return result;
        int after = 0;
        if (openingTag(html, QStringLiteral("tr"), position, &after) < 0) break;
        position = after; QString cells[3]; bool match = true;
        for (int cell = 0; cell < 3; ++cell) {
            while (position < html.size() && html.at(position).isSpace()) ++position;
            int valueBegin = 0;
            const int boundary = position + 3;
            if (html.midRef(position, 3).compare(QStringLiteral("<td"), Qt::CaseInsensitive) != 0
                || boundary >= html.size() || html.at(boundary).isLetterOrNumber() || html.at(boundary) == '_') {
                match = false; break;
            }
            const int openingEnd = html.indexOf('>', boundary);
            if (openingEnd < 0) { position = html.size(); match = false; break; }
            valueBegin = openingEnd + 1;
            const int end = html.indexOf(QStringLiteral("</td>"), valueBegin, Qt::CaseInsensitive);
            if (end < 0) { position = html.size(); match = false; break; }
            cells[cell] = html.mid(valueBegin, end - valueBegin);
            position = end + 5;
        }
        if (!match) continue;
        while (position < html.size() && html.at(position).isSpace()) ++position;
        if (html.midRef(position, 5).compare(QStringLiteral("</tr>"), Qt::CaseInsensitive) != 0) continue;
        position += 5;
        for (auto &cell : cells) {
            cell = htmlDecode(cell, cancel, &result.cancelled, &result.error);
            if (result.cancelled) return result;
        }
        if ((cells[0].compare("File", Qt::CaseInsensitive) == 0 && cells[1].compare("Key", Qt::CaseInsensitive) == 0)
            || cells[0].trimmed().isEmpty() || cells[1].trimmed().isEmpty()) continue;
        TranslationIdentity identity{normalized(cells[0]), cells[1]};
        const auto previous = result.values.constFind(identity);
        if (previous == result.values.cend()) {
            if (result.order.size() >= MaximumEntries) { result.error = QStringLiteral("Resume entry safety limit exceeded."); return result; }
            result.order.append(identity);
            textBytes += 2 * (qint64(identity.relativePath.size()) + identity.key.size());
        } else textBytes -= 2 * qint64(previous->size());
        textBytes += 2 * qint64(cells[2].size());
        if (textBytes > MaximumAggregateBytes) { result.error = QStringLiteral("Resume text safety limit exceeded."); return result; }
        result.values.insert(identity, cells[2]);
    }
    if (progress) progress(html.size(), html.size());
    if (cancelled(cancel, &result.cancelled, &result.error)) return result;
    result.success = true; return result;
}
