#include "SceTypes.hpp"
#include "prx/libSceVideoOut/include/Buffer.hpp"
#include "prx/libSceVideoOut/include/Output.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include <array>
#include "tests/VideoOutTestEnvironment.hpp"
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#undef main

extern "C" {
int APS5_VABI scePthreadCreate(Pthread* thread, const PthreadAttr* attr, PthreadEntry entry, void* arg, const char* name);
void APS5_VABI exit_nid_postfix(int code);
}

namespace {

constexpr int Flippers = 2;
constexpr int Pollers = 2;
constexpr int Submitters = 2;

std::atomic<int> running{0};
int handle = 0;
VideoOutTestEnvironment* environment = nullptr;

void Require(bool value, const char* message) {
    if (value) return;
    std::fprintf(stderr, "video out exit while presenting: %s\n", message);
    std::fflush(stderr);
    std::_Exit(1);
}

void* APS5_VABI Flipper(void*) {
    running.fetch_add(1);
    for (std::int64_t argument = 0;; ++argument) {
        sceVideoOutSubmitFlip(handle, 0, 1, argument);
        sceVideoOutWaitVblank(handle);
    }
}

void* APS5_VABI Poller(void*) {
    running.fetch_add(1);
    for (;;) {
        VideoOutFlipStatus status{};
        sceVideoOutGetFlipStatus(handle, &status);
    }
}

void* APS5_VABI Submitter(void*) {
    running.fetch_add(1);
    for (std::uint32_t value = 0;; ++value) {
        std::array<std::uint32_t, 5> words{0xc0017600, 0x240, value, 0xc0001000, 0};
        Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
        sceAgcDriverSubmitDcb(&packet);
    }
}

void RemoveEnvironment() {
    delete environment;
}

[[noreturn]] void Run(const std::string& mode) {
    try {
        environment = new VideoOutTestEnvironment();
        std::atexit(RemoveEnvironment);
        handle = sceVideoOutOpen(255, 0, 0, nullptr);
    } catch (const std::exception& error) {
        if (std::string(error.what()).find("Vulkan support") != std::string::npos && !std::getenv("ANYPS5_REQUIRE_DISPLAY")) {
            std::printf("skipped, no display or Vulkan device: %s\n", error.what());
            std::fflush(stdout);
            std::_Exit(77);
        }
        std::fprintf(stderr, "video out exit while presenting: %s\n", error.what());
        std::_Exit(1);
    }
    Require(handle > 0, "the output must open");
    static std::vector<std::byte> allocation(65536 + 65535);
    const auto address = reinterpret_cast<std::uintptr_t>(allocation.data());
    VideoOutBuffers buffer{allocation.data() + ((65536u - (address & 65535u)) & 65535u), nullptr, {nullptr, nullptr}};
    VideoOutBufferAttribute2 attribute{};
    attribute.width = 64;
    attribute.height = 64;
    attribute.pixel_format = 0x8000000000000000ull;
    Require(sceVideoOutRegisterBuffers2(handle, 0, 0, &buffer, 1, &attribute, 0, nullptr) == 0, "buffer registration failed");
    Pthread thread = nullptr;
    for (int index = 0; index < Flippers; ++index) Require(scePthreadCreate(&thread, nullptr, Flipper, nullptr, "flipper") == 0, "creating a flipper failed");
    for (int index = 0; index < Pollers; ++index) Require(scePthreadCreate(&thread, nullptr, Poller, nullptr, "poller") == 0, "creating a poller failed");
    for (int index = 0; index < Submitters; ++index) Require(scePthreadCreate(&thread, nullptr, Submitter, nullptr, "submitter") == 0, "creating a submitter failed");
    while (running.load() != Flippers + Pollers + Submitters) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    std::printf("video out exit while presenting (%s): exiting\n", mode.c_str());
    std::fflush(stdout);
    if (mode == "sigterm") {
        kill(getpid(), SIGTERM);
        for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    exit_nid_postfix(0);
    std::_Exit(1);
}

}

#ifdef __APPLE__
extern "C" [[noreturn]] void Aps5StartGuest_nid_no_patch(void (*entry)(void*, void*), void* block);
#endif

int main(int argc, char** argv) {
    static std::string mode;
    mode = argc > 1 ? argv[1] : "exit";
#ifdef __APPLE__
    Aps5StartGuest_nid_no_patch([](void*, void*) { Run(mode); }, nullptr);
#else
    Run(mode);
#endif
}
