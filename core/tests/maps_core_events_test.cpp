// Test-only bodies for MapsCoreImpl's `logos_events:` declarations. In the
// module build these are generated (maps_core_events_cdylib.cpp); here they
// forward into logos_test::EventCapture so tests can observe emissions.
#include <logos_test.h>

#include "maps_core_impl.h"

#define MAPS_EVENT(name) \
    void MapsCoreImpl::name(const std::string& json) { logos_test::recordEvent(#name, json); }

MAPS_EVENT(revisionPublished)
MAPS_EVENT(revisionApplied)
MAPS_EVENT(basemapChanged)
MAPS_EVENT(anchorMissing)
MAPS_EVENT(viewOpened)
MAPS_EVENT(viewClosed)
MAPS_EVENT(viewStateChanged)
MAPS_EVENT(cameraIntent)
MAPS_EVENT(viewOptionsChanged)
MAPS_EVENT(trackRequested)
MAPS_EVENT(snapshotRequested)
MAPS_EVENT(featureStateChanged)
MAPS_EVENT(cacheClearRequested)
MAPS_EVENT(viewEvent)
MAPS_EVENT(catalogueChanged)

#undef MAPS_EVENT
