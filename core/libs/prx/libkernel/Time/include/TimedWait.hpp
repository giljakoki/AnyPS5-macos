#ifndef CORE_LIBS_PRX_LIBKERNEL_TIME_INCLUDE_TIMEDWAIT_HPP
#define CORE_LIBS_PRX_LIBKERNEL_TIME_INCLUDE_TIMEDWAIT_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>

namespace TimedWait {

bool Coarse();
void BindThreadWaitState(std::atomic<int>* state);
std::uint64_t NowNanos();
std::uint64_t DeadlineNanos(std::uint64_t microseconds);
std::uint64_t RemainingMicros(std::uint64_t deadlineNanos);
void SleepNanos(std::uint64_t nanos);
void SleepUntil(std::uint64_t deadlineNanos);
void PollSleepUntil(std::uint64_t deadlineNanos);

constexpr std::uint64_t LOCK_BULK_NANOS = 40000000ULL;
constexpr std::uint64_t LOCK_POLL_NANOS = 500000ULL;
constexpr std::uint64_t LOCK_TAIL_NANOS = 1000000ULL;

template <class TTryLock, class TTryLockFor>
bool AcquireUntil(std::uint64_t deadlineNanos, TTryLock tryLock, TTryLockFor tryLockFor) {
    if (Coarse()) return tryLockFor(RemainingMicros(deadlineNanos));
    for (;;) {
        if (tryLock()) return true;
        const std::uint64_t now = NowNanos();
        if (now >= deadlineNanos) return false;
        const std::uint64_t remaining = deadlineNanos - now;
#ifdef _WIN32
        if (remaining > LOCK_TAIL_NANOS) {
            PollSleepUntil(now + (remaining < LOCK_POLL_NANOS ? remaining : LOCK_POLL_NANOS));
            continue;
        }
#else
        if (remaining > LOCK_BULK_NANOS) {
            if (tryLockFor((remaining - LOCK_BULK_NANOS) / 1000ULL)) return true;
            continue;
        }
#endif
        if (remaining <= LOCK_TAIL_NANOS) SleepUntil(deadlineNanos);
        else PollSleepUntil(now + LOCK_POLL_NANOS);
    }
}

struct Waiter;
class Condition;

struct RaiseState {
    std::mutex lock;
    std::atomic<int> pending{0};
    std::atomic<int> signalsInFlight{0};
    std::atomic<int> waitDepth{0};
    std::atomic<bool> delivering{false};
    Condition* waitingOn = nullptr;
};

enum class RaiseRoute { Queued, Signal };
enum class RaiseSignal { Run, Deferred, Stale };

void BindThreadRaiseState(RaiseState* state);
void SetRaiseDelivery(void (*deliver)());
RaiseRoute RequestRaise(RaiseState& target);
RaiseSignal ClaimRaiseSignal();
RaiseState* CurrentRaiseState();
bool EnterWait(RaiseState* state, Condition* condition);
void LeaveWait(RaiseState* state);
void DrainRaises(RaiseState* state);
void DrainRaisesInWait(RaiseState* state);

class RaiseCritical {
public:
    explicit RaiseCritical(RaiseState* state) : state(state) {
        if (state != nullptr) state->waitDepth.fetch_add(1, std::memory_order_seq_cst);
    }
    ~RaiseCritical() noexcept(false) {
        if (state != nullptr && state->waitDepth.fetch_sub(1, std::memory_order_seq_cst) == 1) DrainRaises(state);
    }
    RaiseCritical(const RaiseCritical&) = delete;
    RaiseCritical& operator=(const RaiseCritical&) = delete;

private:
    RaiseState* state;
};

class Condition {
public:
    Condition() = default;
    ~Condition() = default;
    Condition(const Condition&) = delete;
    Condition& operator=(const Condition&) = delete;

    void NotifyOne();
    void NotifyAll();

    template <class TLock>
    void Wait(TLock& lock) {
#ifdef _WIN32
        if (!Coarse()) {
            Waiter* waiter = enqueue();
            lock.unlock();
            waitSignal(waiter);
            lock.lock();
            return;
        }
#endif
#ifdef _WIN32
        coarse.wait(lock);
#else
        RaiseState* raise = CurrentRaiseState();
        const std::uint64_t observed = generation.load(std::memory_order_seq_cst);
        lock.unlock();
        {
            RaiseCritical critical(raise);
            const auto guard = internal;
            std::unique_lock inner(*guard);
            const bool published = EnterWait(raise, this);
            if (generation.load(std::memory_order_seq_cst) == observed && (!published || raise->pending.load(std::memory_order_seq_cst) == 0)) signal.wait(inner);
            LeaveWait(raise);
        }
        DrainRaisesInWait(raise);
        lock.lock();
#endif
    }

    template <class TLock>
    bool WaitUntil(TLock& lock, std::uint64_t deadlineNanos) {
#ifdef _WIN32
        if (!Coarse()) {
            Waiter* waiter = enqueue();
            lock.unlock();
            const bool signaled = waitSignalUntil(waiter, deadlineNanos);
            lock.lock();
            return signaled;
        }
#endif
#ifdef _WIN32
        return coarse.wait_for(lock, std::chrono::microseconds(RemainingMicros(deadlineNanos))) == std::cv_status::no_timeout;
#else
        RaiseState* raise = CurrentRaiseState();
        const std::uint64_t observed = generation.load(std::memory_order_seq_cst);
        lock.unlock();
        bool signaled = true;
        {
            RaiseCritical critical(raise);
            const auto guard = internal;
            std::unique_lock inner(*guard);
            const bool published = EnterWait(raise, this);
            if (generation.load(std::memory_order_seq_cst) == observed && (!published || raise->pending.load(std::memory_order_seq_cst) == 0))
                signaled = signal.wait_for(inner, std::chrono::microseconds(RemainingMicros(deadlineNanos))) == std::cv_status::no_timeout;
            LeaveWait(raise);
        }
        DrainRaisesInWait(raise);
        lock.lock();
        return signaled;
#endif
    }

    template <class TLock, class TPredicate>
    void Wait(TLock& lock, TPredicate predicate) {
        while (!predicate()) Wait(lock);
    }

    template <class TLock, class TPredicate>
    bool WaitUntil(TLock& lock, std::uint64_t deadlineNanos, TPredicate predicate) {
        while (!predicate()) {
            if (!WaitUntil(lock, deadlineNanos)) return predicate();
        }
        return true;
    }

private:
    friend RaiseRoute RequestRaise(RaiseState& target);
#ifdef _WIN32
    Waiter* enqueue();
    void unlink(Waiter* waiter);
    void waitSignal(Waiter* waiter);
    bool waitSignalUntil(Waiter* waiter, std::uint64_t deadlineNanos);

    std::mutex queueLock;
    Waiter* head = nullptr;
    Waiter* tail = nullptr;
    std::condition_variable_any coarse;
#else
    std::shared_ptr<std::mutex> internal = std::make_shared<std::mutex>();
    std::condition_variable signal;
    std::atomic<std::uint64_t> generation{0};
#endif
};

class LockWaiters {
public:
    LockWaiters() = default;
    LockWaiters(const LockWaiters&) = delete;
    LockWaiters& operator=(const LockWaiters&) = delete;

    template <class TTryLock>
    void Acquire(TTryLock tryLock) {
        if (tryLock()) return;
        RaiseCritical critical(CurrentRaiseState());
        std::unique_lock guard(lock);
        Waiting waiting(waiters);
        while (!tryLock()) released.Wait(guard);
    }

    template <class TTryLock>
    bool AcquireUntil(std::uint64_t deadlineNanos, TTryLock tryLock) {
        if (tryLock()) return true;
        RaiseCritical critical(CurrentRaiseState());
        std::unique_lock guard(lock);
        Waiting waiting(waiters);
        while (!tryLock()) {
            if (!released.WaitUntil(guard, deadlineNanos)) return tryLock();
        }
        return true;
    }

    void Released() {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (waiters.load(std::memory_order_seq_cst) == 0) return;
        RaiseCritical critical(CurrentRaiseState());
        std::lock_guard guard(lock);
        released.NotifyAll();
    }

private:
    struct Waiting {
        explicit Waiting(std::atomic<int>& count) : count(count) {
            count.fetch_add(1, std::memory_order_seq_cst);
            std::atomic_thread_fence(std::memory_order_seq_cst);
        }
        ~Waiting() { count.fetch_sub(1, std::memory_order_seq_cst); }
        Waiting(const Waiting&) = delete;
        Waiting& operator=(const Waiting&) = delete;
        std::atomic<int>& count;
    };

    std::mutex lock;
    Condition released;
    std::atomic<int> waiters{0};
};

template <class TNative>
class GuestLock {
public:
    void lock() {
#ifdef __APPLE__
        waiters.Acquire([this] { return native.try_lock(); });
#else
        native.lock();
#endif
    }

    bool try_lock() { return native.try_lock(); }

    bool try_lock_for(std::chrono::microseconds timeout) {
#ifdef __APPLE__
        return waiters.AcquireUntil(DeadlineNanos(timeout.count() > 0 ? static_cast<std::uint64_t>(timeout.count()) : 0), [this] { return native.try_lock(); });
#else
        return native.try_lock_for(timeout);
#endif
    }

    template <class TClock, class TDuration>
    bool try_lock_until(const std::chrono::time_point<TClock, TDuration>& deadline) {
#ifdef __APPLE__
        const auto now = TClock::now();
        if (deadline <= now) return native.try_lock();
        return try_lock_for(std::chrono::ceil<std::chrono::microseconds>(deadline - now));
#else
        return native.try_lock_until(deadline);
#endif
    }

    void unlock() {
        native.unlock();
#ifdef __APPLE__
        waiters.Released();
#endif
    }

private:
    TNative native;
#ifdef __APPLE__
    LockWaiters waiters;
#endif
};

}  // namespace TimedWait

#endif
