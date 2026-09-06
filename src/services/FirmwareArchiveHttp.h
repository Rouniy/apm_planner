#ifndef FIRMWAREARCHIVEHTTP_H
#define FIRMWAREARCHIVEHTTP_H
#include "FirmwareArchiveTypes.h"

class FirmwareArchiveHttp final
{
public:
    // Local QNetworkAccessManager/event loop in caller worker; no GUI singleton.
    static FirmwareArchive::Fetch transport(int timeoutMs = 5 * 60 * 1000);
};
#endif
