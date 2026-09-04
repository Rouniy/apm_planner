#ifndef CONFIGFLIGHTMODESPAGEFACTORY_H
#define CONFIGFLIGHTMODESPAGEFACTORY_H

#include "ParamField.h"
#include "comm/VehicleEndpoint.h"
#include "core/parameters/ParameterMetaData.h"
#include "core/parameters/ParameterMetaDataRepository.h"

#include <QList>

class ConfigFlightModesView;
class LinkManager;
class QGCUASParamManager;
class QWidget;

struct ConfigFlightModesPageContext
{
    ParameterFirmwareFamily firmwareFamily =
        ParameterFirmwareFamily::Unknown;
    ParameterMetaDataCatalog catalog;
    QList<ConfigFriendlyParameterValue> parameters;
    VehicleTargetLease target;
    LinkManager *linkManager = nullptr;
    QGCUASParamManager *parameterManager = nullptr;
    bool parameterSnapshotComplete = false;
};

/** Creates the same pinned native Flight Modes page for CONFIG and SETUP. */
ConfigFlightModesView *CreateConfigFlightModesPage(
    const ConfigFlightModesPageContext &context,
    QWidget *parent = nullptr);

#endif // CONFIGFLIGHTMODESPAGEFACTORY_H
