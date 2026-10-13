#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DIAGNOSTICS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DIAGNOSTICS_HPP

#include "prx/libSceAgcDriver/Execution/include/QueueState.hpp"
#include <cstdint>
#include <cstddef>
#include <span>

namespace AgcDriver::DriverDetail {

double TraceMs();

void require(bool condition, const char* reason);

std::uint32_t readRegister(const Registers& registers, std::uint32_t offset);

std::uint32_t readUserData(const Registers& shader, std::uint32_t offset);

bool SkipFailedWork();

void NoteSkippedWork(const char* reason, const char* kind);

bool RecentWorkTraced();

void NoteRecentWork(const char* kind, std::uint32_t queue, std::uint64_t serial, std::size_t offset, const QueueState& state, std::span<const std::uint32_t> packet);

void DumpRecentWork(const char* why);

void InstallRecentWorkDump();

}

#endif
