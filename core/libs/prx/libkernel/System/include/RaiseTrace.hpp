#ifndef CORE_LIBS_PRX_LIBKERNEL_SYSTEM_INCLUDE_RAISETRACE_HPP
#define CORE_LIBS_PRX_LIBKERNEL_SYSTEM_INCLUDE_RAISETRACE_HPP

#include "prx/libkernel/Pthread/include/Pthread.hpp"
#include <cstdint>

namespace RaiseTrace {

constexpr std::uint64_t UnhandledGraceNanos = 1000000000ULL;

bool Enabled();
bool Unhandled(std::uint32_t sent, std::uint32_t entered, std::uint32_t reported, std::uint64_t sentNanos, std::uint64_t nowNanos);
void Start();
void Raising(Pthread thread);
void Sent(Pthread thread, const char* name, const char* how);
void Entered(Pthread self);
void Returned(Pthread self);

}

#endif
