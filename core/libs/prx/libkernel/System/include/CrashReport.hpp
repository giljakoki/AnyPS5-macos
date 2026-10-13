#ifndef CORE_LIBS_PRX_LIBKERNEL_SYSTEM_INCLUDE_CRASHREPORT_HPP
#define CORE_LIBS_PRX_LIBKERNEL_SYSTEM_INCLUDE_CRASHREPORT_HPP

namespace CrashReport {

void Install();
void PrepareThread();
bool Owns(int hostSignal);
bool Restore(int hostSignal);

}

#endif
