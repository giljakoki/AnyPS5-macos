#include "prx/libSceAgcDriver/Graphics/include/UploadTrace.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Packets/FrameHitches.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Packets/HitchSampler.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/SynchronizationStatistics.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Queues/WorkerAffinity.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/WaitMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/WorkerSampler.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>

namespace AgcDriver::DriverDetail {

namespace {

// How long every queue must stay idle before queue 0 stores the pending GPU results into guest
// memory (StoreIdleResults). Storing at the first idle moment wrote every render target back after
// each batch gap of a frame (Outer Wilds: 27 write-backs, 430 MiB per present) although the next
// batch draws into them again; a target that stays idle this long is stored as before. A CPU read
// that has a sync point (a label, a pending-write overlap, a flip) still flushes at once.
// APS5_IDLE_STORE_DELAY_MS=<ms> sets it, APS5_NO_IDLE_STORE_DELAY=1 stores at the first idle moment.
std::chrono::milliseconds IdleStoreDelay() {
    static const std::chrono::milliseconds delay = [] {
        if (std::getenv("APS5_NO_IDLE_STORE_DELAY") != nullptr) return std::chrono::milliseconds(0);
        if (const char* value = std::getenv("APS5_IDLE_STORE_DELAY_MS")) return std::chrono::milliseconds(std::max(std::atoi(value), 0));
        return std::chrono::milliseconds(100);
    }();
    return delay;
}

}

FrameTiming* Driver::frameTiming() {
    if (!APS5_ENABLE_TIMING_LOG) return nullptr;
    if (pendingFrameTiming == nullptr) pendingFrameTiming = std::make_shared<FrameTiming>(++frameSerial, true);
    return pendingFrameTiming.get();
}

FrameTiming* Driver::includeTimingSubmission(const Submission& submission, bool firstSegment) {
    if (!APS5_ENABLE_TIMING_LOG) return nullptr;
    auto* frame = submission.queue == 0 ? frameTiming() : FrameTiming::Async();
    require(submission.receivedAt != FrameTiming::Clock::time_point{} && submission.receivedAt <= submission.enqueuedAt && submission.enqueuedAt <= submission.dequeuedAt && submission.dequeuedAt <= submission.orderedAt, "invalid submission timing order");
    if (submission.queue == 0) frame->IncludeSubmission(submission.serial, submission.receivedAt, submission.enqueuedAt, submission.orderedAt, firstSegment);
    else if (firstSegment) {
        frame->Add(frame->Get("Submission", "accept"), submission.enqueuedAt - submission.receivedAt);
        frame->Add(frame->Get("Submission", "queue"), submission.orderedAt - submission.enqueuedAt);
    }
    if (firstSegment) {
        frame->Add(frame->Get("Submission", "dequeue"), submission.dequeuedAt - submission.enqueuedAt);
        frame->Add(frame->Get("Submission", "order_wait"), submission.orderedAt - submission.dequeuedAt);
        if (!submission.suspend) {
            frame->Add(frame->Get("Submission", "copy"), submission.copiedAt - submission.receivedAt, submission.commands.size() * sizeof(std::uint32_t));
            frame->Add(frame->Get("Submission", "validate"), submission.validatedAt - submission.copiedAt);
            frame->Add(frame->Get("Submission", "flip_room_wait"), submission.roomReadyAt - submission.validatedAt);
            frame->Add(frame->Get("Submission", "reserve_enqueue"), submission.enqueuedAt - submission.roomReadyAt);
        }
    }
    return frame;
}

void Driver::markCompleted(std::uint64_t serial) {
    completedOutOfOrder.insert(serial);
    while (!completedOutOfOrder.empty() && *completedOutOfOrder.begin() == completed + 1) {
        completed = *completedOutOfOrder.begin();
        completedOutOfOrder.erase(completedOutOfOrder.begin());
    }
}

const std::atomic<std::uint64_t>*& Driver::workerQueued() {
    static thread_local const std::atomic<std::uint64_t>* queued = nullptr;
    return queued;
}

void Driver::reapCompletionLabels() {
    std::unique_lock gpuLock(GuestMemory::GpuMutex(), std::try_to_lock);
    if (!gpuLock.owns_lock()) {
        ++triesFailed[TryIdle];
        return;
    }

    if (!completionsPending()) return;

    bumpEpoch(&EpochBumps::reaps);
    if (const auto localDevice = device.Load()) localDevice->ReapRecorded();
}

void Driver::run(std::uint32_t id) noexcept {
    onWorkerThread() = true;
    {
        char role[32];
        std::snprintf(role, sizeof role, "queue worker 0x%x", id);
        PinWorkerThread(role);
    }

    GuestMemory::TagGpuLockThread(id);
    if (id == 0) StartWorkerSampler();
    Submission submission;
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    auto& costs = submissionCosts(id);
    try {
        for (;;) {
            submission = Submission{};
            struct Running {
                std::atomic<std::uint32_t>& count;
                std::atomic<std::uint64_t>& idleEpoch;
                std::condition_variable& changed;
                bool active = false;
                Running(std::atomic<std::uint32_t>& count, std::atomic<std::uint64_t>& idleEpoch, std::condition_variable& changed)
                    : count(count), idleEpoch(idleEpoch), changed(changed) {}
                void Start() {
                    count.fetch_add(1, std::memory_order_acq_rel);
                    active = true;
                }
                ~Running() {
                    if (active && count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                        idleEpoch.fetch_add(1, std::memory_order_release);
                        changed.notify_all();
                    }
                }
            } running{runningWorkers, idleEpoch, changed};
            static const bool traceGpu = std::getenv("APS5_TRACE_GPU") != nullptr;
            {
                PerformanceContext timingContext(id == 0 ? frameTiming() : FrameTiming::Async());
                PerformanceTimer timing("Driver.WorkerWait");
                std::unique_lock lock(mutex);
                timing.Mark("mutex_wait");
                auto& worker = workers.at(id);
                auto& pending = worker.pending;
                workerQueued() = &worker.queued;
                if (traceGpu && pending.empty()) std::fprintf(stderr, "[gpu] %.1f idle queue=0x%x\n", TraceMs(), id);
                const auto ready = [&] { return failure || stopping || !pending.empty(); };
                const auto allQueuesIdle = [&] {
                    return runningWorkers.load(std::memory_order_acquire) == 0
                        && std::all_of(workers.begin(), workers.end(), [](const auto& item) { return item.second.pending.empty(); });
                };
                const auto poll = [&](const auto& done) {
#ifdef _WIN32
                    lock.unlock();
                    PollSleep();
                    lock.lock();
                    return done();
#else
                    return changed.wait_for(lock, std::chrono::milliseconds(1), done);
#endif
                };

                while (!ready()) {
                    if (!completionsPending() || (id != 0 && Graphics::Recorder::PendingCompletionLabels() == 0)) {
                        const auto idleEpochAtCheck = idleEpoch.load(std::memory_order_acquire);
                        const bool storeIdleResults = id == 0 && allQueuesIdle() && !completionsPending();
                        if (storeIdleResults && IdleStoreDelay().count() > 0) {
                            // Wait for the queues to stay idle: new work (or a completion to poll)
                            // ends the wait without storing, the pending results stay on the GPU.
                            if (changed.wait_for(lock, IdleStoreDelay(), [&] { return ready() || !allQueuesIdle() || completionsPending(); })) {
                                if (ready()) break;
                                continue;
                            }
                        }
                        lock.unlock();
                        if (storeIdleResults) {
                            if (const auto localDevice = device.Load()) {
                                auto* hitches = FrameHitches();
                                const auto storeStart = hitches != nullptr ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                                localDevice->StoreIdleResults();
                                if (hitches != nullptr) {
                                    const auto storeEnd = std::chrono::steady_clock::now();
                                    if (auto report = hitches->IdleStoreFinished(std::chrono::duration<double, std::milli>(storeEnd - storeStart).count())) {
                                        if (auto* sampler = HitchSampler::Get()) {
                                            report->pop_back();
                                            *report += "; worker: " + sampler->Describe(SampledThread::Worker, storeStart, storeEnd) + "\n";
                                        }
                                        AgcDriver::ProfilePrint_nid_no_patch("%s", report->c_str());
                                    }
                                }
                            }
                        }
                        lock.lock();
                        if (ready()) break;
                        if (id == 0 && allQueuesIdle() && !completionsPending()
                            && (!storeIdleResults || idleEpoch.load(std::memory_order_acquire) != idleEpochAtCheck)) continue;
                        if (id == 0) queue0Dormant.store(true, std::memory_order_relaxed);
                        const auto idleEpochAtWait = idleEpoch.load(std::memory_order_acquire);
                        changed.wait(lock, [&] { return ready() || (id == 0 && idleEpoch.load(std::memory_order_acquire) != idleEpochAtWait); });
                        if (id == 0) queue0Dormant.store(false, std::memory_order_relaxed);
                        if (id == 0 && !ready() && idleEpoch.load(std::memory_order_acquire) != idleEpochAtWait) continue;
                        break;
                    }
                    if (poll(ready)) break;
                    lock.unlock();
                    reapCompletionLabels();
                    lock.lock();
                }
                timing.Mark("idle_and_completion_poll");
                rethrowFailure();
                if (stopping || shutdownToken.stop_requested() || pending.empty()) {
                    break;
                }
                submission = std::move(pending.front());
                pending.pop_front();
                if (APS5_ENABLE_TIMING_LOG) submission.dequeuedAt = std::chrono::steady_clock::now();
                if (id == 0) queue0Executing = submission.suspend ? 0 : submission.received;
                if (submission.waitFree) {
                    orderHolders.fetch_add(1, std::memory_order_acq_rel);
                    const auto released = [&] { return failure || stopping || orderReleased(id, submission.received); };
                    while (!released()) poll(released);
                    orderHolders.fetch_sub(1, std::memory_order_acq_rel);
                    rethrowFailure();
                }
                if (APS5_ENABLE_TIMING_LOG) submission.orderedAt = std::chrono::steady_clock::now();
                timing.Mark("dequeue_order");
                worker.queued.fetch_sub(1, std::memory_order_acq_rel);
                running.Start();
                if (profile && submission.enqueuedAt != std::chrono::steady_clock::time_point{}) costs.dequeueNs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - submission.enqueuedAt).count());
            }
            const auto traceExecuteStart = std::chrono::steady_clock::now();
            execute(submission);
            if (Graphics::UploadTraceEnabled()) Graphics::UploadTrace("done serial=%llu queue=0x%x after %.1f ms on the worker", static_cast<unsigned long long>(submission.serial), id, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - traceExecuteStart).count());
            if (traceGpu) std::fprintf(stderr, "[gpu] %.1f done serial=%llu queue=0x%x\n", TraceMs(), static_cast<unsigned long long>(submission.serial), id);
            const auto completeStart = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            PerformanceContext completionContext(id == 0 ? frameTiming() : FrameTiming::Async());
            PerformanceTimer completionTiming("Driver.Completion");
            bool notify = true;
            {
                std::lock_guard lock(mutex);
                rethrowFailure();
                markCompleted(submission.serial);
                forgetUnfinishedWrites(workers.at(id), submission);
                if (id == 0) queue0Executing = 0;

                notify = idleWaiters != 0 || orderHolders.load(std::memory_order_acquire) != 0;
            }
            if (notify) changed.notify_all();
            else ++costs.notifiesSkipped;
            if (profile) costs.completeNs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - completeStart).count());
        }
    } catch (const ProcessShutdown&) {
        submission = Submission{};
    } catch (...) {
        const auto error = std::current_exception();
        for (const auto& [offset, flip] : submission.flips) flip->Fail(error);
        ReportFailure(error);
    }
}

}
