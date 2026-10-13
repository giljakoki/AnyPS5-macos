#ifndef CORE_LIBS_PRX_LIBKERNEL_SYSTEM_INCLUDE_RAISETRACE_HPP
#define CORE_LIBS_PRX_LIBKERNEL_SYSTEM_INCLUDE_RAISETRACE_HPP

#include "prx/libkernel/Pthread/include/Pthread.hpp"

namespace RaiseTrace {

bool Enabled();
void Start();
void Sent(Pthread thread, const char* name, bool sent);
void Entered(Pthread self);
void Returned(Pthread self);

}

#endif
