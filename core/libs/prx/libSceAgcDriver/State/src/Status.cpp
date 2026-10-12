#include <cstdio>
#include <cstdlib>
#include "prx/libSceAgcDriver/State/include/Status.hpp"

#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

extern "C" {

bool APS5_VABI sceAgcDriverIsCaptureInProgress(void) {
    return false;
}

bool APS5_VABI sceAgcDriverIsTraceInProgress(void) {
    return false;
}

bool APS5_VABI sceAgcDriverIsSubmitValidationEnabled(void) {
    if (std::getenv("APS5_TRACE_SUBMITS") != nullptr) std::fprintf(stderr, "[submit-trace] %s\n", __func__);
    return false;
}

int APS5_VABI sceAgcDriverRequestCaptureStart(const char* path) {
    (void)path;
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceAgcDriverRequestCaptureStop() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceAgcDriverTriggerCapture() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

}
