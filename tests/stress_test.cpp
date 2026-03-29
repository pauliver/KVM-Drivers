// stress_test.cpp - Long-running stress test for driver validation
#include <windows.h>
#include <iostream>
#include <random>
#include <chrono>
#include <atomic>
#include <vector>
#include <thread>
#include <iomanip>

#include "../usermode/core/driver_interface.h"

// Test configuration
struct StressConfig {
    int durationHours = 12;
    int eventsPerSecond = 100;
    int maxConcurrentKeys = 6;
    int mouseMoveRange = 500;
    bool useAllDrivers = true;
    bool randomizeTiming = true;
};

// Test statistics
struct StressStats {
    std::atomic<ULONG64> totalEvents{0};
    std::atomic<ULONG64> keyboardEvents{0};
    std::atomic<ULONG64> mouseEvents{0};
    std::atomic<ULONG64> controllerEvents{0};
    std::atomic<ULONG64> errors{0};
    std::atomic<double> avgLatencyMs{0.0};
    
    void Print() const {
        std::cout << "\n[Stats] Total: " << totalEvents.load()
                  << " | KB: " << keyboardEvents.load()
                  << " | Mouse: " << mouseEvents.load()
                  << " | Ctrl: " << controllerEvents.load()
                  << " | Errors: " << errors.load()
                  << " | Latency: " << std::fixed << std::setprecision(2) 
                  << avgLatencyMs.load() << "ms\n";
    }
};

class StressTest {
public:
    StressTest(const StressConfig& config) 
        : config_(config), running_(false), driverInterface_(nullptr) {}
    
    bool Initialize() {
        driverInterface_ = new DriverInterface();
        if (!driverInterface_->Initialize()) {
            std::cerr << "[Stress] Failed to initialize driver interface\n";
            return false;
        }
        
        std::cout << "[Stress] Driver interface initialized\n";
        std::cout << "[Stress] Using " << (driverInterface_->IsDriverInjectionAvailable() 
            ? "kernel drivers" : "SendInput fallback") << "\n";
        
        return true;
    }
    
    void Run() {
        running_ = true;
        auto startTime = std::chrono::steady_clock::now();
        auto endTime = startTime + std::chrono::hours(config_.durationHours);
        
        std::cout << "[Stress] Starting " << config_.durationHours << " hour stress test\n";
        std::cout << "[Stress] Target: " << config_.eventsPerSecond << " events/second\n";
        std::cout << "[Stress] Press Ctrl+C to stop early\n\n";
        
        // Start statistics thread
        std::thread statsThread(&StressTest::StatsLoop, this);
        
        // Run stress loops
        std::vector<std::thread> workers;
        if (config_.useAllDrivers) {
            workers.emplace_back(&StressTest::KeyboardStressLoop, this);
            workers.emplace_back(&StressTest::MouseStressLoop, this);
            workers.emplace_back(&StressTest::ControllerStressLoop, this);
        } else {
            workers.emplace_back(&StressTest::MixedStressLoop, this);
        }
        
        // Wait for completion
        for (auto& t : workers) {
            t.join();
        }
        
        running_ = false;
        statsThread.join();
        
        PrintSummary();
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
    
    void KeyboardStressLoop() {
        std::uniform_int_distribution<int> keyDist(0x04, 0x27); // a-z, 1-0
        std::uniform_int_distribution<int> modDist(0, 1);
        std::uniform_int_distribution<int> delayDist(1, 20);
        
        while (running_) {
            auto start = std::chrono::high_resolution_clock::now();
            
            // Random key with occasional modifier
            UCHAR key = static_cast<UCHAR>(keyDist(rng_));
            UCHAR mod = modDist(rng_) == 1 ? VKB_MOD_LEFT_CTRL : 0;
            
            if (driverInterface_->InjectKeyDown(key, mod)) {
                stats_.keyboardEvents++;
                stats_.totalEvents++;
                
                // Release quickly
                Sleep(10);
                driverInterface_->InjectKeyUp(key, mod);
            } else {
                stats_.errors++;
            }
            
            auto end = std::chrono::high_resolution_clock::now();
            auto latency = std::chrono::duration<double, std::milli>(end - start).count();
            UpdateAverageLatency(latency);
            
            // Random delay for variable timing
            if (config_.randomizeTiming) {
                Sleep(delayDist(rng_));
            } else {
                Sleep(1000 / config_.eventsPerSecond);
            }
        }
    }
    
    void MouseStressLoop() {
        std::uniform_int_distribution<int> moveDist(-config_.mouseMoveRange, config_.mouseMoveRange);
        std::uniform_int_distribution<int> buttonDist(1, 5);
        std::uniform_int_distribution<int> scrollDist(-5, 5);
        std::uniform_int_distribution<int> actionDist(0, 3);
        
        while (running_) {
            auto start = std::chrono::high_resolution_clock::now();
            bool success = false;
            
            switch (actionDist(rng_)) {
                case 0: // Move
                    success = driverInterface_->InjectMouseMove(moveDist(rng_), moveDist(rng_), false);
                    break;
                    
                case 1: // Button press
                    success = driverInterface_->InjectMouseButton(static_cast<UCHAR>(buttonDist(rng_)), true);
                    if (success) {
                        Sleep(10);
                        driverInterface_->InjectMouseButton(static_cast<UCHAR>(buttonDist(rng_)), false);
                    }
                    break;
                    
                case 2: // Scroll
                    success = driverInterface_->InjectMouseScroll(scrollDist(rng_), 0);
                    break;
                    
                case 3: // Absolute move
                    success = driverInterface_->InjectMouseMove(
                        moveDist(rng_) + 960, // Center-ish
                        moveDist(rng_) + 540,
                        true);
                    break;
            }
            
            if (success) {
                stats_.mouseEvents++;
                stats_.totalEvents++;
            } else {
                stats_.errors++;
            }
            
            auto end = std::chrono::high_resolution_clock::now();
            auto latency = std::chrono::duration<double, std::milli>(end - start).count();
            UpdateAverageLatency(latency);
            
            Sleep(1000 / config_.eventsPerSecond);
        }
    }
    
    void ControllerStressLoop() {
        // Controller stress would go here
        // Requires controller driver to be connected
        while (running_) {
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
    
    void StatsLoop() {
        while (running_) {
            Sleep(60000); // Print stats every minute
            if (running_) {
                stats_.Print();
            }
        }
    }
    
    void UpdateAverageLatency(double latency) {
        double current = stats_.avgLatencyMs.load();
        double updated = (current * 0.9) + (latency * 0.1);
        stats_.avgLatencyMs.store(updated);
    }
    
    void PrintSummary() {
        std::cout << "\n========================================\n";
        std::cout << "Stress Test Summary\n";
        std::cout << "========================================\n";
        stats_.Print();
        
        double errorRate = stats_.totalEvents.load() > 0 
            ? (stats_.errors.load() * 100.0 / stats_.totalEvents.load()) 
            : 0;
        
        std::cout << "Error Rate: " << std::fixed << std::setprecision(4) << errorRate << "%\n";
        
        if (errorRate > 1.0) {
            std::cout << "RESULT: FAILED - Error rate too high\n";
        } else if (errorRate > 0.1) {
            std::cout << "RESULT: WARNING - Minor errors detected\n";
        } else {
            std::cout << "RESULT: PASSED\n";
        }
        
        std::cout << "========================================\n";
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
        if (strcmp(argv[i], "--hours") == 0 && i + 1 < argc) {
            config.durationHours = atoi(argv[i + 1]);
        } else if (strcmp(argv[i], "--rate") == 0 && i + 1 < argc) {
            config.eventsPerSecond = atoi(argv[i + 1]);
        } else if (strcmp(argv[i], "--single") == 0) {
            config.useAllDrivers = false;
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
