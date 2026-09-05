#ifndef DATAFLASHLOGANONYMIZER_H
#define DATAFLASHLOGANONYMIZER_H

#include "LogAnonymizer.h"

class DataFlashLogAnonymizer final
{
public:
    /**
     * Rewrites one seekable, already-open binary DataFlash device into an
     * already-open empty staging device. The caller alone owns publication.
     */
    static LogAnonymizeResult anonymize(
        QIODevice *input,
        QIODevice *output,
        const LogAnonymizeOptions &options,
        const LogAnonymizeCancel &cancel = {},
        const LogAnonymizeProgress &progress = {});

private:
    DataFlashLogAnonymizer() = delete;
};

#endif // DATAFLASHLOGANONYMIZER_H
