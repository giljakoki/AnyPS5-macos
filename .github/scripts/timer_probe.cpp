#include <mach/mach.h>
#include <mach/task_policy.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <time.h>
#include <vector>

namespace {

constexpr int Rounds = 40;

void Report(const char* label, std::vector<double> late) {
    std::sort(late.begin(), late.end());
    std::printf("%-44s late by median %6.2f ms, 90th %6.2f ms, max %6.2f ms\n", label, late[late.size() / 2], late[late.size() * 9 / 10], late.back());
}

void Measure(const char* label, bool interactive) {
    std::thread worker([&] {
        if (interactive) pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        std::mutex mutex;
        std::condition_variable condition;
        std::vector<double> waits;
        std::vector<double> sleeps;
        for (int round = 0; round < Rounds; ++round) {
            std::unique_lock lock(mutex);
            auto start = std::chrono::steady_clock::now();
            condition.wait_for(lock, std::chrono::milliseconds(5));
            waits.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() - 5.0);
            lock.unlock();
            start = std::chrono::steady_clock::now();
            const timespec request{0, 1000000};
            nanosleep(&request, nullptr);
            sleeps.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() - 1.0);
        }
        char name[96];
        std::snprintf(name, sizeof(name), "%s, 5 ms condition wait", label);
        Report(name, waits);
        std::snprintf(name, sizeof(name), "%s, 1 ms nanosleep", label);
        Report(name, sleeps);
    });
    worker.join();
}

}

int main() {
    Measure("default QoS", false);
    Measure("user-interactive QoS", true);
    task_qos_policy policy{LATENCY_QOS_TIER_0, THROUGHPUT_QOS_TIER_0};
    const kern_return_t result = task_policy_set(mach_task_self(), TASK_BASE_QOS_POLICY, reinterpret_cast<task_policy_t>(&policy), TASK_QOS_POLICY_COUNT);
    std::printf("task_policy_set(TASK_BASE_QOS_POLICY, tier 0): %d\n", result);
    Measure("default QoS, task latency tier 0", false);
}
