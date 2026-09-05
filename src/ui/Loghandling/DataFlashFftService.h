#ifndef DATAFLASHFFTSERVICE_H
#define DATAFLASHFFTSERVICE_H

#include "DataFlashFftAnalyzer.h"

#include <QString>

/** Streaming DataFlash parser adapter for the MP10 FFT analysis. */
class DataFlashFftService final
{
public:
    static DataFlashFftAnalyzer::Result Analyze(
        const QString &path,
        const DataFlashFftAnalyzer::Options &options =
            DataFlashFftAnalyzer::Options());

private:
    DataFlashFftService() = delete;
};

#endif // DATAFLASHFFTSERVICE_H
