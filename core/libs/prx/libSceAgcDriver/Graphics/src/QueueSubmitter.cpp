#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GpuFreeTrace.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace AgcDriver::Graphics {

namespace {

struct SubmitterRegistry {
    std::mutex mutex;
    std::map<VkQueue, std::weak_ptr<QueueSubmitter>> submitters;
};

SubmitterRegistry& submitterRegistry() {
    static auto& registry = *new SubmitterRegistry;
    return registry;
}

std::atomic<std::uint64_t> threadSubmissions{0};

std::shared_ptr<QueueSubmitter> registeredSubmitter(VkQueue queue) {
    auto& registry = submitterRegistry();
    std::lock_guard lock(registry.mutex);
    const auto found = registry.submitters.find(queue);
    return found != registry.submitters.end() ? found->second.lock() : nullptr;
}

}

bool QueueSubmitter::Enabled() {
    static const bool disabled = std::getenv("APS5_NO_SUBMIT_THREAD") != nullptr;
    return !disabled;
}

std::shared_ptr<QueueSubmitter> QueueSubmitter::Acquire(VkQueue queue, PFN_vkQueueSubmit submit) {
    if (!Enabled() || queue == VK_NULL_HANDLE || submit == nullptr) return nullptr;
    auto& registry = submitterRegistry();
    std::lock_guard lock(registry.mutex);
    auto& slot = registry.submitters[queue];
    if (auto existing = slot.lock()) return existing;
    auto made = std::make_shared<QueueSubmitter>(queue, submit);
    slot = made;
    return made;
}

void QueueSubmitter::Drain(VkQueue queue) {
    if (const auto submitter = registeredSubmitter(queue)) submitter->Drain();
}

void QueueSubmitter::DrainAll() {
    std::vector<std::shared_ptr<QueueSubmitter>> live;
    {
        auto& registry = submitterRegistry();
        std::lock_guard lock(registry.mutex);
        for (const auto& [queue, submitter] : registry.submitters) {
            if (auto held = submitter.lock()) live.push_back(std::move(held));
        }
    }
    for (const auto& submitter : live) submitter->Drain();
}

std::unique_lock<std::mutex> QueueSubmitter::Access(VkQueue queue) {
    const auto submitter = registeredSubmitter(queue);
    if (submitter == nullptr) return {};
    submitter->Drain();
    return std::unique_lock(submitter->queueMutex);
}

bool QueueSubmitter::SubmitSignaling(VkQueue queue, VkCommandBuffer commands, VkFence fence, VkSemaphore signal) {
    const auto submitter = registeredSubmitter(queue);
    if (submitter == nullptr) return false;
    submitter->Enqueue(commands, fence, VK_NULL_HANDLE, 0, signal);
    return true;
}

std::uint64_t QueueSubmitter::Submitted() {
    return threadSubmissions.load(std::memory_order_relaxed);
}

QueueSubmitter::QueueSubmitter(VkQueue queue, PFN_vkQueueSubmit submit) : queue(queue), submit(submit) {
    thread = std::thread([this] { run(); });
}

QueueSubmitter::~QueueSubmitter() {
    {
        std::lock_guard lock(mutex);
        stopping = true;
    }
    work.notify_all();
    if (thread.joinable()) thread.join();
}

void QueueSubmitter::Enqueue(VkCommandBuffer commands, VkFence fence, VkSemaphore timeline, std::uint64_t value, VkSemaphore signal) {
    Require(timeline == VK_NULL_HANDLE || signal == VK_NULL_HANDLE, "a submission signals either the timeline or a binary semaphore");
    {
        std::lock_guard lock(mutex);
        Check(error, "vkQueueSubmit recorder (submit thread)");
        jobs.push_back({commands, fence, timeline, value, signal});
    }
    work.notify_one();
}

void QueueSubmitter::Drain() {
    std::unique_lock lock(mutex);
    drained.wait(lock, [&] { return jobs.empty(); });
    Check(error, "vkQueueSubmit recorder (submit thread)");
}

QueueSubmitter::Queued QueueSubmitter::Pending() {
    std::lock_guard lock(mutex);
    if (jobs.empty()) return {};
    return {jobs.size(), jobs.front().value, jobs.back().value};
}

void QueueSubmitter::run() {
    std::unique_lock lock(mutex);
    for (;;) {
        work.wait(lock, [&] { return stopping || !jobs.empty(); });
        if (jobs.empty()) return;
        const auto job = jobs.front();
        lock.unlock();
        VkSubmitInfo submission{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submission.commandBufferCount = 1;
        submission.pCommandBuffers = &job.commands;
        VkTimelineSemaphoreSubmitInfoKHR timelineInfo{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO_KHR};
        timelineInfo.signalSemaphoreValueCount = 1;
        timelineInfo.pSignalSemaphoreValues = &job.value;
        if (job.timeline != VK_NULL_HANDLE) {
            submission.pNext = &timelineInfo;
            submission.signalSemaphoreCount = 1;
            submission.pSignalSemaphores = &job.timeline;
        } else if (job.signal != VK_NULL_HANDLE) {
            submission.signalSemaphoreCount = 1;
            submission.pSignalSemaphores = &job.signal;
        }
        VkResult result = VK_SUCCESS;
        {
            std::lock_guard queueLock(queueMutex);
            result = submit(queue, 1, &submission, job.fence);
        }
        threadSubmissions.fetch_add(1, std::memory_order_relaxed);
        lock.lock();
        jobs.pop_front();
        if (result != VK_SUCCESS && error == VK_SUCCESS) {
            error = result;
            std::fprintf(stderr, "[gpu] vkQueueSubmit on the submit thread failed (%d)\n", static_cast<int>(result));
            ReportImportRetirements("vkQueueSubmit failed on the submit thread");
            ReportGpuFrees("vkQueueSubmit failed on the submit thread");
        }
        drained.notify_all();
    }
}

}
