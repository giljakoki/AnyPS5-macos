#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_GPUFREETRACE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_GPUFREETRACE_HPP

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

namespace AgcDriver::Graphics {

PFN_vkGetDeviceProcAddr GuardDeviceProc(PFN_vkGetDeviceProcAddr real);
void ReportGpuFrees(const char* why);
// Whether frees of memory and query pools wait for every earlier queue submission (APS5_FREE_GATE).
bool FreeGateEnabled();

}

#endif
