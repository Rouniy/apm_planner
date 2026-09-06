#ifndef RESXTRANSLATIONSERVICE_H
#define RESXTRANSLATIONSERVICE_H

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

struct TranslationCulture {
    QString name, displayName;
    QString label() const { return displayName + QStringLiteral(" (") + name + QLatin1Char(')'); }
};

struct TranslationIdentity {
    QString relativePath, key;
};
bool operator==(const TranslationIdentity &, const TranslationIdentity &) noexcept;
uint qHash(const TranslationIdentity &, uint seed = 0) noexcept;

struct ResxTranslationEntry {
    QString relativePath, key, sourceText, translation, comment;
    bool hasExistingTranslation = false;
};

struct ResxTranslationProject {
    QString sourceRoot, culture;
    QVector<ResxTranslationEntry> entries;
    int resourceFiles = 0;
    QStringList warnings;
};

// Synchronous QtCore worker API. Callbacks are trusted observers and execute
// on the caller's thread; they must not concurrently modify filesystem state.
// No vehicle traffic, GUI objects, locale mutation or network access.
class ResxTranslationService final
{
public:
    using Cancel = std::function<bool()>;
    using Progress = std::function<void(qint64, qint64)>;
    struct LoadResult {
        bool success = false, cancelled = false;
        QString error;
        ResxTranslationProject project;
    };
    struct ExportResult {
        bool success = false, cancelled = false;
        QString error, outputRoot;
        int resourceFiles = 0, translatedEntries = 0, overwrittenFiles = 0;
        QString backupDirectory, resumeHtmlPath;
        // Actual completed publications/backups, retained even on failure.
        QStringList publishedPaths, backupPaths, warnings;
    };
    struct ImportResult {
        bool success = false, cancelled = false;
        QString error;
        QHash<TranslationIdentity, QString> values;
        QVector<TranslationIdentity> order;
    };
    struct TextResult {
        bool success = false, cancelled = false;
        QString error, text;
    };

    static constexpr int MaximumFiles = 20000;
    static constexpr int MaximumDirectories = 20000;
    static constexpr int MaximumDepth = 64;
    static constexpr int MaximumEntries = 200000;
    static constexpr qint64 MaximumResourceBytes = 16 * 1024 * 1024;
    static constexpr qint64 MaximumResumeBytes = 100 * 1024 * 1024;
    static constexpr qint64 MaximumTextBytes = 100 * 1024 * 1024;
    static constexpr qint64 MaximumAggregateBytes = 256 * 1024 * 1024;

    static QVector<TranslationCulture> cultures(QString *error = nullptr);
    static LoadResult load(const QString &sourceRoot, const QString &culture,
                           Cancel cancel = {}, Progress progress = {});
    // `export` is a C++ keyword; this is MP10 Export with explicit receipts.
    static ExportResult exportTranslations(const QString &outputRoot, const QString &culture,
        const QVector<ResxTranslationEntry> &entries, Cancel cancel = {}, Progress progress = {});
    static ImportResult importResumeHtml(const QString &path, Cancel cancel = {}, Progress progress = {});
    static TextResult buildCsv(const QVector<ResxTranslationEntry> &entries,
                              Cancel cancel = {}, Progress progress = {});
    static bool resumeFileMatches(const QString &importedFile, const QString &sourceRelativePath);
    static QString localizedRelativePath(const QString &relativePath, const QString &culture,
                                        QString *error = nullptr);
    static bool isTranslatable(const QString &fileName, const QString &key);
};

#endif
