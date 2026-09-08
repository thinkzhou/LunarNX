#include "diagnostics.h"
int main() {
    lunar::startDropDiagnosticWriter();
    lunar::diagnosticLog("verbose-only", "must not appear");
    lunar::persistentEventLog("steam-health", "video_rx=%u hid_open=%d", 42u, 1);
    lunar::persistentEventLog("steam-ihs", "tag=SteamNegotiation host config confirmed");
    lunar::stopDropDiagnosticWriter();
}
