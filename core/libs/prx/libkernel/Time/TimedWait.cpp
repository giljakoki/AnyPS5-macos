#include "prx/libkernel/Time/include/TimedWait.hpp"
#include "prx/libkernel/Pthread/include/WindowsThreadLocal.hpp"

#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <time.h>
#endif

namespace TimedWait {

namespace {

constexpr std::uint64_t YIELD_BELOW_NANOS = 50000ULL;
constexpr std::uint64_t LEAD_NANOS = 500000ULL;

struct ThreadWaitState {
    std::atomic<int> own{0};
    std::atomic<int>* bound = &own;
};
ThreadWaitState& WaitState() {
#ifdef _WIN32
    return WindowsThreadLocal<ThreadWaitState>::Get();
#else
    static thread_local ThreadWaitState state;
    return state;
#endif
}

#ifdef _WIN32

using NtTestAlertFunction = LONG(NTAPI*)();

void DrainApcs() {
    static const auto testAlert = reinterpret_cast<NtTestAlertFunction>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtTestAlert")));
    if (testAlert) testAlert();
    else SleepEx(0, TRUE);
}

DWORD AlertableWait(DWORD count, const HANDLE* handles, DWORD milliseconds) {
    WaitState().bound->fetch_add(1, std::memory_order_seq_cst);
    DWORD result;
    do {
        result = WaitForMultipleObjectsEx(count, handles, FALSE, milliseconds, TRUE);
    } while (result == WAIT_IO_COMPLETION);
    WaitState().bound->fetch_sub(1, std::memory_order_seq_cst);
    DrainApcs();
    return result;
}

void AlertableSleep(DWORD milliseconds) {
    WaitState().bound->fetch_add(1, std::memory_order_seq_cst);
    SleepEx(milliseconds, TRUE);
    WaitState().bound->fetch_sub(1, std::memory_order_seq_cst);
    DrainApcs();
}

HANDLE ThreadTimer(unsigned kind);

HANDLE CreateHighResolutionTimer() {
    return CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
}

bool TimerWait(HANDLE timer, std::uint64_t nanos) {
    thread_local int depth = 0;
    struct Nesting {
        HANDLE own;
        explicit Nesting(bool nested) : own(nested ? CreateHighResolutionTimer() : nullptr) { ++depth; }
        ~Nesting() {
            --depth;
            if (own) CloseHandle(own);
        }
    } nesting(depth != 0);
    if (depth > 1) timer = nesting.own;
    if (!timer) return false;
    LARGE_INTEGER due{};
    due.QuadPart = -static_cast<LONGLONG>(nanos / 100ULL);
    if (due.QuadPart == 0) due.QuadPart = -1;
    if (!SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) return false;
    return AlertableWait(1, &timer, INFINITE) == WAIT_OBJECT_0;
}

DWORD WholeMilliseconds(std::uint64_t nanos) {
    const std::uint64_t millis = nanos / 1000000ULL;
    return millis == 0 ? 1u : static_cast<DWORD>(millis > 0xFFFFFFFEULL ? 0xFFFFFFFEULL : millis);
}

void SleepNanosCoarse(std::uint64_t nanos) {
    if (nanos < YIELD_BELOW_NANOS) {
        SwitchToThread();
        return;
    }
    const HANDLE timer = ThreadTimer(0);
    if (TimerWait(timer, nanos)) return;
    AlertableSleep(static_cast<DWORD>((nanos + 999999ULL) / 1000000ULL));
}

#endif

}  // namespace

#ifdef _WIN32

struct Waiter {
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE timer = CreateHighResolutionTimer();
    HANDLE sleepTimers[3]{};
    Waiter* previous = nullptr;
    Waiter* next = nullptr;
    Waiter* nested = nullptr;
    bool queued = false;
    bool busy = false;

    ~Waiter() {
        delete nested;
        if (event) CloseHandle(event);
        if (timer) CloseHandle(timer);
        for (const auto sleepTimer : sleepTimers) if (sleepTimer) CloseHandle(sleepTimer);
    }
};

namespace {

Waiter* ThisThreadWaiter() {
    return &WindowsThreadLocal<Waiter>::Get();
}

HANDLE ThreadTimer(unsigned kind) {
    auto& timer = ThisThreadWaiter()->sleepTimers[kind];
    if (!timer) timer = CreateHighResolutionTimer();
    return timer;
}

Waiter* AcquireWaiter() {
    Waiter* waiter = ThisThreadWaiter();
    while (waiter->busy) {
        if (!waiter->nested) waiter->nested = new Waiter();
        waiter = waiter->nested;
    }
    waiter->busy = true;
    return waiter;
}

bool EventSet(HANDLE event, DWORD milliseconds) {
    return AlertableWait(1, &event, milliseconds) == WAIT_OBJECT_0;
}

bool WaitEventUntil(Waiter* waiter, std::uint64_t deadlineNanos) {
    for (;;) {
        const std::uint64_t now = NowNanos();
        if (now >= deadlineNanos) return EventSet(waiter->event, 0);
        const std::uint64_t remaining = deadlineNanos - now;
        if (remaining > LEAD_NANOS) {
            const std::uint64_t bulk = remaining - LEAD_NANOS;
            LARGE_INTEGER due{};
            due.QuadPart = -static_cast<LONGLONG>(bulk / 100ULL);
            if (waiter->timer && SetWaitableTimer(waiter->timer, &due, 0, nullptr, nullptr, FALSE)) {
                HANDLE handles[2] = {waiter->event, waiter->timer};
                if (AlertableWait(2, handles, INFINITE) == WAIT_OBJECT_0) return true;
                continue;
            }
            if (EventSet(waiter->event, WholeMilliseconds(bulk))) return true;
            continue;
        }
        if (EventSet(waiter->event, 0)) return true;
        YieldProcessor();
    }
}

}  // namespace

Waiter* Condition::enqueue() {
    Waiter* waiter = AcquireWaiter();
    std::lock_guard lock(queueLock);
    waiter->queued = true;
    waiter->next = nullptr;
    waiter->previous = tail;
    if (tail) tail->next = waiter;
    else head = waiter;
    tail = waiter;
    return waiter;
}

void Condition::unlink(Waiter* waiter) {
    if (waiter->previous) waiter->previous->next = waiter->next;
    else head = waiter->next;
    if (waiter->next) waiter->next->previous = waiter->previous;
    else tail = waiter->previous;
    waiter->previous = nullptr;
    waiter->next = nullptr;
    waiter->queued = false;
}

void Condition::waitSignal(Waiter* waiter) {
    AlertableWait(1, &waiter->event, INFINITE);
    waiter->busy = false;
}

bool Condition::waitSignalUntil(Waiter* waiter, std::uint64_t deadlineNanos) {
    bool signaled = WaitEventUntil(waiter, deadlineNanos);
    if (!signaled) {
        std::lock_guard lock(queueLock);
        if (waiter->queued) {
            unlink(waiter);
        } else {
            WaitForSingleObject(waiter->event, 0);
            signaled = true;
        }
    }
    waiter->busy = false;
    return signaled;
}

void Condition::NotifyOne() {
    if (Coarse()) {
        coarse.notify_one();
        return;
    }
    std::lock_guard lock(queueLock);
    Waiter* waiter = head;
    if (!waiter) return;
    unlink(waiter);
    SetEvent(waiter->event);
}

void Condition::NotifyAll() {
    if (Coarse()) {
        coarse.notify_all();
        return;
    }
    std::lock_guard lock(queueLock);
    while (Waiter* waiter = head) {
        unlink(waiter);
        SetEvent(waiter->event);
    }
}

#else

void Condition::NotifyOne() {
    RaiseCritical critical(CurrentRaiseState());
    const auto guard = internal;
    std::lock_guard inner(*guard);
    generation.fetch_add(1, std::memory_order_seq_cst);
    signal.notify_one();
}

void Condition::NotifyAll() {
    RaiseCritical critical(CurrentRaiseState());
    const auto guard = internal;
    std::lock_guard inner(*guard);
    generation.fetch_add(1, std::memory_order_seq_cst);
    signal.notify_all();
}

#endif

namespace {

std::atomic<void (*)()> raiseDelivery{nullptr};

struct BoundRaise {
    RaiseState* state = nullptr;
};

RaiseState*& BoundRaiseState() {
#ifdef _WIN32
    return WindowsThreadLocal<BoundRaise>::Get().state;
#else
    static thread_local RaiseState* state = nullptr;
    return state;
#endif
}

}

void BindThreadRaiseState(RaiseState* state) {
    BoundRaiseState() = state;
}

void SetRaiseDelivery(void (*deliver)()) {
    raiseDelivery.store(deliver, std::memory_order_release);
}

RaiseState* CurrentRaiseState() {
#ifdef __APPLE__
    return BoundRaiseState();
#else
    return nullptr;
#endif
}

bool EnterWait(RaiseState* state, Condition* condition) {
    if (state == nullptr) return false;
    std::lock_guard lock(state->lock);
    state->pending.fetch_add(state->signalsInFlight.exchange(0, std::memory_order_seq_cst), std::memory_order_seq_cst);
    if (state->delivering.load(std::memory_order_seq_cst)) return false;
    state->waitingOn = condition;
    return true;
}

void LeaveWait(RaiseState* state) {
    if (state == nullptr) return;
    std::lock_guard lock(state->lock);
    state->waitingOn = nullptr;
}

void DrainRaises(RaiseState* state) {
    for (;;) {
        state->waitDepth.fetch_add(1, std::memory_order_seq_cst);
        bool claimed = false;
        {
            std::lock_guard lock(state->lock);
            if (!state->delivering.load(std::memory_order_seq_cst) && state->pending.load(std::memory_order_seq_cst) != 0) {
                state->pending.fetch_sub(1, std::memory_order_seq_cst);
                state->delivering.store(true, std::memory_order_seq_cst);
                claimed = true;
            }
        }
        state->waitDepth.fetch_sub(1, std::memory_order_seq_cst);
        if (!claimed) {
            if (state->delivering.load(std::memory_order_seq_cst) || state->pending.load(std::memory_order_seq_cst) == 0) return;
            continue;
        }
        const auto deliver = raiseDelivery.load(std::memory_order_acquire);
        if (deliver == nullptr) throw std::runtime_error("A raised exception is pending on a waiting thread without a delivery function");
        deliver();
        state->waitDepth.fetch_add(1, std::memory_order_seq_cst);
        {
            std::lock_guard lock(state->lock);
            state->delivering.store(false, std::memory_order_seq_cst);
        }
        state->waitDepth.fetch_sub(1, std::memory_order_seq_cst);
    }
}

RaiseSignal ClaimRaiseSignal() {
#ifdef __APPLE__
    RaiseState* state = BoundRaiseState();
    if (state == nullptr) return RaiseSignal::Run;
    int inFlight = state->signalsInFlight.load(std::memory_order_seq_cst);
    do {
        if (inFlight == 0) return RaiseSignal::Stale;
    } while (!state->signalsInFlight.compare_exchange_weak(inFlight, inFlight - 1, std::memory_order_seq_cst));
    if (state->waitDepth.load(std::memory_order_seq_cst) == 0 && !state->delivering.load(std::memory_order_seq_cst)) return RaiseSignal::Run;
    state->pending.fetch_add(1, std::memory_order_seq_cst);
    return RaiseSignal::Deferred;
#else
    return RaiseSignal::Run;
#endif
}

RaiseRoute RequestRaise(RaiseState& target) {
#ifdef __APPLE__
    constexpr std::uint64_t RetryNanos = 100000000ULL;
    const std::uint64_t deadline = NowNanos() + RetryNanos;
    for (;;) {
        {
            std::lock_guard lock(target.lock);
            if (target.delivering.load(std::memory_order_seq_cst)) {
                target.pending.fetch_add(1, std::memory_order_seq_cst);
                return RaiseRoute::Queued;
            }
            Condition* condition = target.waitingOn;
            if (condition == nullptr) {
                target.signalsInFlight.fetch_add(1, std::memory_order_seq_cst);
                return RaiseRoute::Signal;
            }
            const auto guard = condition->internal;
            if (guard->try_lock()) {
                std::lock_guard inner(*guard, std::adopt_lock);
                target.pending.fetch_add(1, std::memory_order_seq_cst);
                condition->signal.notify_all();
                return RaiseRoute::Queued;
            }
            if (NowNanos() >= deadline) {
                target.signalsInFlight.fetch_add(1, std::memory_order_seq_cst);
                return RaiseRoute::Signal;
            }
        }
        std::this_thread::yield();
    }
#else
    (void)target;
    return RaiseRoute::Signal;
#endif
}

void BindThreadWaitState(std::atomic<int>* state) {
    auto& local = WaitState();
    local.bound = state != nullptr ? state : &local.own;
}

bool Coarse() {
#ifdef _WIN32
    static const bool coarse = std::getenv("APS5_COARSE_TIMED_WAITS") != nullptr;
    return coarse;
#else
    return true;
#endif
}

std::uint64_t NowNanos() {
#ifdef _WIN32
    static const std::uint64_t frequency = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return static_cast<std::uint64_t>(f.QuadPart);
    }();
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    const std::uint64_t ticks = static_cast<std::uint64_t>(counter.QuadPart);
    return (ticks / frequency) * 1000000000ULL + (ticks % frequency) * 1000000000ULL / frequency;
#else
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<std::uint64_t>(ts.tv_nsec);
#endif
}

std::uint64_t DeadlineNanos(std::uint64_t microseconds) {
    const std::uint64_t now = NowNanos();
    if (microseconds > (std::numeric_limits<std::uint64_t>::max() - now) / 1000ULL) return std::numeric_limits<std::uint64_t>::max();
    return now + microseconds * 1000ULL;
}

std::uint64_t RemainingMicros(std::uint64_t deadlineNanos) {
    const std::uint64_t now = NowNanos();
    return deadlineNanos > now ? (deadlineNanos - now) / 1000ULL : 0;
}

void SleepUntil(std::uint64_t deadlineNanos) {
#ifdef _WIN32
    std::uint64_t now = NowNanos();
    if (deadlineNanos > now + LEAD_NANOS) {
        const HANDLE timer = ThreadTimer(1);
        if (!TimerWait(timer, deadlineNanos - now - LEAD_NANOS)) AlertableSleep(WholeMilliseconds(deadlineNanos - now - LEAD_NANOS));
        now = NowNanos();
    }
    while (now < deadlineNanos) {
        YieldProcessor();
        now = NowNanos();
    }
#else
    const std::uint64_t now = NowNanos();
    if (deadlineNanos > now) SleepNanos(deadlineNanos - now);
#endif
}

void PollSleepUntil(std::uint64_t deadlineNanos) {
#ifdef _WIN32
    const std::uint64_t now = NowNanos();
    if (deadlineNanos <= now) return;
    const HANDLE timer = ThreadTimer(2);
    if (!TimerWait(timer, deadlineNanos - now)) AlertableSleep(WholeMilliseconds(deadlineNanos - now));
#else
    SleepUntil(deadlineNanos);
#endif
}

void SleepNanos(std::uint64_t nanos) {
    if (nanos == 0) return;
#ifdef _WIN32
    if (Coarse()) {
        SleepNanosCoarse(nanos);
        return;
    }
    if (nanos < YIELD_BELOW_NANOS) {
        SwitchToThread();
        return;
    }
    SleepUntil(NowNanos() + nanos);
#else
    struct timespec req{};
    req.tv_sec = static_cast<time_t>(nanos / 1000000000ULL);
    req.tv_nsec = static_cast<long>(nanos % 1000000000ULL);
    // The guest's errno is the host's, and macOS nanosleep leaves ETIMEDOUT in it on success.
    const int savedErrno = errno;
    while (nanosleep(&req, &req) == -1 && errno == EINTR) {}
    errno = savedErrno;
#endif
}

}  // namespace TimedWait
