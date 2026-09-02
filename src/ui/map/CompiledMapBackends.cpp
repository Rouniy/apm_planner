#include "CompiledMapBackends.h"

#include "OPMapBackendWidget.h"

void RegisterCompiledMapBackends()
{
    OPMapBackendWidget::RegisterBackend();

#ifdef APM_HAS_QTLOCATION_MAP
    // QGroundControlMapWidget::RegisterBackend() is added here when the
    // optional Qt 6.11/QtLocation adapter is compiled.
#endif
}
