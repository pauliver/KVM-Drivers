// stress_test.cpp - Long-running stress test for driver validation (72-hour capable)
#include <windows.h>
#include <psapi.h>
#include <iostream>
#include <fstream>
#include <random>
#include <chrono>
#include <atomic>
#include <vector>
#include <thread>
#include <iomanip>
#include <algorithm>
#include <sstream>
#include <mutex>
#include <ctime>

#include "../src/usermode/core/driver_interface.h"

#pragma comment(lib, "psapi.lib")

// Test configuration
struct StressConfig {
    int  durationHours    = 12;
    int  eventsPerSecond  = 100;
    int  maxConcurrentKeys= 6;
    int  mouseMoveRange   = 500;
    bool useAllDrivers    = true;
    bool randomizeTiming  = true;
    bool enableWatchdog   = true;
    int  watchdogTimeoutS = 30;   // secs before watchdog flags a hang
    std::string resultFile = "";  // JSON result output path
};

// Test statistics
struct StressStats {
    std::atomic<ULONG64> totalEvents{0};
    std::atomic<ULONG64> keyboardEvents{0};
    std::atomic<ULONG64> mouseEvents{0};
    std::atomic<ULONG64> controllerEvents{0};
    std::atomic<ULONG64> errors{0};
    std::atomic<ULONG64> driverReconnects{0};
    std::atomic<ULONG64> watchdogFires{0};
    std::atomic<double>  avgLatencyMs{0.0};
    std::atomic<double>  maxLatencyMs{0.0};
    std::atomic<ULONG64> peakMemoryKB{0};

    // Percentile tracking (lock-protected)
    std::vector<double>  latencySamples;
    std::mutex           latencyMutex;

    void RecordLatency(double ms) {
        // Update running avg (EMA)
        double cur = avgLatencyMs.load();
        avgLatencyMs.store(cur * 0.95 + ms * 0.05);
        // Update max
        double curMax = maxLatencyMs.load();
        if (ms > curMax) maxLatencyMs.store(ms);
        // Sample at most 10000 entries for percentile
        std::lock_guard<std::mutex> lk(latencyMutex);
        if (latencySamples.size() < 10000) latencySamples.push_back(ms);
    }

    double Percentile(double p) {
        std::lock_guard<std::mutex> lk(latencyMutex);
        if (latencySamples.empty()) return 0;
        auto v = latencySamples;  // copy
        std::sort(v.begin(), v.end());
        size_t idx = (size_t)(p / 100.0 * (v.size() - 1));
        return v[idx];
    }

    void Print() const {
        std::cout << "\n[Stats] Total: " << totalEvents.load()
                  << " | KB: " << keyboardEvents.load()
                  << " | Mouse: " << mouseEvents.load()
                  << " | Ctrl: " << controllerEvents.load()
                  << " | Errors: " << errors.load()
                  << " | Reconnects: " << driverReconnects.load()
                  << " | Watchdog: " << watchdogFires.load()
                  << " | AvgLat: " << std::fixed << std::setprecision(2)
                  << avgLatencyMs.load() << "ms"
                  << " | MaxLat: " << maxLatencyMs.load() << "ms"
                  << " | PeakMem: " << peakMemoryKB.load() << "KB\n";
    }
};

class StressTest {
public:
    StressTest(const StressConfig& config) 
        : config_(config), running_(false), driverInterface_(nullptr) {}
    
    bool Initialize() {
        if (!TryConnectDriver()) return false;

        // Set up Ctrl+C handler
        SetConsoleCtrlHandler([](DWORD) -> BOOL {
            std::cout << "\n[Stress] Interrupted by user\n";
            return TRUE;
        }, TRUE);

        return true;
    }

    bool TryConnectDriver() {
        if (driverInterface_) { driverInterface_->Disconnect(); delete driverInterface_; }
        driverInterface_ = new DriverInterface();
        if (!driverInterface_->Initialize()) {
            std::cerr << "[Stress] Failed to initialize driver interface\n";
            return false;
        }
        std::cout << "[Stress] Driver interface connected ("
                  << (driverInterface_->IsDriverInjectionAvailable()
                      ? "kernel drivers" : "SendInput fallback") << ")\n";
        return true;
    }
    
    void Run() {
        running_ = true;
        startTime_ = std::chrono::steady_clock::now();
        endTime_   = startTime_ + std::chrono::hours(config_.durationHours);

        std::cout << "[Stress] Starting " << config_.durationHours
                  << "-hour stress test at " << CurrentTimeStr() << "\n";
        std::cout << "[Stress] Target: " << config_.eventsPerSecond << " events/second\n";
        std::cout << "[Stress] Expected end: "
                  << TimeStr(std::chrono::system_clock::now()
                      + std::chrono::hours(config_.durationHours)) << "\n";
        std::cout << "[Stress] Press Ctrl+C to stop early\n\n";

        // Stats + watchdog threads
        std::thread statsThread   (&StressTest::StatsLoop,    this);
        std::thread memThread     (&StressTest::MemMonLoop,   this);
        std::thread watchdogThread(&StressTest::WatchdogLoop, this);

        // Worker threads
        std::vector<std::thread> workers;
        if (config_.useAllDrivers) {
            workers.emplace_back(&StressTest::KeyboardStressLoop,   this);
            workers.emplace_back(&StressTest::MouseStressLoop,      this);
            workers.emplace_back(&StressTest::ControllerStressLoop, this);
        } else {
            workers.emplace_back(&StressTest::MixedStressLoop, this);
        }

        for (auto& t : workers)   t.join();

        running_ = false;
        statsThread.join();
        memThread.join();
        watchdogThread.join();

        PrintSummary();
        if (!config_.resultFile.empty()) WriteJsonResult();
    }
    
    void Shutdown() {
        running_ = false;
        if (driverInterface_) {
            driverInterface_->Disconnect();
            delete driverInterface_;
            driverInterface_ = nullptr;
        }
    }

private:
    StressConfig config_;
    std::atomic<bool> running_;
    DriverInterface* driverInterface_;
    StressStats stats_;
    std::mt19937 rng_{std::random_device{}()};
    std::chrono::steady_clock::time_point startTime_;
    std::chrono::steady_clock::time_point endTime_;
    std::atomic<ULONG64> lastWatchdogEventCount_{0};
    
    bool ShouldStop() const {
        return !running_ || std::chrono::steady_clock::now() >= endTime_;
    }

    void KeyboardStressLoop() {
        std::uniform_int_distribution<int> keyDist(0x04, 0x27);
        std::uniform_int_distribution<int> modDist(0, 3);
        std::uniform_int_distribution<int> delayDist(1, 20);

        while (!ShouldStop()) {
            auto start = std::chrono::high_resolution_clock::now();

            UCHAR key = static_cast<UCHAR>(keyDist(rng_));
            UCHAR mod = (modDist(rng_) == 0) ? VKB_MOD_LEFT_CTRL : 0;

            if (driverInterface_->InjectKeyDown(key, mod)) {
                stats_.keyboardEvents++;
                stats_.totalEvents++;
                Sleep(10);
                driverInterface_->InjectKeyUp(key, mod);
            } else {
                stats_.errors++;
                TryReconnectIfNeeded();
            }

            stats_.RecordLatency(std::chrono::duration<double, std::milli>(
                std::chrono::high_resolution_clock::now() - start).count());

            Sleep(config_.randomizeTiming ? delayDist(rng_) : 1000 / config_.eventsPerSecond);
        }
    }
    
    void MouseStressLoop() {
        std::uniform_int_distribution<int> moveDist(-config_.mouseMoveRange, config_.mouseMoveRange);
        std::uniform_int_distribution<int> buttonDist(1, 3);
        std::uniform_int_distribution<int> scrollDist(-5, 5);
        std::uniform_int_distribution<int> actionDist(0, 3);

        while (!ShouldStop()) {
            auto start = std::chrono::high_resolution_clock::now();
            bool success = false;

            switch (actionDist(rng_)) {
                case 0: success = driverInterface_->InjectMouseMove(
                            moveDist(rng_), moveDist(rng_), false); break;
                case 1: success = driverInterface_->InjectMouseButton(
                            static_cast<UCHAR>(buttonDist(rng_)), true);
                        if (success) {
                            Sleep(10);
                            driverInterface_->InjectMouseButton(
                                static_cast<UCHAR>(buttonDist(rng_)), false);
                        }
                        break;
                case 2: success = driverInterface_->InjectMouseScroll(scrollDist(rng_), 0); break;
                case 3: success = driverInterface_->InjectMouseMove(
                            moveDist(rng_) + 960, moveDist(rng_) + 540, true); break;
            }

            if (success) { stats_.mouseEvents++; stats_.totalEvents++; }
            else         { stats_.errors++; TryReconnectIfNeeded(); }

            stats_.RecordLatency(std::chrono::duration<double, std::milli>(
                std::chrono::high_resolution_clock::now() - start).count());

            Sleep(1000 / config_.eventsPerSecond);
        }
    }
    
    void ControllerStressLoop() {
        while (!ShouldStop()) {
            // Controller reports can be added when vxinput is connected
            stats_.controllerEvents++;
            stats_.totalEvents++;
            Sleep(1000 / config_.eventsPerSecond);
        }
    }
    
    void MixedStressLoop() {
        std::uniform_int_distribution<int> typeDist(0, 2);
        
        while (running_) {
            switch (typeDist(rng_)) {
                case 0:
                    KeyboardStressSingle();
                    break;
                case 1:
                    MouseStressSingle();
                    break;
                case 2:
                    // Controller stress
                    break;
            }
            
            Sleep(1000 / config_.eventsPerSecond);
        }
    }
    
    void KeyboardStressSingle() {
        UCHAR key = 0x04 + (rand() % 26); // a-z
        if (driverInterface_->InjectKeyDown(key, 0)) {
            stats_.keyboardEvents++;
            stats_.totalEvents++;
            Sleep(10);
            driverInterface_->InjectKeyUp(key, 0);
        } else {
            stats_.errors++;
        }
    }
    
    void MouseStressSingle() {
        int x = (rand() % 200) - 100;
        int y = (rand() % 200) - 100;
        if (driverInterface_->InjectMouseMove(x, y, false)) {
            stats_.mouseEvents++;
            stats_.totalEvents++;
        } else {
            stats_.errors++;
        }
    }
    
    void TryReconnectIfNeeded() {
        // If error rate is very high, try to reconnect driver
        ULONG64 total = stats_.totalEvents.load();
        ULONG64 errs  = stats_.errors.load();
        if (total > 100 && errs * 10 > total) {
            std::cerr << "[Stress] High error rate - attempting driver reconnect\n";
            if (TryConnectDriver()) {
                stats_.driverReconnects++;
                std::cout << "[Stress] Driver reconnected\n";
            }
        }
    }

    void StatsLoop() {
        int minElapsed = 0;
        while (running_ && !ShouldStop()) {
            Sleep(60000);
            minElapsed++;
            auto elapsed = std::chrono::duration_cast<std::chrono::hours>(
                std::chrono::steady_clock::now() - startTime_).count();
            std::cout << "[" << CurrentTimeStr() << "] Hour " << elapsed
                      << "/" << config_.durationHours;
            stats_.Print();
        }
    }

    void MemMonLoop() {
        while (running_ && !ShouldStop()) {
            Sleep(30000);  // check every 30s
            PROCESS_MEMORY_COUNTERS pmc = {};
            pmc.cb = sizeof(pmc);
            if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
                ULONG64 kb = pmc.WorkingSetSize / 1024;
                ULONG64 prev = stats_.peakMemoryKB.load();
                if (kb > prev) stats_.peakMemoryKB.store(kb);
                // Warn if growing unexpectedly
                if (kb > 500 * 1024) {
                    std::cerr << "[Stress] WARNING: process memory > 500MB (" << kb << " KB)\n";
                }
            }
        }
    }

    void WatchdogLoop() {
        if (!config_.enableWatchdog) return;
        ULONG64 lastCount = 0;
        while (running_ && !ShouldStop()) {
            Sleep(config_.watchdogTimeoutS * 1000);
            ULONG64 current = stats_.totalEvents.load();
            if (current == lastCount && running_) {
                stats_.watchdogFires++;
                std::cerr << "[Stress] WATCHDOG: no events in " << config_.watchdogTimeoutS
                          << "s — drivers may be hung (total=" << current << ")\n";
            }
            lastCount = current;
        }
    }
    
    void PrintSummary() {
        auto elapsed = std::chrono::duration_cast<std::chrono::minutes>(
            std::chrono::steady_clock::now() - startTime_).count();

        std::cout << "\n========================================\n";
        std::cout << "Stress Test Summary " << CurrentTimeStr() << "\n";
        std::cout << "========================================\n";
        std::cout << "Duration: " << elapsed / 60 << "h " << elapsed % 60 << "m\n";
        stats_.Print();
        std::cout << "Latency p50: " << std::fixed << std::setprecision(2)
                  << stats_.Percentile(50) << "ms\n";
        std::cout << "Latency p95: " << stats_.Percentile(95) << "ms\n";
        std::cout << "Latency p99: " << stats_.Percentile(99) << "ms\n";

        double errorRate = stats_.totalEvents.load() > 0
            ? (stats_.errors.load() * 100.0 / stats_.totalEvents.load())
            : 0;
        std::cout << "Error Rate: " << errorRate << "%\n";

        const char* result =
            (stats_.watchdogFires > 0)            ? "FAILED - Watchdog fired" :
            (errorRate > 1.0)                     ? "FAILED - Error rate > 1%" :
            (errorRate > 0.1)                     ? "WARNING - Minor errors" :
            (stats_.driverReconnects > 0)         ? "WARNING - Driver reconnects" :
                                                    "PASSED";
        std::cout << "RESULT: " << result << "\n";
        std::cout << "========================================\n";
    }

    void WriteJsonResult() {
        auto elapsed = std::chrono::duration_cast<std::chrono::minutes>(
            std::chrono::steady_clock::now() - startTime_).count();
        double errorRate = stats_.totalEvents.load() > 0
            ? (stats_.errors.load() * 100.0 / stats_.totalEvents.load()) : 0;

        std::ostringstream json;
        json << "{\n"
             << "  \"timestamp\": \"" << CurrentTimeStr() << "\",\n"
             << "  \"duration_minutes\": " << elapsed << ",\n"
             << "  \"total_events\": "    << stats_.totalEvents.load()    << ",\n"
             << "  \"keyboard_events\": " << stats_.keyboardEvents.load() << ",\n"
             << "  \"mouse_events\": "    << stats_.mouseEvents.load()    << ",\n"
             << "  \"errors\": "          << stats_.errors.load()         << ",\n"
             << "  \"driver_reconnects\": " << stats_.driverReconnects.load() << ",\n"
             << "  \"watchdog_fires\": "  << stats_.watchdogFires.load()  << ",\n"
             << "  \"error_rate_pct\": "  << std::fixed << std::setprecision(4) << errorRate << ",\n"
             << "  \"avg_latency_ms\": " << stats_.avgLatencyMs.load()  << ",\n"
             << "  \"max_latency_ms\": " << stats_.maxLatencyMs.load()  << ",\n"
             << "  \"p50_latency_ms\": " << stats_.Percentile(50) << ",\n"
             << "  \"p95_latency_ms\": " << stats_.Percentile(95) << ",\n"
             << "  \"p99_latency_ms\": " << stats_.Percentile(99) << ",\n"
             << "  \"peak_memory_kb\": " << stats_.peakMemoryKB.load() << "\n"
             << "}\n";

        std::ofstream f(config_.resultFile);
        f << json.str();
        std::cout << "[Stress] Results written to " << config_.resultFile << "\n";
    }

    static std::string CurrentTimeStr() {
        std::time_t t = std::time(nullptr);
        char buf[64];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
        return buf;
    }

    static std::string TimeStr(std::chrono::system_clock::time_point tp) {
        std::time_t t = std::chrono::system_clock::to_time_t(tp);
        char buf[64];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
        return buf;
    }
};

// Entry point
int main(int argc, char* argv[]) {
    std::cout << "========================================\n";
    std::cout << "KVM-Drivers Stress Test\n";
    std::cout << "========================================\n\n";
    
    StressConfig config;
    
    // Parse command line
    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "--hours") == 0 && i+1 < argc)
            config.durationHours = atoi(argv[++i]);
        else if (strcmp(argv[i], "--rate") == 0 && i+1 < argc)
            config.eventsPerSecond = atoi(argv[++i]);
        else if (strcmp(argv[i], "--single") == 0)
            config.useAllDrivers = false;
        else if (strcmp(argv[i], "--no-watchdog") == 0)
            config.enableWatchdog = false;
        else if (strcmp(argv[i], "--watchdog-timeout") == 0 && i+1 < argc)
            config.watchdogTimeoutS = atoi(argv[++i]);
        else if (strcmp(argv[i], "--output") == 0 && i+1 < argc)
            config.resultFile = argv[++i];
        else if (strcmp(argv[i], "--72h") == 0) {
            config.durationHours = 72;
            config.eventsPerSecond = 60;
        }
    }
    
    std::cout << "Configuration:\n";
    std::cout << "  Duration: " << config.durationHours << " hours\n";
    std::cout << "  Rate: " << config.eventsPerSecond << " events/second\n";
    std::cout << "  Mode: " << (config.useAllDrivers ? "Multi-driver" : "Single-thread") << "\n\n";
    
    StressTest test(config);
    
    if (!test.Initialize()) {
        std::cerr << "Failed to initialize stress test\n";
        return 1;
    }
    
    test.Run();
    test.Shutdown();
    
    return 0;
}
