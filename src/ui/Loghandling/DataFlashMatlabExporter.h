#ifndef DATAFLASHMATLABEXPORTER_H
#define DATAFLASHMATLABEXPORTER_H

#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

/**
 * Bounded, worker-safe Mission Planner DataFlash BIN/LOG MATLAB exporter.
 *
 * Prepare is read-only and freezes the source identity, schema, instance
 * routing, exact derived output name and Level-5 layout for consent. Export
 * accepts only that immutable plan, rechecks the source, stages beside the
 * destination and never replaces an existing file.
 */
class DataFlashMatlabExporter final
{
public:
    using CancelCheck = std::function<bool()>;
    using Progress = std::function<void(qint64 completedWork,
                                        qint64 totalWork)>;

    static constexpr qint64 MaximumInputBytes = 1024LL * 1024LL * 1024LL;
    static constexpr qint64 MaximumRecords = 20'000'000;
    static constexpr int MaximumVariables = 4096;
    static constexpr int MaximumParameters = 10'000;
    static constexpr qint64 MaximumCellRecords = 10'000'000;

    class Plan final
    {
    public:
        Plan();
        Plan(const Plan &);
        Plan &operator=(const Plan &);
        Plan(Plan &&) noexcept;
        Plan &operator=(Plan &&) noexcept;
        ~Plan();

        bool isValid() const noexcept;
        QString sourcePath() const;
        QString outputPath() const;
        qint64 recordCount() const noexcept;
        qint64 estimatedBytes() const noexcept;
        int variableCount() const noexcept;
        QStringList warnings() const;

    private:
        struct Data;
        std::shared_ptr<const Data> d;
        explicit Plan(std::shared_ptr<const Data> data);
        friend class DataFlashMatlabExporter;
    };

    struct PlanResult {
        bool success = false;
        bool cancelled = false;
        QString error;
        QStringList warnings;
        std::shared_ptr<const Plan> plan;
    };

    struct Result {
        bool success = false;
        bool cancelled = false;
        QString error;
        QString outputPath;
        QStringList warnings;
        qint64 recordCount = 0;
        int variableCount = 0;
        qint64 bytesWritten = 0;
    };

    static PlanResult Prepare(const QString &inputPath,
                              const CancelCheck &cancel = {},
                              const Progress &progress = {});
    static Result Export(const Plan &plan,
                         const CancelCheck &cancel = {},
                         const Progress &progress = {});

private:
    DataFlashMatlabExporter() = delete;
};

#endif // DATAFLASHMATLABEXPORTER_H
