#ifndef FLIGHTLOGCLASSIFIER_H
#define FLIGHTLOGCLASSIFIER_H

#include <QString>
#include <QStringList>
#include <functional>

class FlightLogClassifier final
{
public:
    using Cancel = std::function<bool()>;
    using Progress = std::function<void(qint64, qint64)>;
    enum class Disposition { Leave, Move, DeleteEmpty };
    struct Result {
        bool success = false, cancelled = false;
        Disposition disposition = Disposition::Leave;
        QString relativeDirectory, error;
        QStringList warnings;
    };
    // Read-only classification. The caller owns snapshots, consent and moves.
    static Result Classify(const QString &path, const Cancel &cancel = {},
                           const Progress &progress = {});
};

#endif
