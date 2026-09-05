#include "WarningEngine.h"

#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QPointer>
#include <QSaveFile>
#include <QSet>
#include <QTimer>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <cmath>
#include <limits>
#include <utility>

namespace {
constexpr qint64 MaximumXmlBytes = 4 * 1024 * 1024;
constexpr int MaximumRules = 512;
constexpr int MaximumDepth = 16;
constexpr int MaximumNameLength = 256;
constexpr int MaximumTextLength = 4096;
const QString XsiNamespace = QStringLiteral("http://www.w3.org/2001/XMLSchema-instance");

bool fail(QString *error, const QString &message)
{
    if (error) *error = message;
    return false;
}

bool xmlString(const QString &text)
{
    for (int i = 0; i < text.size(); ++i) {
        const ushort c = text.at(i).unicode();
        if (QChar::isHighSurrogate(c)) {
            if (++i >= text.size() || !text.at(i).isLowSurrogate()) return false;
        } else if (QChar::isLowSurrogate(c) || c == 0xfffe || c == 0xffff
                   || (c < 0x20 && c != '\t' && c != '\n' && c != '\r')) {
            return false;
        }
    }
    return true;
}

bool validateRule(const CustomWarning &rule, int depth, int &count,
                  QSet<quint64> &ids, quint64 &highestId, QString *error)
{
    if (depth > MaximumDepth || ++count > MaximumRules)
        return fail(error, QStringLiteral("Warnings exceed the 16-level / 512-condition limit."));
    if (rule.child.size() > 1)
        return fail(error, QStringLiteral("Each condition may have only one AND child."));
    if (rule.name.size() > MaximumNameLength || rule.text.size() > MaximumTextLength
        || !xmlString(rule.name) || !xmlString(rule.text))
        return fail(error, QStringLiteral("Warning name or text is too long or contains invalid XML characters."));
    if (!std::isfinite(rule.threshold) || rule.repeatSeconds < 0 || rule.repeatSeconds > 86400)
        return fail(error, QStringLiteral("Threshold must be finite and repeat must be 0..86400 seconds."));
    if (rule.condition < CustomWarning::NONE || rule.condition > CustomWarning::NEQ
        || rule.type < CustomWarning::SpeakAndText || rule.type > CustomWarning::Coloring
        || !WarningEngine::colorNames().contains(rule.color))
        return fail(error, QStringLiteral("Unknown warning condition, action or color."));
    if (rule.id) {
        if (ids.contains(rule.id) || rule.id == std::numeric_limits<quint64>::max())
            return fail(error, QStringLiteral("Duplicate or exhausted warning identity."));
        ids.insert(rule.id);
        highestId = qMax(highestId, rule.id);
    }
    for (const CustomWarning &child : rule.child)
        if (!validateRule(child, depth + 1, count, ids, highestId, error)) return false;
    return true;
}

bool assignIds(CustomWarning &rule, quint64 &next, QString *error)
{
    if (!rule.id) {
        if (next == std::numeric_limits<quint64>::max())
            return fail(error, QStringLiteral("Warning identity space is exhausted."));
        rule.id = next++;
    }
    for (CustomWarning &child : rule.child)
        if (!assignIds(child, next, error)) return false;
    return true;
}

bool prepareRules(QVector<CustomWarning> &rules, quint64 &nextId, QString *error)
{
    QSet<quint64> ids;
    quint64 highestId = 0;
    int count = 0;
    if (rules.size() > MaximumRules)
        return fail(error, QStringLiteral("Too many warning conditions (maximum 512)."));
    for (const CustomWarning &rule : rules)
        if (!validateRule(rule, 1, count, ids, highestId, error)) return false;
    nextId = qMax(nextId, highestId + 1);
    for (CustomWarning &rule : rules)
        if (!assignIds(rule, nextId, error)) return false;
    return true;
}

// XmlSerializer uses an unqualified ArrayOfCustomWarning document with one
// optional Child element containing the same public fields/properties. Do not
// silently repair names: an unavailable field must remain unavailable.
bool checkAttributes(QXmlStreamReader &xml, bool permitNil, bool *isNil = nullptr)
{
    bool nil = false;
    for (const QXmlStreamAttribute &attribute : xml.attributes()) {
        if (permitNil && attribute.namespaceUri() == XsiNamespace
            && attribute.name() == QLatin1String("nil")) {
            if (attribute.value() == QLatin1String("true") || attribute.value() == QLatin1String("1"))
                nil = true;
            else if (attribute.value() != QLatin1String("false") && attribute.value() != QLatin1String("0"))
                xml.raiseError(QStringLiteral("Invalid xsi:nil value."));
        } else {
            xml.raiseError(QStringLiteral("Unexpected warning XML attribute."));
        }
    }
    if (!xml.namespaceUri().isEmpty()) xml.raiseError(QStringLiteral("Unexpected warning XML namespace."));
    if (isNil) *isNil = nil;
    return !xml.hasError();
}

bool nextElement(QXmlStreamReader &xml)
{
    while (!xml.atEnd()) {
        const auto token = xml.readNext();
        if (token == QXmlStreamReader::DTD || token == QXmlStreamReader::EntityReference) {
            xml.raiseError(QStringLiteral("DTD and entity declarations are not permitted."));
            return false;
        }
        if (token == QXmlStreamReader::StartElement) return true;
        if (token == QXmlStreamReader::EndElement) return false;
        if (token == QXmlStreamReader::Characters && !xml.isWhitespace()) {
            xml.raiseError(QStringLiteral("Unexpected text outside a warning field."));
            return false;
        }
    }
    return false;
}

QString readScalar(QXmlStreamReader &xml, bool permitNil, bool *nil = nullptr)
{
    bool isNil = false;
    if (!checkAttributes(xml, permitNil, &isNil)) return {};
    QString value;
    while (!xml.atEnd()) {
        const auto token = xml.readNext();
        if (token == QXmlStreamReader::EndElement) break;
        if (token == QXmlStreamReader::Characters) {
            value += xml.text();
            if (value.size() > MaximumTextLength)
                xml.raiseError(QStringLiteral("Warning XML field exceeds 4096 characters."));
        } else if (token != QXmlStreamReader::Comment && token != QXmlStreamReader::ProcessingInstruction) {
            xml.raiseError(QStringLiteral("Warning fields must contain scalar text only."));
        }
    }
    if (isNil && !value.trimmed().isEmpty()) xml.raiseError(QStringLiteral("A nil field contains a value."));
    if (nil) *nil = isNil;
    return isNil ? QString() : value;
}

bool readRule(QXmlStreamReader &xml, CustomWarning &rule, int depth, int &count, bool childObject = false)
{
    if (depth > MaximumDepth || ++count > MaximumRules) {
        xml.raiseError(QStringLiteral("Warnings exceed the 16-level / 512-condition limit."));
        return false;
    }
    if (!checkAttributes(xml, childObject)) return false;
    QSet<QString> seen;
    while (nextElement(xml)) {
        const QString field = xml.name().toString();
        if (seen.contains(field)) {
            xml.raiseError(QStringLiteral("Duplicate warning field: %1").arg(field));
            return false;
        }
        seen.insert(field);
        if (field == QLatin1String("Child")) {
            bool nil = false;
            if (!checkAttributes(xml, true, &nil)) return false;
            if (nil) {
                // Consume a null Child without allowing hidden nested data.
                readScalar(xml, true);
            } else {
                // Child itself is the object, not a wrapper for CustomWarning.
                CustomWarning child;
                if (!readRule(xml, child, depth + 1, count, true)) return false;
                rule.child.append(child);
            }
            continue;
        }
        const bool stringField = field == QLatin1String("Name") || field == QLatin1String("Text")
                                 || field == QLatin1String("color");
        bool nil = false;
        const QString value = readScalar(xml, stringField, &nil);
        if (xml.hasError()) return false;
        if (field == QLatin1String("Name")) rule.name = value;
        else if (field == QLatin1String("Text")) rule.text = value;
        else if (field == QLatin1String("color")) rule.color = nil ? QStringLiteral("NoColor") : value;
        else if (field == QLatin1String("Warning")) {
            bool ok = false;
            QLocale locale = QLocale::c();
            locale.setNumberOptions(QLocale::RejectGroupSeparator);
            rule.threshold = locale.toDouble(value.trimmed(), &ok);
            if (!ok || !std::isfinite(rule.threshold)) xml.raiseError(QStringLiteral("Invalid finite warning threshold."));
        } else if (field == QLatin1String("RepeatTime")) {
            bool ok = false;
            rule.repeatSeconds = value.trimmed().toInt(&ok);
            if (!ok || rule.repeatSeconds < 0 || rule.repeatSeconds > 86400)
                xml.raiseError(QStringLiteral("Invalid repeat interval (0..86400 seconds)."));
        } else if (field == QLatin1String("ConditionType")) {
            const int index = WarningEngine::conditionNames().indexOf(value.trimmed());
            if (index < 0) xml.raiseError(QStringLiteral("Unknown ConditionType token."));
            else rule.condition = static_cast<CustomWarning::Conditional>(index);
        } else if (field == QLatin1String("type")) {
            const int index = WarningEngine::typeNames().indexOf(value.trimmed());
            if (index < 0) xml.raiseError(QStringLiteral("Unknown warning type token."));
            else rule.type = static_cast<CustomWarning::WarningType>(index);
        } else {
            xml.raiseError(QStringLiteral("Unknown warning XML field: %1").arg(field));
        }
        if (xml.hasError()) return false;
    }
    return !xml.hasError();
}

bool parseXml(const QByteArray &bytes, QVector<CustomWarning> &rules, QString *error)
{
    QXmlStreamReader xml(bytes);
    if (!nextElement(xml) || xml.name() != QLatin1String("ArrayOfCustomWarning"))
        return fail(error, QStringLiteral("Expected ArrayOfCustomWarning XML document."));
    if (!checkAttributes(xml, false)) return fail(error, xml.errorString());
    int count = 0;
    while (nextElement(xml)) {
        if (xml.name() != QLatin1String("CustomWarning")) {
            xml.raiseError(QStringLiteral("Expected CustomWarning element."));
            break;
        }
        CustomWarning rule;
        if (!readRule(xml, rule, 1, count)) break;
        rules.append(rule);
    }
    if (!xml.hasError()) {
        // Consume the complete document so trailing garbage/second roots fail.
        while (!xml.atEnd()) {
            const auto token = xml.readNext();
            if (token == QXmlStreamReader::StartElement || token == QXmlStreamReader::DTD
                || token == QXmlStreamReader::EntityReference
                || (token == QXmlStreamReader::Characters && !xml.isWhitespace()))
                xml.raiseError(QStringLiteral("Unexpected content after warning document."));
        }
    }
    return xml.hasError() ? fail(error, QStringLiteral("Warning XML line %1: %2")
                                          .arg(xml.lineNumber()).arg(xml.errorString())) : true;
}

void writeRule(QXmlStreamWriter &xml, const CustomWarning &rule, const QString &element)
{
    xml.writeStartElement(element);
    if (!rule.child.isEmpty()) writeRule(xml, rule.child.constFirst(), QStringLiteral("Child"));
    xml.writeTextElement(QStringLiteral("Name"), rule.name);
    xml.writeTextElement(QStringLiteral("Warning"), QString::number(rule.threshold, 'g', 17));
    xml.writeTextElement(QStringLiteral("type"), WarningEngine::typeNames().at(rule.type));
    xml.writeTextElement(QStringLiteral("color"), rule.color);
    xml.writeTextElement(QStringLiteral("RepeatTime"), QString::number(rule.repeatSeconds));
    xml.writeTextElement(QStringLiteral("ConditionType"), WarningEngine::conditionNames().at(rule.condition));
    xml.writeTextElement(QStringLiteral("Text"), rule.text);
    xml.writeEndElement();
}

bool matches(const CustomWarning &rule, const WarningEngine::Values &values)
{
    const auto found = values.constFind(rule.name);
    if (found == values.cend() || !std::isfinite(found.value())) return false;
    const double value = found.value();
    bool condition = false;
    switch (rule.condition) {
    case CustomWarning::NONE: break;
    case CustomWarning::LT: condition = value < rule.threshold; break;
    case CustomWarning::LTEQ: condition = value <= rule.threshold; break;
    case CustomWarning::EQ: condition = value == rule.threshold; break;
    case CustomWarning::GT: condition = value > rule.threshold; break;
    case CustomWarning::GTEQ: condition = value >= rule.threshold; break;
    case CustomWarning::NEQ: condition = value != rule.threshold; break;
    }
    return condition && (rule.child.isEmpty() || matches(rule.child.constFirst(), values));
}

QString displayNumber(double value)
{
    QString result = QString::number(value, 'f', 2);
    while (result.endsWith(QLatin1Char('0'))) result.chop(1);
    if (result.endsWith(QLatin1Char('.'))) result.chop(1);
    return result == QLatin1String("-0") ? QStringLiteral("0") : result;
}
} // namespace

WarningEngine::WarningEngine(QString configPath, ValueProvider provider, Clock clock, QObject *parent)
    : QObject(parent), m_configPath(std::move(configPath)), m_provider(std::move(provider)),
      m_clock(std::move(clock)), m_timer(new QTimer(this))
{
    m_elapsed.start();
    m_timer->setInterval(250);
    connect(m_timer, &QTimer::timeout, this, &WarningEngine::tick);
}

QStringList WarningEngine::conditionNames()
{
    return {QStringLiteral("NONE"), QStringLiteral("LT"), QStringLiteral("LTEQ"), QStringLiteral("EQ"),
            QStringLiteral("GT"), QStringLiteral("GTEQ"), QStringLiteral("NEQ")};
}

QStringList WarningEngine::typeNames()
{
    return {QStringLiteral("SpeakAndText"), QStringLiteral("Coloring")};
}

QStringList WarningEngine::colorNames()
{
    return {QStringLiteral("NoColor"), QStringLiteral("Red"), QStringLiteral("OrangeRed"),
            QStringLiteral("Maroon"), QStringLiteral("Yellow"), QStringLiteral("Gold"),
            QStringLiteral("Goldenrod"), QStringLiteral("LawnGreen"), QStringLiteral("Green"),
            QStringLiteral("DarkGreen")};
}

QString WarningEngine::formatText(const CustomWarning &rule, double value)
{
    return QString(rule.text).replace(QStringLiteral("{warning}"), displayNumber(rule.threshold))
        .replace(QStringLiteral("{value}"), displayNumber(value)).replace(QStringLiteral("{name}"), rule.name);
}

bool WarningEngine::setRules(QVector<CustomWarning> rules, QString *error)
{
    if (error) error->clear();
    quint64 next = m_nextId;
    if (!prepareRules(rules, next, error)) return false;
    QSet<quint64> liveIds;
    QSet<QString> coloredFields;
    for (const auto &rule : rules) {
        liveIds.insert(rule.id);
        if (rule.type == CustomWarning::Coloring) coloredFields.insert(rule.name);
    }
    for (auto it = m_lastSpoken.begin(); it != m_lastSpoken.end();) {
        if (!liveIds.contains(it.key())) it = m_lastSpoken.erase(it);
        else ++it;
    }
    const auto previousColors = m_colors;
    for (auto it = m_colors.begin(); it != m_colors.end();) {
        if (!coloredFields.contains(it.key())) it = m_colors.erase(it);
        else ++it;
    }
    const bool clearColors = previousColors != m_colors;
    m_rules = std::move(rules);
    m_nextId = next;
    // Stable rules retain their repeat countdown during any editor change.
    // Recompute still-owned colors at the next tick without an empty flash.
    m_dirty = true;
    const quint64 revision = ++m_revision;
    QPointer<WarningEngine> guard(this);
    if (clearColors) emit colorsChanged();
    if (!guard || m_revision != revision) return true;
    emit rulesChanged(); // Final callback boundary: callers may edit or delete us.
    return true;
}

bool WarningEngine::load(QString *error)
{
    if (error) error->clear();
    QVector<CustomWarning> rules;
    QFile input(m_configPath);
    if (QFileInfo::exists(m_configPath)) {
        if (!input.open(QIODevice::ReadOnly)) return fail(error, input.errorString());
        if (input.size() > MaximumXmlBytes) return fail(error, QStringLiteral("Warning XML exceeds 4 MiB."));
        const QByteArray bytes = input.read(MaximumXmlBytes + 1);
        if (input.error() != QFileDevice::NoError) return fail(error, input.errorString());
        if (bytes.size() > MaximumXmlBytes || !input.atEnd())
            return fail(error, QStringLiteral("Warning XML exceeds 4 MiB."));
        if (!parseXml(bytes, rules, error)) return false;
    }
    quint64 next = m_nextId;
    if (!prepareRules(rules, next, error)) return false;
    const bool clearColors = !m_colors.isEmpty();
    m_rules = std::move(rules);
    m_nextId = next;
    m_lastSpoken.clear();
    m_colors.clear();
    m_dirty = false;
    const quint64 revision = ++m_revision;
    QPointer<WarningEngine> guard(this);
    if (clearColors) emit colorsChanged();
    if (!guard || m_revision != revision) return true;
    emit rulesChanged();
    return true;
}

bool WarningEngine::save(QString *error)
{
    if (error) error->clear();
    // Serialize before opening the destination. Validation is already enforced
    // by setRules/load; runtime identities deliberately never enter the XML.
    QByteArray bytes;
    QXmlStreamWriter xml(&bytes);
    xml.setAutoFormatting(true);
    xml.writeStartDocument();
    xml.writeStartElement(QStringLiteral("ArrayOfCustomWarning"));
    xml.writeNamespace(XsiNamespace, QStringLiteral("xsi"));
    xml.writeNamespace(QStringLiteral("http://www.w3.org/2001/XMLSchema"), QStringLiteral("xsd"));
    for (const CustomWarning &rule : m_rules) writeRule(xml, rule, QStringLiteral("CustomWarning"));
    xml.writeEndElement();
    xml.writeEndDocument();
    if (xml.hasError() || bytes.size() > MaximumXmlBytes)
        return fail(error, QStringLiteral("Cannot serialize warnings within the 4 MiB XML limit."));
    QSaveFile output(m_configPath);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly)) return fail(error, output.errorString());
    if (output.write(bytes) != bytes.size()) return fail(error, output.errorString());
    if (!output.commit()) return fail(error, output.errorString());
    m_dirty = false;
    emit rulesChanged();
    return true;
}

void WarningEngine::setRunning(bool running)
{
    if (running) m_timer->start();
    else {
        m_timer->stop();
        resetEpoch();
    }
}

void WarningEngine::resetEpoch()
{
    ++m_revision;
    m_lastSpoken.clear();
    const bool changed = !m_colors.isEmpty();
    m_colors.clear();
    if (changed) emit colorsChanged();
}

void WarningEngine::tick()
{
    if (m_ticking) return;
    m_ticking = true;
    QPointer<WarningEngine> guard(this);
    struct FinishTick {
        std::function<void()> finish;
        ~FinishTick() { finish(); }
    } finish{[guard] { if (guard) guard->m_ticking = false; }};
    const quint64 revision = m_revision;
    const auto provider = m_provider;
    const auto clock = m_clock;
    const QVector<CustomWarning> rules = m_rules;
    auto lastSpoken = m_lastSpoken;
    Values values;
    qint64 now = 0;
    try {
        if (provider) values = provider();
        if (!guard || m_revision != revision) return;
        now = clock ? clock() : m_elapsed.elapsed();
        if (!guard || m_revision != revision) return;
    } catch (...) {
        // A failed source is an empty snapshot, never a fabricated zero.
        if (!guard || m_revision != revision) return;
        values.clear();
    }
    Colors colors;
    QStringList messages;
    for (const CustomWarning &rule : rules) {
        if (!matches(rule, values)) continue;
        if (rule.type == CustomWarning::Coloring) {
            if (rule.color == QLatin1String("NoColor")) colors.remove(rule.name);
            else colors.insert(rule.name, rule.color);
            continue;
        }
        const auto last = lastSpoken.constFind(rule.id);
        // An injected clock can roll back; treat that as an unavailable timing
        // interval rather than overflowing subtraction or bursting speech.
        if (last != lastSpoken.cend()
            && (now < last.value() || static_cast<quint64>(now) - static_cast<quint64>(last.value())
                < static_cast<quint64>(rule.repeatSeconds) * 1000)) continue;
        lastSpoken.insert(rule.id, now);
        messages.append(formatText(rule, values.value(rule.name)));
    }
    const bool colorChanged = m_colors != colors;
    m_colors = std::move(colors);
    m_lastSpoken = std::move(lastSpoken);
    if (colorChanged) {
        emit colorsChanged();
        if (!guard || m_revision != revision) return;
    }
    for (const QString &message : messages) {
        emit warningMessage(message);
        if (!guard || m_revision != revision) return;
    }
}
