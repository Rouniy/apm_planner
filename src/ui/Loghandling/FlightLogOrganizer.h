#ifndef FLIGHTLOGORGANIZER_H
#define FLIGHTLOGORGANIZER_H

#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>
#include <memory>

class FlightLogOrganizer final
{
    struct PlanData;
public:
    using Cancel = std::function<bool()>;
    using Progress = std::function<void(qint64, qint64)>;
    enum class Operation { Move, DeleteEmpty };
    struct Entry {
        Operation operation = Operation::Move;
        QString source, destination;
        qint64 bytes = 0;
    };
    class Plan {
    public:
        Plan() = default;
        bool isValid() const;
        QString root() const;
        int candidateCount() const;
        const QVector<Entry> &entries() const;
        const QStringList &warnings() const;
    private:
        friend class FlightLogOrganizer;
        std::shared_ptr<const PlanData> d;
    };
    struct Analysis {
        bool success = false, cancelled = false;
        QString error;
        Plan plan;
    };
    struct Result {
        bool success = false, cancelled = false;
        QString error;
        QVector<Entry> completed;
        QStringList warnings;
        int remaining = 0;
    };
    static constexpr int MaximumFiles = 20000;
    // Analysis is read-only. Execute accepts only an analyzed immutable plan;
    // the UI must list its exact entries and obtain explicit confirmation.
    static Analysis Analyze(const QString &root, const Cancel &cancel = {},
                            const Progress &progress = {});
    static Result Execute(const Plan &plan, const Cancel &cancel = {},
                          const Progress &progress = {});
};

#endif
