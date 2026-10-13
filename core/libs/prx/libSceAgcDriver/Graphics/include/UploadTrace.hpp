#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_UPLOADTRACE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_UPLOADTRACE_HPP

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string_view>
#include <thread>

namespace AgcDriver::Graphics {

inline std::chrono::steady_clock::time_point UploadTraceOrigin() {
    static const auto origin = std::chrono::steady_clock::now();
    return origin;
}

inline double UploadTraceMs(std::chrono::steady_clock::time_point at = std::chrono::steady_clock::now()) {
    return std::chrono::duration<double, std::milli>(at - UploadTraceOrigin()).count();
}

inline bool UploadTraceEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_TRACE_UPLOADS");
        if (value == nullptr || std::string_view(value) != "1") return false;
        static_cast<void>(UploadTraceOrigin());
        return true;
    }();
    if (!enabled) return false;
    static const double windowMs = [] {
        const char* value = std::getenv("APS5_TRACE_UPLOADS_SECONDS");
        const double seconds = value != nullptr ? std::strtod(value, nullptr) : 30.0;
        return (seconds > 0.0 ? seconds : 30.0) * 1000.0;
    }();
    return UploadTraceMs() < windowMs;
}

inline void UploadTrace(const char* format, ...) {
    char text[512];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    std::fprintf(stderr, "[upload] %.2f t%05zu %s\n", UploadTraceMs(), std::hash<std::thread::id>{}(std::this_thread::get_id()) % 100000u, text);
}

}

#endif
