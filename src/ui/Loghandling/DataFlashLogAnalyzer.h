#ifndef DATAFLASHLOGANALYZER_H
#define DATAFLASHLOGANALYZER_H

#include <QPair>
#include <QStringList>
#include <QVector>
#include <functional>

/** Offline MP10 LogAnalyzer checks. No vehicle writes or diagnostic authority. */
class DataFlashLogAnalyzer
{
public:
    using Cancel = std::function<bool()>;
    using Progress = std::function<void(qint64, qint64)>;
    struct TestResult { QString name, status, message; };
    struct Result {
        bool success = false;
        bool cancelled = false;
        QString error;
        QStringList warnings;
        QVector<TestResult> tests;
    };
    enum class VehicleType { Unknown, Copter, Plane, Rover };
    using Values = QVector<QPair<QString, double>>;
    using TextValues = QVector<QPair<QString, QString>>;
    struct Sample {
        qint64 line = 0;
        double timeSeconds = 0;
        Values values;
        TextValues textValues;
    };
    struct RecordGroup { QString type; QVector<Sample> samples; };
    // Ordered containers preserve MP10 dictionary insertion order and the
    // first invalid-value attribution. Keys are matched case-insensitively.
    struct Data {
        qint64 lineCount = 0;
        VehicleType vehicleType = VehicleType::Unknown;
        QVector<RecordGroup> records;
        Values parameters;
    };
    static constexpr qint64 MaximumInputBytes = 1024LL * 1024 * 1024;
    static constexpr int MaximumSamples = 1000000;
    static constexpr int MaximumNumericValues = 4000000;
    static constexpr qint64 MaximumTextBytes = 32LL * 1024 * 1024;
    static constexpr int MaximumParameters = 100000;

    static Result Analyze(const QString &path, Cancel cancel = {}, Progress progress = {});
    static Result Analyze(const Data &data, Cancel cancel = {}, Progress progress = {});
    static QString Format(const QVector<TestResult> &tests);
    static QString Classify(double value, double warn, double fail, bool higherWorse);
};

#endif // DATAFLASHLOGANALYZER_H
