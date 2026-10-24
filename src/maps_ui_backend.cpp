#include "maps_ui_backend.h"

// Generated umbrella for metadata.json "dependencies" (typed wrappers behind
// modules(), typed event accessors). Include it once there are dependencies.
// #include "logos_sdk.h"

QString MapsUiBackend::ping(QString message)
{
    setPingCount(pingCount() + 1);
    setStatus(QStringLiteral("pong #%1").arg(pingCount()));
    return QStringLiteral("pong: %1").arg(message);
}
