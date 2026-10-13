#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_RECORDERPROGRESS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_RECORDERPROGRESS_HPP

#include <atomic>
#include <cstdint>

namespace AgcDriver::Graphics {

class Recorder;

// The active recorder's batch progress for code that also builds into test binaries without
// Recorder.cpp (the buffer pool's quarantine, the Vulkan free tracer's batch deferral): Recorder.cpp
// installs these when it is loaded, and without it there is no active recorder.
struct RecorderProgressHooks {
    const Recorder* (*active)() = nullptr;
    std::uint64_t (*newestSubmitted)(const Recorder& recorder) = nullptr;
    bool (*batchFinished)(const Recorder& recorder, std::uint64_t serial) = nullptr;
};

namespace RecorderProgressDetail {
inline std::atomic<const RecorderProgressHooks*> installed{nullptr};
}

inline void InstallRecorderProgress(const RecorderProgressHooks& hooks) {
    RecorderProgressDetail::installed.store(&hooks, std::memory_order_release);
}

// The active recorder, or null when there is none or Recorder.cpp is not linked.
inline const Recorder* ActiveRecorderProgress() {
    const auto* hooks = RecorderProgressDetail::installed.load(std::memory_order_acquire);
    return hooks != nullptr && hooks->active != nullptr ? hooks->active() : nullptr;
}

// The recorder's newest submitted batch serial (Recorder::NewestSubmitted).
inline std::uint64_t NewestSubmittedBatch(const Recorder& recorder) {
    return RecorderProgressDetail::installed.load(std::memory_order_acquire)->newestSubmitted(recorder);
}

// Whether the recorder's batch `serial` completed (Recorder::BatchFinished).
inline bool BatchFinished(const Recorder& recorder, std::uint64_t serial) {
    return RecorderProgressDetail::installed.load(std::memory_order_acquire)->batchFinished(recorder, serial);
}

}

#endif
