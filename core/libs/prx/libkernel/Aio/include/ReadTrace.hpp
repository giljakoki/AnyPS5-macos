#ifndef CORE_LIBS_PRX_LIBKERNEL_AIO_INCLUDE_READTRACE_HPP
#define CORE_LIBS_PRX_LIBKERNEL_AIO_INCLUDE_READTRACE_HPP

#include <cstdint>

namespace ReadTrace {

enum class Kind { Aio, Pread, Read, AioWait };

bool Enabled();
void Record(Kind kind, int descriptor, std::uint64_t bytes, std::uint64_t nanos);

}

#endif
