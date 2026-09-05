#ifndef APJDEFAULTSEMBEDDER_H
#define APJDEFAULTSEMBEDDER_H

#include <QString>
#include <QStringList>
#include <functional>

class ApjDefaultsEmbedder final
{
public:
    struct Options { bool overwriteExisting = false; };
    struct Result {
        bool success = false;
        bool cancelled = false;
        QString error, outputPath;
        QStringList warnings;
        qint64 imageBytes = 0, defaultsBytes = 0, maximumDefaultsBytes = 0, outputBytes = 0;
        int repairedDescriptors = 0;
    };
    using Progress = std::function<void(qint64, qint64)>;
    using Cancel = std::function<bool()>;
    static constexpr qint64 MaximumImageBytes = 64LL * 1024 * 1024;
    static constexpr qint64 MaximumJsonBytes = 96LL * 1024 * 1024;
    static constexpr qint64 MaximumParameterBytes = 1024 * 1024;

    // MP10 appends this suffix; it does not replace the original extension.
    static QString SuggestedOutputPath(const QString &firmware);
    // UTF-8/16/32 BOMs are accepted; decoded defaults must be ASCII text.
    // CRs are removed. Unknown JSON members retain their original bytes.
    // No firmware is flashed, signed, or sent to any vehicle.
    static Result Embed(const QString &firmware, const QString &parameters,
                        const Options &options, const Progress &progress = {},
                        const Cancel &cancel = {});
};

#endif
