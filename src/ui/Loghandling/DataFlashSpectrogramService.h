#ifndef DATAFLASHSPECTROGRAMSERVICE_H
#define DATAFLASHSPECTROGRAMSERVICE_H

#include "DataFlashSpectrogramAnalyzer.h"

#include <QString>

/** Streaming DataFlash parser adapter for the offline spectrogram analyzer. */
class DataFlashSpectrogramService final
{
public:
    using CancellationCheck =
        DataFlashSpectrogramAnalyzer::CancellationCheck;

    static DataFlashSpectrogramAnalyzer::AnalysisResult Generate(
        const QString &path,
        const QString &sensorName,
        int minimumDb,
        int maximumDb,
        const CancellationCheck &isCancelled = CancellationCheck());

private:
    DataFlashSpectrogramService() = delete;
};

#endif // DATAFLASHSPECTROGRAMSERVICE_H
