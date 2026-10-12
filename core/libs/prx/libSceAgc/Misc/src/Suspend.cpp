#include <cstdio>
#include <cstdlib>
#include "prx/libSceAgc/Misc/include/Suspend.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"

#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

extern "C" {

int APS5_VABI sceAgcSuspendPoint(void) {
    AgcDriverSuspendPoint_nid_postfix();
    return 0;
}

int APS5_VABI sceAgcSetSubmitMode(std::uint32_t mode) {
    if (std::getenv("APS5_TRACE_SUBMITS") != nullptr) std::fprintf(stderr, "[submit-trace] sceAgcSetSubmitMode mode=%u\n", static_cast<unsigned>(mode));
    if (mode != 0) NotImplemented_nid_no_patch(__func__);
    return 0;
}

}
