#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_GPUFREETRACE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_GPUFREETRACE_HPP

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#include <cstdint>

namespace AgcDriver::Graphics {

PFN_vkGetDeviceProcAddr GuardDeviceProc(PFN_vkGetDeviceProcAddr real);
void ReportGpuFrees(const char* why);

// A failed submit prints the import retirement record through this, installed by
// GuestBufferMemory.cpp when it is loaded, so that test binaries built without it still link.
void InstallImportRetirementReport(void (*report)(const char* why));
// The import retirement record (ReportImportRetirements) when GuestBufferMemory.cpp is linked.
void ReportImportRetirementsIfLinked(const char* why);

// Whether frees of memory and query pools wait for every earlier queue submission (APS5_FREE_GATE).
bool FreeGateEnabled();

// MoltenVK walks every live buffer and image (under a device lock) for each global memory barrier
// whose destination is a host read, to synchronize managed-storage resources; on an Apple GPU, with
// every host-visible memory type coherent, no resource needs it and the walk does nothing else. The
// device sets this when that holds; the vkCmdPipelineBarrier wrapper then leaves
// VK_ACCESS_HOST_READ_BIT out of such barriers' destination access (the host stage, buffer and image
// barriers stay). APS5_KEEP_HOST_BARRIERS=1 keeps them.
void SetHostReadBarriersInert(bool inert);
bool HostReadBarriersInert();
// Barrier calls whose host read was left out since the last call.
std::uint64_t TakeHostReadBarriersStripped();

}

#endif
