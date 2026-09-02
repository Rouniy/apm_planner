#include "QmlPluginManifest.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace {

const QSet<QString> kTopLevelFields{
    QStringLiteral("manifestVersion"),
    QStringLiteral("id"),
    QStringLiteral("name"),
    QStringLiteral("version"),
    QStringLiteral("apiMajor"),
    QStringLiteral("main"),
    QStringLiteral("ui"),
    QStringLiteral("description"),
    QStringLiteral("author")
};

const QSet<QString> kRequiredTopLevelFields{
    QStringLiteral("manifestVersion"),
    QStringLiteral("id"),
    QStringLiteral("name"),
    QStringLiteral("version"),
    QStringLiteral("apiMajor"),
    QStringLiteral("main"),
    QStringLiteral("ui")
};

const QSet<QString> kUiFields{
    QStringLiteral("type"),
    QStringLiteral("title"),
    QStringLiteral("icon")
};

const QSet<QString> kRequiredUiFields{
    QStringLiteral("type"),
    QStringLiteral("title")
};

QString firstUnknownField(const QJsonObject &object,
                          const QSet<QString> &allowed)
{
    QStringList unknownFields;
    const QStringList fields = object.keys();
    for (const QString &field : fields) {
        if (!allowed.contains(field)) {
            unknownFields.append(field);
        }
    }
    std::sort(unknownFields.begin(), unknownFields.end());
    return unknownFields.isEmpty() ? QString() : unknownFields.constFirst();
}

QString firstMissingField(const QJsonObject &object,
                          const QSet<QString> &required)
{
    QStringList missingFields;
    for (const QString &field : required) {
        if (!object.contains(field)) {
            missingFields.append(field);
        }
    }
    std::sort(missingFields.begin(), missingFields.end());
    return missingFields.isEmpty() ? QString() : missingFields.constFirst();
}

bool isExactInteger(const QJsonValue &value, int expected)
{
    if (!value.isDouble()) {
        return false;
    }
    const double number = value.toDouble();
    return std::isfinite(number)
        && number == static_cast<double>(expected);
}

bool isValidPluginId(const QString &id)
{
    static const QRegularExpression expression(QStringLiteral(
        "^[a-z][a-z0-9]*(?:-[a-z0-9]+)*"
        "(?:\\.[a-z][a-z0-9]*(?:-[a-z0-9]+)*)+$"));
    return expression.match(id).hasMatch();
}

bool isValidSemVer(const QString &version)
{
    static const QRegularExpression expression(QStringLiteral(
        "^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)"
        "(?:-([0-9A-Za-z-]+(?:\\.[0-9A-Za-z-]+)*))?"
        "(?:\\+([0-9A-Za-z-]+(?:\\.[0-9A-Za-z-]+)*))?$"));
    const QRegularExpressionMatch match = expression.match(version);
    if (!match.hasMatch()) {
        return false;
    }

    const QString prerelease = match.captured(4);
    if (prerelease.isEmpty()) {
        return true;
    }
    const QStringList identifiers = prerelease.split(QLatin1Char('.'));
    static const QRegularExpression numeric(QStringLiteral("^[0-9]+$"));
    for (const QString &identifier : identifiers) {
        if (numeric.match(identifier).hasMatch()
            && identifier.size() > 1
            && identifier.startsWith(QLatin1Char('0'))) {
            return false;
        }
    }
    return true;
}

bool isLexicallySafeRelativePath(const QString &path)
{
    if (path.isEmpty() || path.trimmed() != path
        || path.contains(QLatin1Char('\\'))
        || QDir::isAbsolutePath(path)
        || path.startsWith(QLatin1Char('/'))
        || QRegularExpression(QStringLiteral("^[A-Za-z]:"))
               .match(path).hasMatch()) {
        return false;
    }

    const QStringList segments = path.split(QLatin1Char('/'));
    for (const QString &segment : segments) {
        if (segment == QStringLiteral("..")) {
            return false;
        }
    }
    return QDir::cleanPath(path) != QStringLiteral(".");
}

QString absolutePluginDirectory(const QString &pluginDirectory)
{
    const QFileInfo directoryInfo(pluginDirectory);
    const QString canonical = directoryInfo.canonicalFilePath();
    if (!canonical.isEmpty()) {
        return QDir::cleanPath(canonical);
    }
    return QDir::cleanPath(directoryInfo.absoluteFilePath());
}

bool existingPathStaysInsidePlugin(const QString &pluginDirectory,
                                   const QString &relativePath,
                                   QString *resolvedPath)
{
    const QString candidate = QDir(pluginDirectory).absoluteFilePath(
        QDir::cleanPath(relativePath));
    const QFileInfo candidateInfo(candidate);
    *resolvedPath = QDir::cleanPath(candidateInfo.absoluteFilePath());

    if (!candidateInfo.exists()) {
        return true;
    }
    if (!candidateInfo.isFile()) {
        return false;
    }

    const QString canonicalCandidate = candidateInfo.canonicalFilePath();
    if (canonicalCandidate.isEmpty()) {
        return false;
    }
    const QString relativeCanonical = QDir(pluginDirectory).relativeFilePath(
        canonicalCandidate);
    return !QDir::isAbsolutePath(relativeCanonical)
        && relativeCanonical != QStringLiteral("..")
        && !relativeCanonical.startsWith(QStringLiteral("../"));
}

bool isNonEmptyString(const QJsonValue &value)
{
    return value.isString() && !value.toString().trimmed().isEmpty();
}

} // namespace

QmlPluginManifest QmlPluginManifest::load(const QString &manifestFilePath)
{
    const QFileInfo manifestInfo(manifestFilePath);
    const QString absoluteManifestPath = QDir::cleanPath(
        manifestInfo.absoluteFilePath());
    const QString pluginDirectory = absolutePluginDirectory(
        manifestInfo.absolutePath());

    QFile file(absoluteManifestPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return failure(Error::CannotOpen,
                       QStringLiteral("Cannot open plugin manifest '%1': %2")
                           .arg(absoluteManifestPath, file.errorString()),
                       pluginDirectory, absoluteManifestPath);
    }

    QmlPluginManifest manifest = parse(file.readAll(), pluginDirectory);
    manifest.m_manifestFilePath = absoluteManifestPath;
    return manifest;
}

QmlPluginManifest QmlPluginManifest::parse(const QByteArray &json,
                                           const QString &pluginDirectory)
{
    const QString absoluteDirectory = absolutePluginDirectory(pluginDirectory);
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        return failure(Error::InvalidJson,
                       QStringLiteral("Invalid plugin manifest JSON at offset %1: %2")
                           .arg(parseError.offset)
                           .arg(parseError.errorString()),
                       absoluteDirectory);
    }
    if (!document.isObject()) {
        return failure(Error::RootNotObject,
                       QStringLiteral("Plugin manifest root must be an object"),
                       absoluteDirectory);
    }

    const QJsonObject root = document.object();
    const QString unknownTopLevel = firstUnknownField(root, kTopLevelFields);
    if (!unknownTopLevel.isEmpty()) {
        return failure(Error::UnknownField,
                       QStringLiteral("Unknown plugin manifest field '%1'")
                           .arg(unknownTopLevel),
                       absoluteDirectory);
    }
    const QString missingTopLevel = firstMissingField(
        root, kRequiredTopLevelFields);
    if (!missingTopLevel.isEmpty()) {
        return failure(Error::MissingField,
                       QStringLiteral("Missing required plugin manifest field '%1'")
                           .arg(missingTopLevel),
                       absoluteDirectory);
    }

    if (!root.value(QStringLiteral("manifestVersion")).isDouble()) {
        return failure(Error::WrongType,
                       QStringLiteral("'manifestVersion' must be an integer"),
                       absoluteDirectory);
    }
    if (!isExactInteger(root.value(QStringLiteral("manifestVersion")),
                        SupportedManifestVersion)) {
        return failure(Error::UnsupportedManifestVersion,
                       QStringLiteral("Unsupported plugin manifest version"),
                       absoluteDirectory);
    }

    if (!root.value(QStringLiteral("id")).isString()) {
        return failure(Error::WrongType,
                       QStringLiteral("'id' must be a string"),
                       absoluteDirectory);
    }
    const QString id = root.value(QStringLiteral("id")).toString();
    if (!isValidPluginId(id)) {
        return failure(Error::InvalidId,
                       QStringLiteral("'id' must be a lowercase reverse-DNS identifier"),
                       absoluteDirectory);
    }

    if (!isNonEmptyString(root.value(QStringLiteral("name")))) {
        return failure(Error::WrongType,
                       QStringLiteral("'name' must be a non-empty string"),
                       absoluteDirectory);
    }

    if (!root.value(QStringLiteral("version")).isString()) {
        return failure(Error::WrongType,
                       QStringLiteral("'version' must be a string"),
                       absoluteDirectory);
    }
    const QString version = root.value(QStringLiteral("version")).toString();
    if (!isValidSemVer(version)) {
        return failure(Error::InvalidVersion,
                       QStringLiteral("'version' must be a valid semantic version"),
                       absoluteDirectory);
    }

    if (!root.value(QStringLiteral("apiMajor")).isDouble()) {
        return failure(Error::WrongType,
                       QStringLiteral("'apiMajor' must be an integer"),
                       absoluteDirectory);
    }
    if (!isExactInteger(root.value(QStringLiteral("apiMajor")),
                        SupportedApiMajor)) {
        return failure(Error::UnsupportedApiMajor,
                       QStringLiteral("Unsupported QML plugin API major version"),
                       absoluteDirectory);
    }

    if (!root.value(QStringLiteral("main")).isString()) {
        return failure(Error::WrongType,
                       QStringLiteral("'main' must be a string"),
                       absoluteDirectory);
    }
    const QString main = root.value(QStringLiteral("main")).toString();
    if (!isLexicallySafeRelativePath(main)
        || !main.endsWith(QStringLiteral(".qml"))) {
        return failure(Error::InvalidPath,
                       QStringLiteral("'main' must be a safe relative .qml path"),
                       absoluteDirectory);
    }
    QString mainFilePath;
    if (!existingPathStaysInsidePlugin(absoluteDirectory, main,
                                       &mainFilePath)) {
        return failure(Error::PathEscapesPlugin,
                       QStringLiteral("'main' resolves outside the plugin directory or is not a file"),
                       absoluteDirectory);
    }

    if (!root.value(QStringLiteral("ui")).isObject()) {
        return failure(Error::WrongType,
                       QStringLiteral("'ui' must be an object"),
                       absoluteDirectory);
    }
    const QJsonObject ui = root.value(QStringLiteral("ui")).toObject();
    const QString unknownUi = firstUnknownField(ui, kUiFields);
    if (!unknownUi.isEmpty()) {
        return failure(Error::UnknownField,
                       QStringLiteral("Unknown plugin UI field '%1'")
                           .arg(unknownUi),
                       absoluteDirectory);
    }
    const QString missingUi = firstMissingField(ui, kRequiredUiFields);
    if (!missingUi.isEmpty()) {
        return failure(Error::MissingField,
                       QStringLiteral("Missing required plugin UI field '%1'")
                           .arg(missingUi),
                       absoluteDirectory);
    }
    if (!ui.value(QStringLiteral("type")).isString()) {
        return failure(Error::WrongType,
                       QStringLiteral("'ui.type' must be a string"),
                       absoluteDirectory);
    }
    const QString uiType = ui.value(QStringLiteral("type")).toString();
    if (uiType != QStringLiteral("toolPage")) {
        return failure(Error::InvalidUiType,
                       QStringLiteral("'ui.type' must be 'toolPage'"),
                       absoluteDirectory);
    }
    if (!isNonEmptyString(ui.value(QStringLiteral("title")))) {
        return failure(Error::WrongType,
                       QStringLiteral("'ui.title' must be a non-empty string"),
                       absoluteDirectory);
    }

    QString icon;
    QString iconFilePath;
    if (ui.contains(QStringLiteral("icon"))) {
        if (!ui.value(QStringLiteral("icon")).isString()) {
            return failure(Error::WrongType,
                           QStringLiteral("'ui.icon' must be a string"),
                           absoluteDirectory);
        }
        icon = ui.value(QStringLiteral("icon")).toString();
        if (!isLexicallySafeRelativePath(icon)) {
            return failure(Error::InvalidPath,
                           QStringLiteral("'ui.icon' must be a safe relative path"),
                           absoluteDirectory);
        }
        if (!existingPathStaysInsidePlugin(absoluteDirectory, icon,
                                           &iconFilePath)) {
            return failure(Error::PathEscapesPlugin,
                           QStringLiteral("'ui.icon' resolves outside the plugin directory or is not a file"),
                           absoluteDirectory);
        }
    }

    for (const QString &optionalString : {QStringLiteral("description"),
                                          QStringLiteral("author")}) {
        if (root.contains(optionalString)
            && !root.value(optionalString).isString()) {
            return failure(Error::WrongType,
                           QStringLiteral("'%1' must be a string")
                               .arg(optionalString),
                           absoluteDirectory);
        }
    }

    QmlPluginManifest manifest;
    manifest.m_error = Error::None;
    manifest.m_manifestVersion = SupportedManifestVersion;
    manifest.m_id = id;
    manifest.m_name = root.value(QStringLiteral("name")).toString();
    manifest.m_version = version;
    manifest.m_apiMajor = SupportedApiMajor;
    manifest.m_main = main;
    manifest.m_mainFilePath = mainFilePath;
    manifest.m_ui.type = uiType;
    manifest.m_ui.title = ui.value(QStringLiteral("title")).toString();
    manifest.m_ui.icon = icon;
    manifest.m_ui.iconFilePath = iconFilePath;
    manifest.m_description = root.value(QStringLiteral("description")).toString();
    manifest.m_author = root.value(QStringLiteral("author")).toString();
    manifest.m_pluginDirectory = absoluteDirectory;
    return manifest;
}

bool QmlPluginManifest::isValid() const
{
    return m_error == Error::None;
}

QmlPluginManifest::Error QmlPluginManifest::error() const
{
    return m_error;
}

QString QmlPluginManifest::errorString() const
{
    return m_errorString;
}

int QmlPluginManifest::manifestVersion() const { return m_manifestVersion; }
QString QmlPluginManifest::id() const { return m_id; }
QString QmlPluginManifest::name() const { return m_name; }
QString QmlPluginManifest::version() const { return m_version; }
int QmlPluginManifest::apiMajor() const { return m_apiMajor; }
QString QmlPluginManifest::main() const { return m_main; }
QString QmlPluginManifest::mainFilePath() const { return m_mainFilePath; }
QmlPluginManifest::UiContribution QmlPluginManifest::ui() const { return m_ui; }
QString QmlPluginManifest::description() const { return m_description; }
QString QmlPluginManifest::author() const { return m_author; }
QString QmlPluginManifest::pluginDirectory() const { return m_pluginDirectory; }
QString QmlPluginManifest::manifestFilePath() const { return m_manifestFilePath; }

QmlPluginManifest QmlPluginManifest::failure(
    Error error, const QString &message, const QString &pluginDirectory,
    const QString &manifestFilePath)
{
    QmlPluginManifest manifest;
    manifest.m_error = error;
    manifest.m_errorString = message;
    manifest.m_pluginDirectory = pluginDirectory;
    manifest.m_manifestFilePath = manifestFilePath;
    return manifest;
}
