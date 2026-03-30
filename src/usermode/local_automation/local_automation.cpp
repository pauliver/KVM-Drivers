// local_automation.cpp - Direct automation mode for same-machine testing
// Bypasses network entirely - connects directly to driver interface
// Usage: local_automation.exe --script tests/mytest.yaml

#include <windows.h>
#include <gdiplus.h>
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <fstream>
#include <sstream>
#include <cmath>
#include <yaml-cpp/yaml.h>

#pragma comment(lib, "gdiplus.lib")

#include "../usermode/core/driver_interface.h"
#include "../common/logging/unified_logger_user.h"
#include "../common/performance/performance_monitor.h"

// Test action types
enum class ActionType {
    MouseMove,
    MouseClick,
    MouseScroll,
    KeyDown,
    KeyUp,
    KeyPress,
    KeyCombo,
    Wait,
    Screenshot,
    Assert
};

struct TestAction {
    ActionType type;
    std::string description;
    std::map<std::string, std::string> params;
    int delayAfterMs = 0;
};

struct TestResult {
    bool passed;
    std::string message;
    std::chrono::milliseconds duration;
};

class LocalAutomation {
public:
    LocalAutomation() 
        : driverInterface_(nullptr)
        , logger_(nullptr)
        , perfMonitor_(nullptr)
        , currentTest_(nullptr)
        , testPassed_(0)
        , testFailed_(0) {
    }
    
    ~LocalAutomation() {
        Shutdown();
    }
    
    bool Initialize() {
        std::cout << "[LocalAutomation] Initializing...\n";
        
        // Initialize logging
        logger_ = new LOGGER_CONTEXT();
        UserLogger_Initialize("local_automation.log", LOG_LEVEL_DEBUG, LOG_CATEGORY_ALL);

        // Initialize performance monitoring
        perfMonitor_ = new PERF_MONITOR_CONTEXT();
        PerfMonitorInitialize(perfMonitor_, PERF_CATEGORY_ALL, 1000, 5000);
        
        // Initialize driver interface (direct, no network)
        driverInterface_ = new DriverInterface();
        if (!driverInterface_->Initialize()) {
            std::cerr << "[LocalAutomation] Failed to initialize driver interface\n";
            LOG_ERROR(logger_, LOG_CATEGORY_DRIVER, "LocalAuto", 
                "Driver interface initialization failed");
            return false;
        }
        
        LOG_INFO(logger_, LOG_CATEGORY_GENERAL, "LocalAuto", 
            "Local automation initialized successfully");
        
        return true;
    }
    
    void Shutdown() {
        if (driverInterface_) {
            driverInterface_->Disconnect();
            delete driverInterface_;
            driverInterface_ = nullptr;
        }
        
        if (perfMonitor_) {
            PerfMonitorShutdown(perfMonitor_);
            delete perfMonitor_;
            perfMonitor_ = nullptr;
        }

        if (logger_) {
            UserLogger_Shutdown();
            delete logger_;
            logger_ = nullptr;
        }
    }
    
    // Load and execute a YAML test script
    bool RunTestScript(const std::string& scriptPath) {
        std::cout << "[LocalAutomation] Loading test script: " << scriptPath << "\n";
        
        try {
            YAML::Node config = YAML::LoadFile(scriptPath);
            
            // Parse test metadata
            std::string testName = config["name"].as<std::string>("Unnamed Test");
            std::string testDescription = config["description"].as<std::string>("");
            
            std::cout << "\n========================================\n";
            std::cout << "Test: " << testName << "\n";
            if (!testDescription.empty()) {
                std::cout << "Description: " << testDescription << "\n";
            }
            std::cout << "========================================\n\n";
            
            LOG_INFO(logger_, LOG_CATEGORY_GENERAL, "LocalAuto", 
                "Starting test: %s", testName.c_str());
            
            auto testStart = std::chrono::steady_clock::now();
            
            // Execute setup actions
            if (config["setup"]) {
                std::cout << "[SETUP]\n";
                if (!ExecuteActions(config["setup"])) {
                    return false;
                }
            }
            
            // Execute main test steps
            if (config["steps"]) {
                std::cout << "[STEPS]\n";
                if (!ExecuteSteps(config["steps"])) {
                    return false;
                }
            }
            
            // Execute assertions
            if (config["assertions"]) {
                std::cout << "[ASSERTIONS]\n";
                if (!ExecuteAssertions(config["assertions"])) {
                    return false;
                }
            }
            
            // Execute teardown
            if (config["teardown"]) {
                std::cout << "[TEARDOWN]\n";
                ExecuteActions(config["teardown"]);
            }
            
            auto testEnd = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(testEnd - testStart);
            
            // Print summary
            std::cout << "\n========================================\n";
            std::cout << "TEST COMPLETE\n";
            std::cout << "Duration: " << duration.count() << "ms\n";
            std::cout << "Passed: " << testPassed_ << "\n";
            std::cout << "Failed: " << testFailed_ << "\n";
            std::cout << "========================================\n";
            
            LOG_INFO(logger_, LOG_CATEGORY_GENERAL, "LocalAuto", 
                "Test completed: %s (passed=%d, failed=%d, duration=%dms)",
                testName.c_str(), testPassed_, testFailed_, (int)duration.count());
            
            return testFailed_ == 0;
            
        } catch (const YAML::Exception& e) {
            std::cerr << "[LocalAutomation] YAML error: " << e.what() << "\n";
            return false;
        }
    }
    
    // Run in interactive mode (REPL)
    void RunInteractive() {
        std::cout << "\nLocal Automation Interactive Mode\n";
        std::cout << "Type 'help' for commands, 'exit' to quit\n\n";
        
        std::string line;
        while (true) {
            std::cout << "auto> ";
            std::getline(std::cin, line);
            
            if (line == "exit" || line == "quit") {
                break;
            } else if (line == "help") {
                PrintHelp();
            } else if (line.rfind("click ", 0) == 0) {
                HandleClickCommand(line.substr(6));
            } else if (line.rfind("move ", 0) == 0) {
                HandleMoveCommand(line.substr(5));
            } else if (line.rfind("key ", 0) == 0) {
                HandleKeyCommand(line.substr(4));
            } else if (line.rfind("type ", 0) == 0) {
                HandleTypeCommand(line.substr(5));
            } else if (line.rfind("wait ", 0) == 0) {
                HandleWaitCommand(line.substr(5));
            } else if (line == "screenshot") {
                TakeScreenshot("interactive.png");
            } else {
                std::cout << "Unknown command. Type 'help' for available commands.\n";
            }
        }
    }
    
    // Run quick smoke test
    bool RunSmokeTest() {
        std::cout << "\n========================================\n";
        std::cout << "RUNNING SMOKE TEST\n";
        std::cout << "========================================\n\n";
        
        bool allPassed = true;
        
        // Test 1: Keyboard
        std::cout << "[1/5] Testing keyboard injection...\n";
        if (!TestKeyboard()) {
            std::cerr << "  FAILED: Keyboard test\n";
            allPassed = false;
        } else {
            std::cout << "  PASSED\n";
        }
        
        // Test 2: Mouse
        std::cout << "[2/5] Testing mouse injection...\n";
        if (!TestMouse()) {
            std::cerr << "  FAILED: Mouse test\n";
            allPassed = false;
        } else {
            std::cout << "  PASSED\n";
        }
        
        // Test 3: Controller
        std::cout << "[3/5] Testing controller injection...\n";
        if (!TestController()) {
            std::cerr << "  FAILED: Controller test\n";
            allPassed = false;
        } else {
            std::cout << "  PASSED\n";
        }
        
        // Test 4: Display
        std::cout << "[4/5] Testing display capture...\n";
        if (!TestDisplay()) {
            std::cerr << "  FAILED: Display test\n";
            allPassed = false;
        } else {
            std::cout << "  PASSED\n";
        }
        
        // Test 5: Performance
        std::cout << "[5/5] Testing performance...\n";
        if (!TestPerformance()) {
            std::cerr << "  FAILED: Performance test\n";
            allPassed = false;
        } else {
            std::cout << "  PASSED\n";
        }
        
        std::cout << "\n========================================\n";
        std::cout << "SMOKE TEST " << (allPassed ? "PASSED" : "FAILED") << "\n";
        std::cout << "========================================\n";
        
        return allPassed;
    }

private:
    DriverInterface* driverInterface_;
    PLOGGER_CONTEXT logger_;
    PPERF_MONITOR_CONTEXT perfMonitor_;
    
    int testPassed_;
    int testFailed_;
    const YAML::Node* currentTest_;
    
    bool ExecuteActions(const YAML::Node& actions) {
        for (const auto& action : actions) {
            if (!ExecuteSingleAction(action)) {
                return false;
            }
        }
        return true;
    }
    
    bool ExecuteSteps(const YAML::Node& steps) {
        for (const auto& step : steps) {
            int id = step["id"].as<int>(0);
            std::string description = step["description"].as<std::string>("");
            
            if (!description.empty()) {
                std::cout << "  Step " << id << ": " << description << "\n";
            }
            
            if (!ExecuteSingleAction(step)) {
                std::cerr << "    FAILED\n";
                testFailed_++;
                if (step["continue_on_error"].as<bool>(false) == false) {
                    return false;
                }
            } else {
                testPassed_++;
            }
            
            // Delay after step
            int delay = step["delay_after_ms"].as<int>(100);
            if (delay > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            }
        }
        return true;
    }
    
    bool ExecuteSingleAction(const YAML::Node& action) {
        std::string actionType = action["action"].as<std::string>("");
        auto params = action["params"];
        
        ULONGLONG perfStart;
        PerfMonitorStart(perfMonitor_, PERF_CATEGORY_IO, 
            actionType.c_str(), __FUNCTION__, __LINE__, &perfStart);
        
        bool result = false;
        
        if (actionType == "input.mouse.click") {
            int x = params["x"].as<int>(0);
            int y = params["y"].as<int>(0);
            std::string button = params["button"].as<std::string>("left");
            
            driverInterface_->InjectMouseMove(x, y, true);
            int btnCode = (button == "left") ? 1 : (button == "right") ? 2 : 3;
            driverInterface_->InjectMouseButton(btnCode, true);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            driverInterface_->InjectMouseButton(btnCode, false);
            result = true;
            
        } else if (actionType == "input.mouse.move") {
            int x = params["x"].as<int>(0);
            int y = params["y"].as<int>(0);
            bool absolute = params["absolute"].as<bool>(true);
            
            result = driverInterface_->InjectMouseMove(x, y, absolute);
            
        } else if (actionType == "input.keyboard.keydown") {
            int keycode = params["keycode"].as<int>(0);
            int modifiers = params["modifiers"].as<int>(0);
            result = driverInterface_->InjectKeyDown(keycode, modifiers);
            
        } else if (actionType == "input.keyboard.keyup") {
            int keycode = params["keycode"].as<int>(0);
            int modifiers = params["modifiers"].as<int>(0);
            result = driverInterface_->InjectKeyUp(keycode, modifiers);
            
        } else if (actionType == "input.keyboard.type") {
            std::string text = params["text"].as<std::string>("");
            int interval = params["interval_ms"].as<int>(10);
            
            for (char c : text) {
                driverInterface_->InjectKeyDown(c, 0);
                std::this_thread::sleep_for(std::chrono::milliseconds(interval));
                driverInterface_->InjectKeyUp(c, 0);
            }
            result = true;
            
        } else if (actionType == "input.keyboard.combo") {
            auto keys = params["keys"].as<std::vector<std::string>>();
            // Press all keys
            for (const auto& key : keys) {
                driverInterface_->InjectKeyDown(KeyNameToCode(key), 0);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            // Release all keys in reverse
            for (auto it = keys.rbegin(); it != keys.rend(); ++it) {
                driverInterface_->InjectKeyUp(KeyNameToCode(*it), 0);
            }
            result = true;
            
        } else if (actionType == "system.wait") {
            int duration = params["duration_ms"].as<int>(1000);
            std::this_thread::sleep_for(std::chrono::milliseconds(duration));
            result = true;
            
        } else if (actionType == "display.set_resolution") {
            int width = params["width"].as<int>(1920);
            int height = params["height"].as<int>(1080);
            result = driverInterface_->SetDisplayResolution(width, height);
            
        } else if (actionType == "display.capture") {
            std::string name = params["name"].as<std::string>("capture");
            result = TakeScreenshot(name + ".png");
            
        } else {
            std::cerr << "Unknown action type: " << actionType << "\n";
            result = false;
        }
        
        PerfMonitorEnd(perfMonitor_, PERF_CATEGORY_IO,
            actionType.c_str(), __FUNCTION__, __LINE__, perfStart, nullptr);
        
        return result;
    }
    
    // Helper methods
    int KeyNameToCode(const std::string& name) {
        static std::map<std::string, int> keyMap = {
            {"ctrl", 0x11}, {"shift", 0x10}, {"alt", 0x12},
            {"tab", 0x09}, {"return", 0x0D}, {"enter", 0x0D},
            {"esc", 0x1B}, {"escape", 0x1B}, {"space", 0x20},
            {"delete", 0x2E}, {"del", 0x2E}, {"backspace", 0x08},
            {"up", 0x26}, {"down", 0x28}, {"left", 0x25}, {"right", 0x27},
            {"home", 0x24}, {"end", 0x23}, {"pageup", 0x21}, {"pagedown", 0x22},
            {"f1", 0x70}, {"f2", 0x71}, {"f3", 0x72}, {"f4", 0x73},
            {"f5", 0x74}, {"f6", 0x75}, {"f7", 0x76}, {"f8", 0x77},
            {"f9", 0x78}, {"f10", 0x79}, {"f11", 0x7A}, {"f12", 0x7B},
        };
        
        auto it = keyMap.find(name);
        if (it != keyMap.end()) {
            return it->second;
        }
        
        // Single character
        if (name.length() == 1) {
            return toupper(name[0]);
        }
        
        return 0;
    }
    
    bool TakeScreenshot(const std::string& filename) {
        using namespace Gdiplus;

        // Initialise GDI+ for this call
        GdiplusStartupInput gpInput;
        ULONG_PTR           gpToken = 0;
        if (GdiplusStartup(&gpToken, &gpInput, NULL) != Ok) {
            std::cerr << "  Screenshot: GDI+ init failed\n";
            return false;
        }

        int w = GetSystemMetrics(SM_CXSCREEN);
        int h = GetSystemMetrics(SM_CYSCREEN);

        HDC     screenDC = GetDC(NULL);
        HDC     memDC    = CreateCompatibleDC(screenDC);
        HBITMAP hBmp     = CreateCompatibleBitmap(screenDC, w, h);
        HBITMAP hOld     = (HBITMAP)SelectObject(memDC, hBmp);
        BitBlt(memDC, 0, 0, w, h, screenDC, 0, 0, SRCCOPY);
        SelectObject(memDC, hOld);

        // Find PNG encoder CLSID
        UINT   num = 0, sz = 0;
        GetImageEncodersSize(&num, &sz);
        std::vector<BYTE> buf(sz);
        ImageCodecInfo* pInfo = reinterpret_cast<ImageCodecInfo*>(buf.data());
        GetImageEncoders(num, sz, pInfo);

        CLSID pngClsid = {};
        bool  found    = false;
        for (UINT i = 0; i < num; i++) {
            if (wcscmp(pInfo[i].MimeType, L"image/png") == 0) {
                pngClsid = pInfo[i].Clsid;
                found = true;
                break;
            }
        }

        bool ok = false;
        if (found) {
            Bitmap bmp(hBmp, NULL);
            std::wstring wf(filename.begin(), filename.end());
            ok = (bmp.Save(wf.c_str(), &pngClsid, NULL) == Ok);
        }

        DeleteDC(memDC);
        ReleaseDC(NULL, screenDC);
        DeleteObject(hBmp);
        GdiplusShutdown(gpToken);

        if (ok) {
            std::cout << "  Screenshot saved to: " << filename << "\n";
        } else {
            std::cerr << "  Screenshot failed: " << filename << "\n";
        }
        return ok;
    }
    
    // Interactive mode handlers
    void PrintHelp() {
        std::cout << "Commands:\n";
        std::cout << "  click <x> <y> [left|right|middle] - Click at coordinates\n";
        std::cout << "  move <x> <y>                     - Move mouse to coordinates\n";
        std::cout << "  key <keycode|name>               - Press and release key\n";
        std::cout << "  type <text>                       - Type text string\n";
        std::cout << "  wait <milliseconds>               - Wait specified time\n";
        std::cout << "  screenshot                        - Take screenshot\n";
        std::cout << "  help                             - Show this help\n";
        std::cout << "  exit                             - Exit interactive mode\n";
    }
    
    void HandleClickCommand(const std::string& args) {
        // Parse: "click 100 200 left"
        std::istringstream iss(args);
        int x, y;
        std::string button = "left";
        iss >> x >> y >> button;
        
        driverInterface_->InjectMouseMove(x, y, true);
        int btn = (button == "left") ? 1 : (button == "right") ? 2 : 3;
        driverInterface_->InjectMouseButton(btn, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        driverInterface_->InjectMouseButton(btn, false);
        
        std::cout << "Clicked " << button << " button at (" << x << ", " << y << ")\n";
    }
    
    void HandleMoveCommand(const std::string& args) {
        std::istringstream iss(args);
        int x, y;
        iss >> x >> y;
        
        driverInterface_->InjectMouseMove(x, y, true);
        std::cout << "Moved mouse to (" << x << ", " << y << ")\n";
    }
    
    void HandleKeyCommand(const std::string& key) {
        int code = KeyNameToCode(key);
        if (code > 0) {
            driverInterface_->InjectKeyDown(code, 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            driverInterface_->InjectKeyUp(code, 0);
            std::cout << "Pressed key: " << key << " (code: " << code << ")\n";
        } else {
            std::cout << "Unknown key: " << key << "\n";
        }
    }
    
    void HandleTypeCommand(const std::string& text) {
        for (char c : text) {
            driverInterface_->InjectKeyDown(c, 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            driverInterface_->InjectKeyUp(c, 0);
        }
        std::cout << "Typed: " << text << "\n";
    }
    
    void HandleWaitCommand(const std::string& ms) {
        int duration = std::stoi(ms);
        std::this_thread::sleep_for(std::chrono::milliseconds(duration));
        std::cout << "Waited " << duration << "ms\n";
    }
    
    // Smoke tests
    bool TestKeyboard() {
        bool ok = true;
        ok &= driverInterface_->InjectKeyDown('A', 0);
        ok &= driverInterface_->InjectKeyUp('A', 0);
        ok &= driverInterface_->InjectKeyDown(VK_RETURN, 0);
        ok &= driverInterface_->InjectKeyUp(VK_RETURN, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return ok;
    }
    
    bool TestMouse() {
        bool ok = true;
        ok &= driverInterface_->InjectMouseMove(100, 100, true);
        ok &= driverInterface_->InjectMouseButton(1, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        ok &= driverInterface_->InjectMouseButton(1, false);
        ok &= driverInterface_->InjectMouseMove(200, 200, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return ok;
    }
    
    bool TestController() {
        XUSB_REPORT report = {};
        report.bReportId     = 0x00;
        report.bSize         = sizeof(XUSB_REPORT);
        report.wButtons      = 0x0001;  // D-pad Up
        report.bLeftTrigger  = 128;
        report.bRightTrigger = 128;
        report.sThumbLX      = 16000;
        report.sThumbLY      = 0;
        report.sThumbRX      = 0;
        report.sThumbRY      = 0;
        bool ok = driverInterface_->InjectControllerReport(report);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        // Release
        report = {};
        report.bReportId = 0x00;
        report.bSize     = sizeof(XUSB_REPORT);
        ok &= driverInterface_->InjectControllerReport(report);
        return ok;
    }

    bool TestDisplay() {
        // Capture a screenshot and verify at least 1% of pixels are non-black
        std::string path = "display_test_capture.png";
        if (!TakeScreenshot(path)) return false;

        // Load the saved PNG and sample pixels using GDI+
        using namespace Gdiplus;
        GdiplusStartupInput gpIn; ULONG_PTR gpTok = 0;
        GdiplusStartup(&gpTok, &gpIn, NULL);

        std::wstring wPath(path.begin(), path.end());
        Bitmap* bmp = Bitmap::FromFile(wPath.c_str());
        bool ok = false;
        if (bmp && bmp->GetLastStatus() == Ok) {
            UINT w = bmp->GetWidth(), h = bmp->GetHeight();
            UINT nonBlack = 0, total = w * h;
            for (UINT y = 0; y < h; y += 10) {
                for (UINT x = 0; x < w; x += 10) {
                    Color c; bmp->GetPixel((INT)x, (INT)y, &c);
                    if (c.GetR() > 8 || c.GetG() > 8 || c.GetB() > 8) nonBlack++;
                }
            }
            double frac = (double)nonBlack / (double)((total + 99) / 100);
            ok = (frac >= 0.01);  // At least 1% non-black pixels
            std::cout << "    Display pixels non-black: "
                      << (int)(frac * 100) << "%\n";
        }
        delete bmp;
        GdiplusShutdown(gpTok);
        return ok;
    }

    // Pixel-diff comparison of two PNG files; returns true if RMSE <= tolerance
    static bool CompareImages(const std::string& ref,
                              const std::string& actual,
                              double tolerance) {
        using namespace Gdiplus;
        GdiplusStartupInput gpIn; ULONG_PTR gpTok = 0;
        GdiplusStartup(&gpTok, &gpIn, NULL);

        std::wstring wRef(ref.begin(), ref.end());
        std::wstring wAct(actual.begin(), actual.end());
        Bitmap* bRef = Bitmap::FromFile(wRef.c_str());
        Bitmap* bAct = Bitmap::FromFile(wAct.c_str());

        bool match = false;
        if (bRef && bAct && bRef->GetLastStatus() == Ok && bAct->GetLastStatus() == Ok) {
            UINT w = bRef->GetWidth(), h = bRef->GetHeight();
            double sumSq = 0; UINT count = 0;
            for (UINT y = 0; y < h; y += 4) {
                for (UINT x = 0; x < w; x += 4) {
                    UINT ax = x * bAct->GetWidth()  / w;
                    UINT ay = y * bAct->GetHeight() / h;
                    Color cr, ca;
                    bRef->GetPixel((INT)x, (INT)y, &cr);
                    bAct->GetPixel((INT)ax, (INT)ay, &ca);
                    auto d = [](int a, int b) { return (double)(a-b)*(a-b); };
                    sumSq += d(cr.GetR(), ca.GetR()) +
                             d(cr.GetG(), ca.GetG()) +
                             d(cr.GetB(), ca.GetB());
                    count++;
                }
            }
            double rmse = (count > 0) ? std::sqrt(sumSq / (count * 3 * 255.0 * 255.0)) : 1.0;
            match = (rmse <= tolerance);
            std::cout << "    Image RMSE: " << rmse << " (tol=" << tolerance << ")\n";
        } else {
            std::cerr << "    CompareImages: failed to load PNG files\n";
        }
        delete bRef; delete bAct;
        GdiplusShutdown(gpTok);
        return match;
    }
    
    bool TestPerformance() {
        auto start = std::chrono::steady_clock::now();
        
        // Send 1000 inputs as fast as possible
        for (int i = 0; i < 100; i++) {
            driverInterface_->InjectKeyDown('A' + (i % 26), 0);
            driverInterface_->InjectKeyUp('A' + (i % 26), 0);
        }
        
        auto end = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        
        double avgLatency = duration.count() / 100.0;
        std::cout << "    Average input latency: " << avgLatency << "ms\n";
        
        return avgLatency < 50.0;  // Should be under 50ms
    }
};

// Main entry point
int main(int argc, char* argv[]) {
    std::cout << "========================================\n";
    std::cout << "KVM-Drivers Local Automation\n";
    std::cout << "Direct driver interface (no network)\n";
    std::cout << "========================================\n\n";
    
    LocalAutomation automation;
    
    if (!automation.Initialize()) {
        std::cerr << "Failed to initialize automation\n";
        return 1;
    }
    
    // Parse command line
    if (argc > 1) {
        std::string arg = argv[1];
        
        if (arg == "--script" && argc > 2) {
            // Run test script
            bool success = automation.RunTestScript(argv[2]);
            return success ? 0 : 1;
            
        } else if (arg == "--smoke") {
            // Run smoke test
            bool success = automation.RunSmokeTest();
            return success ? 0 : 1;
            
        } else if (arg == "--interactive" || arg == "-i") {
            // Interactive mode
            automation.RunInteractive();
            return 0;
            
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage:\n";
            std::cout << "  local_automation.exe --script <test.yaml>   Run test script\n";
            std::cout << "  local_automation.exe --smoke               Run smoke test\n";
            std::cout << "  local_automation.exe --interactive         Interactive REPL mode\n";
            std::cout << "  local_automation.exe --help                Show this help\n";
            return 0;
        }
    }
    
    // Default: interactive mode
    automation.RunInteractive();
    
    return 0;
}
