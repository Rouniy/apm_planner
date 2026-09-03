#include "CotEventSerializer.h"

#include <QList>
#include <QPair>

#include <cmath>

namespace {

using Attributes = QList<QPair<QString, QString>>;

QString xmlNewLine()
{
#ifdef Q_OS_WIN
    return QStringLiteral("\r\n");
#else
    return QStringLiteral("\n");
#endif
}

QString escapeAttribute(const QString &value)
{
    QString escaped;
    escaped.reserve(value.size());
    for (const QChar character : value) {
        switch (character.unicode()) {
        case '&':
            escaped += QStringLiteral("&amp;");
            break;
        case '<':
            escaped += QStringLiteral("&lt;");
            break;
        case '>':
            escaped += QStringLiteral("&gt;");
            break;
        case '"':
            escaped += QStringLiteral("&quot;");
            break;
        case '\r':
            escaped += QStringLiteral("&#xD;");
            break;
        case '\n':
            escaped += QStringLiteral("&#xA;");
            break;
        case '\t':
            escaped += QStringLiteral("&#x9;");
            break;
        default:
            escaped += character;
            break;
        }
    }
    return escaped;
}

class CotXmlWriter final
{
public:
    explicit CotXmlWriter(bool indent)
        : m_indent(indent)
    {
    }

    void startElement(const QString &name, const Attributes &attributes = {})
    {
        beginLine();
        m_xml += QLatin1Char('<') + name;
        appendAttributes(attributes);
        m_xml += QLatin1Char('>');
        ++m_depth;
    }

    void emptyElement(const QString &name, const Attributes &attributes = {})
    {
        beginLine();
        m_xml += QLatin1Char('<') + name;
        appendAttributes(attributes);
        m_xml += QStringLiteral(" />");
    }

    void endElement(const QString &name)
    {
        --m_depth;
        beginLine();
        m_xml += QStringLiteral("</") + name + QLatin1Char('>');
    }

    QString result() const { return m_xml; }

private:
    void beginLine()
    {
        if (!m_indent) {
            return;
        }
        if (!m_xml.isEmpty()) {
            // MP10 leaves XmlWriterSettings.NewLineChars at its default,
            // Environment.NewLine, so the indented wire form is native.
            m_xml += xmlNewLine();
        }
        m_xml += QString(m_depth * 2, QLatin1Char(' '));
    }

    void appendAttributes(const Attributes &attributes)
    {
        for (const auto &attribute : attributes) {
            if (m_indent) {
                m_xml += xmlNewLine();
                m_xml += QString((m_depth + 1) * 2, QLatin1Char(' '));
            } else {
                m_xml += QLatin1Char(' ');
            }
            m_xml += attribute.first;
            m_xml += QStringLiteral("=\"");
            m_xml += escapeAttribute(attribute.second);
            m_xml += QLatin1Char('"');
        }
    }

    bool m_indent = false;
    int m_depth = 0;
    QString m_xml;
};

QString timestamp(const QDateTime &instant)
{
    return instant.toUTC().toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss.zzz'Z'"));
}

QString decimal(double value, int places)
{
    // QString::number is locale independent, matching InvariantCulture.
    return QString::number(value, 'f', places);
}

QString globalCallsign(const CotEventSettings &settings, int systemId)
{
    if (settings.callsign.trimmed().isEmpty()) {
        return QString();
    }
    // MP10 appends the system ID first and trims the complete result.
    return (settings.callsign + QLatin1Char('-') + QString::number(systemId)).trimmed();
}

const CotIdentityOverride *advancedIdentity(const CotEventSettings &settings,
                                            const QString &eventUid,
                                            const CotIdentityOverride *identity)
{
    // MP10 does not pass an identity detail when advanced mode is disabled or
    // the selected row deliberately has an empty UID.
    return settings.advancedIdentityFields && !eventUid.isEmpty() ? identity : nullptr;
}

QString emittedCallsign(const CotEventSettings &settings, int systemId,
                        const CotIdentityOverride *advanced)
{
    if (advanced && !advanced->contactCallsign.isEmpty()) {
        return advanced->contactCallsign;
    }
    return globalCallsign(settings, systemId);
}

bool isValidXml10String(const QString &value)
{
    for (int index = 0; index < value.size(); ++index) {
        const ushort codeUnit = value.at(index).unicode();
        if (codeUnit == 0x0009 || codeUnit == 0x000A || codeUnit == 0x000D
            || (codeUnit >= 0x0020 && codeUnit <= 0xD7FF)
            || (codeUnit >= 0xE000 && codeUnit <= 0xFFFD)) {
            continue;
        }
        if (codeUnit >= 0xD800 && codeUnit <= 0xDBFF) {
            if (index + 1 >= value.size()) {
                return false;
            }
            const ushort lowSurrogate = value.at(index + 1).unicode();
            if (lowSurrogate < 0xDC00 || lowSurrogate > 0xDFFF) {
                return false;
            }
            ++index;
            continue;
        }
        // Includes lone low surrogates, forbidden C0 controls and FFFE/FFFF.
        return false;
    }
    return true;
}

bool validateXmlField(const QString &value, const QString &fieldName,
                      QString *errorMessage)
{
    if (isValidXml10String(value)) {
        return true;
    }
    if (errorMessage) {
        *errorMessage = QStringLiteral("CoT ") + fieldName
            + QStringLiteral(" contains a character that is not valid in XML 1.0.");
    }
    return false;
}

bool validateFinite(double value, const QString &fieldName, QString *errorMessage)
{
    if (std::isfinite(value)) {
        return true;
    }
    if (errorMessage) {
        *errorMessage = QStringLiteral("CoT navigation value ") + fieldName
            + QStringLiteral(" is not finite.");
    }
    return false;
}

QString serializeUnchecked(const CotEventSettings &settings,
                           int systemId, int componentId,
                           const CotNavigationState &navigation,
                           const QDateTime &timestampUtc,
                           const CotIdentityOverride *identity)
{
    const QDateTime time = timestampUtc.toUTC();
    const QString eventUid = CotEventSerializer::ResolveUid(
        settings, systemId, componentId, identity);
    const CotIdentityOverride *advanced = advancedIdentity(settings, eventUid, identity);

    CotXmlWriter writer(settings.indentXml);
    writer.startElement(QStringLiteral("event"), {
        {QStringLiteral("version"), QStringLiteral("2.0")},
        {QStringLiteral("uid"), eventUid},
        {QStringLiteral("type"), settings.eventType},
        {QStringLiteral("time"), timestamp(time)},
        {QStringLiteral("start"), timestamp(time.addSecs(-5))},
        {QStringLiteral("stale"), timestamp(time.addSecs(120))},
        {QStringLiteral("how"), QStringLiteral("m-g")},
    });

    writer.startElement(QStringLiteral("detail"));
    if (advanced && advanced->includeTakv) {
        writer.emptyElement(QStringLiteral("takv"));
    }

    const QString callsign = emittedCallsign(settings, systemId, advanced);
    const QString endpoint = advanced ? advanced->contactEndpoint : QString();
    Attributes contactAttributes;
    if (!callsign.isEmpty()) {
        contactAttributes.append({QStringLiteral("callsign"), callsign});
    }
    if (!endpoint.isEmpty()) {
        contactAttributes.append({QStringLiteral("endpoint"), endpoint});
    }
    if (!contactAttributes.isEmpty()) {
        writer.emptyElement(QStringLiteral("contact"), contactAttributes);
    }
    if (advanced && !advanced->vmf.isEmpty()) {
        writer.emptyElement(QStringLiteral("uid"), {
            {QStringLiteral("vmf"), advanced->vmf},
        });
    }
    writer.emptyElement(QStringLiteral("track"), {
        {QStringLiteral("course"), decimal(navigation.courseDegrees, 2)},
        {QStringLiteral("speed"), decimal(navigation.speedMetresPerSecond, 2)},
    });
    writer.endElement(QStringLiteral("detail"));

    // MP10 feeds AMSL metres into an attribute named "hae". Preserve that
    // interoperable output while keeping the source unit explicit in the type.
    writer.emptyElement(QStringLiteral("point"), {
        {QStringLiteral("lat"), decimal(navigation.latitudeDegrees, 7)},
        {QStringLiteral("lon"), decimal(navigation.longitudeDegrees, 7)},
        {QStringLiteral("hae"), decimal(navigation.altitudeAmslMetres, 2)},
        {QStringLiteral("ce"), QStringLiteral("1.0")},
        {QStringLiteral("le"), QStringLiteral("1.0")},
    });
    writer.endElement(QStringLiteral("event"));
    return writer.result();
}

} // namespace

QString CotEventSerializer::ResolveUid(const CotEventSettings &settings,
                                       int systemId, int componentId,
                                       const CotIdentityOverride *identity)
{
    if (identity) {
        return identity->uid; // A present row with an empty UID intentionally suppresses fallback.
    }
    return settings.uidPrefix + QLatin1Char('-') + QString::number(systemId)
        + QLatin1Char('-') + QString::number(componentId);
}

bool CotEventSerializer::Validate(const CotEventSettings &settings,
                                  int systemId, int componentId,
                                  const CotNavigationState &navigation,
                                  const QDateTime &timestampUtc,
                                  QString *errorMessage,
                                  const CotIdentityOverride *identity)
{
    if (errorMessage) {
        errorMessage->clear();
    }

    const QDateTime time = timestampUtc.toUTC();
    if (!timestampUtc.isValid() || !time.isValid()
        || !time.addSecs(-5).isValid() || !time.addSecs(120).isValid()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("CoT timestamp is invalid.");
        }
        return false;
    }

    if (!validateFinite(navigation.latitudeDegrees, QStringLiteral("latitude"), errorMessage)
        || !validateFinite(navigation.longitudeDegrees, QStringLiteral("longitude"), errorMessage)
        || !validateFinite(navigation.altitudeAmslMetres, QStringLiteral("altitude"), errorMessage)
        || !validateFinite(navigation.courseDegrees, QStringLiteral("course"), errorMessage)
        || !validateFinite(navigation.speedMetresPerSecond, QStringLiteral("speed"), errorMessage)) {
        return false;
    }

    const QString eventUid = ResolveUid(settings, systemId, componentId, identity);
    const CotIdentityOverride *advanced = advancedIdentity(settings, eventUid, identity);
    if (!validateXmlField(eventUid, QStringLiteral("event UID"), errorMessage)
        || !validateXmlField(settings.eventType, QStringLiteral("event type"), errorMessage)
        || !validateXmlField(emittedCallsign(settings, systemId, advanced),
                             QStringLiteral("contact callsign"), errorMessage)) {
        return false;
    }
    if (advanced
        && (!validateXmlField(advanced->contactEndpoint,
                              QStringLiteral("contact endpoint"), errorMessage)
            || !validateXmlField(advanced->vmf, QStringLiteral("VMF UID"), errorMessage))) {
        return false;
    }
    return true;
}

bool CotEventSerializer::TrySerialize(const CotEventSettings &settings,
                                      int systemId, int componentId,
                                      const CotNavigationState &navigation,
                                      const QDateTime &timestampUtc,
                                      QString *xml,
                                      QString *errorMessage,
                                      const CotIdentityOverride *identity)
{
    if (!xml) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Missing CoT XML output.");
        }
        return false;
    }
    *xml = QString();
    if (!Validate(settings, systemId, componentId, navigation, timestampUtc,
                  errorMessage, identity)) {
        return false;
    }
    *xml = serializeUnchecked(settings, systemId, componentId, navigation,
                              timestampUtc, identity);
    return true;
}

QString CotEventSerializer::Serialize(const CotEventSettings &settings,
                                      int systemId, int componentId,
                                      const CotNavigationState &navigation,
                                      const QDateTime &timestampUtc,
                                      const CotIdentityOverride *identity)
{
    QString xml;
    if (!TrySerialize(settings, systemId, componentId, navigation, timestampUtc,
                      &xml, nullptr, identity)) {
        return QString();
    }
    return xml;
}
