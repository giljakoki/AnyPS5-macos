#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_STALEHANDLETRACE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_STALEHANDLETRACE_HPP

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#include <cstdint>

namespace AgcDriver::Graphics {

enum class StaleObject : std::uint8_t { Memory, Buffer, Image, ImageView, BufferView };

bool StaleHandlesTraced();
PFN_vkVoidFunction StaleHandleWrapper(const char* name, PFN_vkVoidFunction real);
void StaleHandleRelease(StaleObject kind, std::uint64_t handle);
std::uint64_t StaleHandleReports();
void StaleHandleDump(const char* why);
double StaleAddressFreedMs(std::uint64_t address);
double StaleMemoryFreedMs(std::uint64_t memory);

}

#endif
