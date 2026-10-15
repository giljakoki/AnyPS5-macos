#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_HOSTTHREAD_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_HOSTTHREAD_HPP

#include <functional>
#include <memory>
#include <pthread.h>
#include <stdexcept>
#include <utility>

namespace AgcDriver::DriverDetail {

// A joinable thread with a 64 MiB stack (a std::thread gets 512 KiB on macOS).
class HostThread {
public:
    HostThread() = default;
    template <typename TBody>
    explicit HostThread(TBody&& body) {
        auto work = std::make_unique<std::function<void()>>(std::forward<TBody>(body));
        pthread_attr_t attributes;
        pthread_attr_init(&attributes);
        pthread_attr_setstacksize(&attributes, 64u << 20u);
        const int created = pthread_create(&handle, &attributes, [](void* argument) -> void* {
            std::unique_ptr<std::function<void()>> owned(static_cast<std::function<void()>*>(argument));
            (*owned)();
            return nullptr;
        }, work.get());
        pthread_attr_destroy(&attributes);
        if (created != 0) throw std::runtime_error("cannot start a driver host thread");
        work.release();
        running = true;
    }
    HostThread(const HostThread&) = delete;
    HostThread& operator=(const HostThread&) = delete;
    HostThread(HostThread&& other) noexcept : handle(other.handle), running(std::exchange(other.running, false)) {}
    HostThread& operator=(HostThread&& other) noexcept {
        if (this != &other) {
            handle = other.handle;
            running = std::exchange(other.running, false);
        }
        return *this;
    }
    [[nodiscard]] bool joinable() const { return running; }
    void join() {
        if (!running) return;
        pthread_join(handle, nullptr);
        running = false;
    }

private:
    pthread_t handle{};
    bool running = false;
};

}

#endif
