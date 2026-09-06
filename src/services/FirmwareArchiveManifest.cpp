#include "FirmwareArchiveManifest.h"

#include <QCryptographicHash>
#include <QDomElement>
#include <QXmlStreamReader>

namespace {
constexpr int MaximumXmlDepth = 128;
constexpr int MaximumXmlNodes = 200000;
constexpr int MaximumPathSegmentCharacters = 96;

void assignError(QString *error, const QString &message)
{
    if (error) *error = message;
}

QString canonicalUrl(const QUrl &url)
{
    return QString::fromLatin1(url.toEncoded(QUrl::FullyEncoded));
}

QString localName(const QDomElement &element)
{
    const QString local = element.localName();
    if (!local.isEmpty()) return local;
    const QString qualified = element.tagName();
    const int colon = qualified.indexOf(QLatin1Char(':'));
    return colon < 0 ? qualified : qualified.mid(colon + 1);
}

QVector<QDomElement> descendantElements(const QDomElement &root)
{
    QVector<QDomElement> result;
    QVector<QDomNode> pending;
    // XDocument.Descendants includes its document element, unlike
    // XElement.Descendants. Keep the same traversal for parsing and rewriting.
    pending.append(root);
    while (!pending.isEmpty()) {
        const QDomNode node = pending.takeLast();
        if (node.isElement()) result.append(node.toElement());
        for (QDomNode child = node.lastChild(); !child.isNull();
             child = child.previousSibling()) {
            pending.append(child);
        }
    }
    return result;
}

bool secureXmlPreflight(const QByteArray &xml, QString *error)
{
    if (xml.isEmpty()) {
        assignError(error, QStringLiteral("The firmware manifest is empty."));
        return false;
    }
    if (xml.size() > FirmwareArchive::MaximumManifestBytes) {
        assignError(error, QStringLiteral(
            "The firmware manifest exceeds the %1-byte safety limit.")
            .arg(FirmwareArchive::MaximumManifestBytes));
        return false;
    }

    QXmlStreamReader reader(xml);
    int depth = 0;
    int nodes = 0;
    while (!reader.atEnd()) {
        const QXmlStreamReader::TokenType token = reader.readNext();
        if (token == QXmlStreamReader::DTD
            || token == QXmlStreamReader::EntityReference) {
            assignError(error, QStringLiteral(
                "DTD and entity declarations are prohibited in firmware manifests."));
            return false;
        }
        if (token == QXmlStreamReader::StartElement) {
            ++depth;
            if (depth > MaximumXmlDepth) {
                assignError(error, QStringLiteral(
                    "The firmware manifest exceeds the XML depth limit."));
                return false;
            }
        } else if (token == QXmlStreamReader::EndElement) {
            --depth;
        }
        if (token == QXmlStreamReader::StartElement
            || token == QXmlStreamReader::EndElement
            || token == QXmlStreamReader::Characters
            || token == QXmlStreamReader::Comment
            || token == QXmlStreamReader::ProcessingInstruction) {
            if (++nodes > MaximumXmlNodes) {
                assignError(error, QStringLiteral(
                    "The firmware manifest exceeds the XML node limit."));
                return false;
            }
        }
    }
    if (reader.hasError()) {
        assignError(error, QStringLiteral("Invalid firmware manifest XML at line %1: %2")
            .arg(reader.lineNumber()).arg(reader.errorString()));
        return false;
    }
    return true;
}

bool isReservedDeviceName(const QString &value)
{
    const QString base = value.section(QLatin1Char('.'), 0, 0).toUpper();
    if (base == QStringLiteral("CON") || base == QStringLiteral("PRN")
        || base == QStringLiteral("AUX") || base == QStringLiteral("NUL")
        || base == QStringLiteral("CLOCK$")
        || base == QStringLiteral("CONIN$")
        || base == QStringLiteral("CONOUT$")) {
        return true;
    }
    if (base.size() == 4
        && (base.startsWith(QStringLiteral("COM"))
            || base.startsWith(QStringLiteral("LPT")))) {
        return base.at(3) >= QLatin1Char('1') && base.at(3) <= QLatin1Char('9');
    }
    return false;
}

QString sanitizeSegment(QString value)
{
    value = value.normalized(QString::NormalizationForm_C).trimmed();
    if (value.isEmpty() || value == QStringLiteral(".")
        || value == QStringLiteral("..")) {
        value = QStringLiteral("_");
    }

    QString result;
    result.reserve(qMin(value.size(), MaximumPathSegmentCharacters));
    for (const QChar character : value) {
        if (result.size() == MaximumPathSegmentCharacters) break;
        const bool invalid = character.category() == QChar::Other_Control
            || character == QLatin1Char('<') || character == QLatin1Char('>')
            || character == QLatin1Char(':') || character == QLatin1Char('"')
            || character == QLatin1Char('/') || character == QLatin1Char('\\')
            || character == QLatin1Char('|') || character == QLatin1Char('?')
            || character == QLatin1Char('*');
        result.append(invalid ? QLatin1Char('_') : character);
    }
    if (!result.isEmpty() && result.at(result.size() - 1).isHighSurrogate()) result.chop(1);
    result = result.trimmed();
    while (result.endsWith(QLatin1Char('.'))
           || result.endsWith(QLatin1Char(' '))) {
        result.chop(1);
    }
    if (result.isEmpty() || result == QStringLiteral(".")
        || result == QStringLiteral("..")) {
        result = QStringLiteral("_");
    }
    if (isReservedDeviceName(result)) {
        result.prepend(QLatin1Char('_'));
        if (result.size() > MaximumPathSegmentCharacters) {
            result.truncate(MaximumPathSegmentCharacters);
        }
    }
    return result;
}

bool replaceElementText(QDomDocument *document, QDomElement element,
                        const QString &text)
{
    while (!element.firstChild().isNull()) {
        if (element.removeChild(element.firstChild()).isNull()) return false;
    }
    return !element.appendChild(document->createTextNode(text)).isNull();
}
}

bool FirmwareArchiveManifest::allowedUrl(const QUrl &url, bool httpsOnly)
{
    if (!url.isValid() || url.isRelative() || url.host().trimmed().isEmpty()) {
        return false;
    }
    const QString scheme = url.scheme();
    if (httpsOnly) {
        if (scheme.compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0) {
            return false;
        }
    } else if (scheme.compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0
               && scheme.compare(QStringLiteral("http"), Qt::CaseInsensitive) != 0) {
        return false;
    }
    // Checking the encoded authority also rejects a syntactically present but
    // empty user-info section such as https://@host/firmware.apj.
    if (!url.userInfo(QUrl::FullyEncoded).isEmpty()
        || url.authority(QUrl::FullyEncoded).contains(QLatin1Char('@'))) {
        return false;
    }
    return true;
}

QString FirmwareArchiveManifest::relativePath(const QUrl &url, QString *error)
{
    assignError(error, QString());
    if (!allowedUrl(url, false)) {
        assignError(error, QStringLiteral(
            "Only absolute HTTP(S) firmware URLs without credentials are accepted."));
        return {};
    }

    QString host = QString::fromLatin1(QUrl::toAce(url.host()));
    // IDNA conversion is for DNS names; Qt returns empty for IPv6 literals.
    if (host.isEmpty()) host = url.host();
    host = sanitizeSegment(host);

    QString fileName = QStringLiteral("firmware.bin");
    const QStringList pathSegments = url.path(QUrl::FullyEncoded).split(
        QLatin1Char('/'), Qt::SkipEmptyParts);
    if (!pathSegments.isEmpty()) {
        fileName = sanitizeSegment(QUrl::fromPercentEncoding(
            pathSegments.constLast().toLatin1()));
    }
    const QByteArray digest = QCryptographicHash::hash(
        canonicalUrl(url).toUtf8(), QCryptographicHash::Sha256).toHex().left(12);
    fileName = QString::fromLatin1(digest) + QLatin1Char('-') + fileName;
    return QStringLiteral("files/%1/%2").arg(host, fileName);
}

FirmwareArchiveManifest::Plan FirmwareArchiveManifest::parse(const QByteArray &xml)
{
    Plan result;
    if (!secureXmlPreflight(xml, &result.error)) return result;

    QString parseError;
    int errorLine = 0;
    int errorColumn = 0;
    if (!result.document.setContent(
            xml, true, &parseError, &errorLine, &errorColumn)) {
        result.error = QStringLiteral("Invalid firmware manifest XML at %1:%2: %3")
            .arg(errorLine).arg(errorColumn).arg(parseError);
        result.document.clear();
        return result;
    }
    const QDomElement root = result.document.documentElement();
    if (root.isNull()) {
        result.error = QStringLiteral("The firmware manifest has no root element.");
        result.document.clear();
        return result;
    }

    QHash<QString, int> downloadsByUrl;
    QHash<QString, QString> urlByFoldedPath;
    const QVector<QDomElement> elements = descendantElements(root);
    for (const QDomElement &element : elements) {
        if (!localName(element).startsWith(
                QStringLiteral("url"), Qt::CaseInsensitive)) {
            continue;
        }
        const QString value = element.text().trimmed();
        if (value.isEmpty()) continue;
        const QUrl url(value, QUrl::StrictMode);
        if (!allowedUrl(url, false)) {
            result.error = QStringLiteral(
                "Firmware manifest field %1 is not an absolute HTTP(S) URL without credentials.")
                .arg(localName(element));
            result.document.clear();
            result.downloads.clear();
            return result;
        }
        const QString key = canonicalUrl(url);
        if (downloadsByUrl.contains(key)) continue;
        if (result.downloads.size() == FirmwareArchive::MaximumDownloads) {
            result.error = QStringLiteral(
                "The firmware manifest exceeds the %1-download safety limit.")
                .arg(FirmwareArchive::MaximumDownloads);
            result.document.clear();
            result.downloads.clear();
            return result;
        }
        QString pathError;
        const QString path = relativePath(url, &pathError);
        if (path.isEmpty()) {
            result.error = pathError;
            result.document.clear();
            result.downloads.clear();
            return result;
        }
        const QString foldedPath = path.toCaseFolded();
        const auto collision = urlByFoldedPath.constFind(foldedPath);
        if (collision != urlByFoldedPath.constEnd() && collision.value() != key) {
            result.error = QStringLiteral(
                "Two firmware URLs produce the same case-insensitive archive path.");
            result.document.clear();
            result.downloads.clear();
            return result;
        }
        downloadsByUrl.insert(key, result.downloads.size());
        urlByFoldedPath.insert(foldedPath, key);
        result.downloads.append({url, path});
    }
    if (result.downloads.isEmpty()) {
        result.error = QStringLiteral(
            "The firmware manifest contains no absolute HTTP(S) firmware URLs.");
        result.document.clear();
        return result;
    }
    result.success = true;
    result.error.clear();
    return result;
}

QByteArray FirmwareArchiveManifest::rewrite(
    const Plan &plan, const QHash<QString, QString> &successfulPaths,
    QString *error)
{
    assignError(error, QString());
    if (!plan.success || plan.document.documentElement().isNull()) {
        assignError(error, QStringLiteral("The firmware manifest plan is invalid."));
        return {};
    }
    if (plan.downloads.isEmpty()
        || plan.downloads.size() > FirmwareArchive::MaximumDownloads
        || successfulPaths.size() > FirmwareArchive::MaximumDownloads) {
        assignError(error, QStringLiteral(
            "The firmware manifest plan exceeds its download bounds."));
        return {};
    }

    QHash<QString, QString> plannedPaths;
    for (const Download &download : plan.downloads) {
        const QString key = canonicalUrl(download.uri);
        const QString derivedPath = relativePath(download.uri);
        if (!allowedUrl(download.uri, false) || key.isEmpty()
            || download.relativePath.isEmpty()
            || derivedPath != download.relativePath
            || plannedPaths.contains(key)) {
            assignError(error, QStringLiteral(
                "The firmware manifest plan contains invalid download metadata."));
            return {};
        }
        plannedPaths.insert(key, download.relativePath);
    }
    for (auto it = successfulPaths.constBegin(); it != successfulPaths.constEnd(); ++it) {
        const auto planned = plannedPaths.constFind(it.key());
        if (planned == plannedPaths.constEnd() || planned.value() != it.value()) {
            assignError(error, QStringLiteral(
                "A successful download path does not match the immutable manifest plan."));
            return {};
        }
    }

    QDomDocument output = plan.document.cloneNode(true).toDocument();
    // Qt serializes QByteArray as UTF-8 but otherwise retains the input's XML
    // declaration, even if it said UTF-16 or Latin-1. Match MP10's XmlWriter.
    for (QDomNode node = output.firstChild(); !node.isNull();) {
        const QDomNode next = node.nextSibling();
        if (node.isProcessingInstruction() && node.nodeName() == QStringLiteral("xml"))
            output.removeChild(node);
        node = next;
    }
    output.insertBefore(output.createProcessingInstruction(QStringLiteral("xml"),
        QStringLiteral("version=\"1.0\" encoding=\"UTF-8\"")), output.firstChild());
    const QVector<QDomElement> elements = descendantElements(
        output.documentElement());
    QHash<QString, bool> documentUrls;
    for (const QDomElement &element : elements) {
        if (!localName(element).startsWith(
                QStringLiteral("url"), Qt::CaseInsensitive)) {
            continue;
        }
        const QString value = element.text().trimmed();
        if (value.isEmpty()) continue;
        const QUrl url(value, QUrl::StrictMode);
        const QString key = canonicalUrl(url);
        if (!allowedUrl(url, false) || !plannedPaths.contains(key)) {
            assignError(error, QStringLiteral(
                "The firmware manifest changed after it was parsed."));
            return {};
        }
        documentUrls.insert(key, true);
    }
    if (documentUrls.size() != plannedPaths.size()) {
        assignError(error, QStringLiteral(
            "The firmware manifest download plan no longer matches its XML."));
        return {};
    }

    for (QDomElement element : elements) {
        if (!localName(element).startsWith(
                QStringLiteral("url"), Qt::CaseInsensitive)) {
            continue;
        }
        const QString value = element.text().trimmed();
        if (value.isEmpty()) continue;
        const QUrl url(value, QUrl::StrictMode);
        const QString key = canonicalUrl(url);
        const auto replacement = successfulPaths.constFind(key);
        if (replacement != successfulPaths.constEnd()
            && !replaceElementText(&output, element, replacement.value())) {
            assignError(error, QStringLiteral(
                "Unable to rewrite a firmware manifest URL."));
            return {};
        }
    }
    return output.toByteArray(2);
}
