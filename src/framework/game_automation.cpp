// game_automation.cpp - Game Testing Extensions Implementation

#include "game_automation.h"
#include <shellapi.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <opencv2/opencv.hpp>
#include <tesseract/baseapi.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "uiautomationcore.lib")

namespace KVMDrivers {
namespace Automation {
namespace GameTesting {

// ==================== Application Launcher Implementation ====================

ApplicationLauncher::ApplicationLauncher() {}
ApplicationLauncher::~ApplicationLauncher() {}

bool ApplicationLauncher::ValidateParameters(const std::map<std::string, std::string>& params,
    std::string& errorMessage) const {
    if (params.find("executable") == params.end()) {
        errorMessage = "Missing 'executable' parameter";
        return false;
    }
    return true;
}

ActionResult ApplicationLauncher::Execute(const TestStep& step, AutomationContext& context) {
    LaunchConfig config;
    
    auto it = step.parameters.find("executable");
    if (it == step.parameters.end()) {
        return ActionResult::Fail("Missing executable parameter");
    }
    config.executablePath = context.InterpolateVariables(it->second);
    
    auto jt = step.parameters.find("working_directory");
    if (jt != step.parameters.end()) {
        config.workingDirectory = context.InterpolateVariables(jt->second);
    }
    
    auto kt = step.parameters.find("args");
    if (kt != step.parameters.end()) {
        // Parse comma-separated arguments
        std::stringstream ss(kt->second);
        std::string arg;
        while (std::getline(ss, arg, ',')) {
            config.arguments.push_back(arg);
        }
    }
    
    auto lt = step.parameters.find("window_title");
    if (lt != step.parameters.end()) {
        config.windowTitle = context.InterpolateVariables(lt->second);
    }
    
    auto mt = step.parameters.find("timeout");
    if (mt != step.parameters.end()) {
        config.launchTimeoutMs = std::stoi(mt->second);
    }
    
    auto nt = step.parameters.find("maximize");
    if (nt != step.parameters.end()) {
        config.maximizeWindow = (nt->second == "true");
    }
    
    context.LogInfo("Launching: " + config.executablePath);
    
    ProcessInfo info = Launch(config);
    
    if (info.isRunning) {
        std::string msg = "Launched successfully (PID: " + std::to_string(info.processId) + 
            ", HWND: " + std::to_string((ULONG_PTR)info.windowHandle) + ")";
        return ActionResult::Ok(msg);
    } else {
        return ActionResult::Fail("Failed to launch application");
    }
}

ProcessInfo ApplicationLauncher::Launch(const LaunchConfig& config) {
    ProcessInfo info = {};
    info.isRunning = false;
    info.processId = 0;
    info.windowHandle = NULL;
    
    // Build command line
    std::string cmdLine = "\"" + config.executablePath + "\"";
    for (const auto& arg : config.arguments) {
        cmdLine += " \"" + arg + "\"";
    }
    
    // Prepare startup info
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    
    // Set working directory
    const char* workingDir = config.workingDirectory.empty() ? NULL : config.workingDirectory.c_str();
    
    // Launch process
    BOOL success = CreateProcessA(
        NULL,                           // Application name (use command line)
        (LPSTR)cmdLine.c_str(),         // Command line
        NULL,                           // Process security attributes
        NULL,                           // Thread security attributes
        FALSE,                          // Inherit handles
        CREATE_NEW_CONSOLE |           // Create new console
        CREATE_DEFAULT_ERROR_MODE,
        NULL,                           // Environment
        workingDir,                     // Working directory
        &si,                            // Startup info
        &pi                             // Process info
    );
    
    if (!success) {
        return info;
    }
    
    info.processId = pi.dwProcessId;
    info.isRunning = true;
    
    // Close thread handle, keep process handle for monitoring
    CloseHandle(pi.hThread);
    
    // Wait for window if requested
    if (config.waitForWindow) {
        info = WaitForWindow(config, info);
    }
    
    // Apply window settings
    if (info.windowHandle != NULL) {
        if (config.setForeground) {
            BringToForeground(info.windowHandle);
        }
        if (config.maximizeWindow) {
            MaximizeWindow(info.windowHandle);
        }
    }
    
    CloseHandle(pi.hProcess);
    return info;
}

bool ApplicationLauncher::WaitForWindow(const LaunchConfig& config, ProcessInfo& info) {
    DWORD startTime = GetTickCount();
    
    while (GetTickCount() - startTime < (DWORD)config.launchTimeoutMs) {
        // Find window by title pattern
        if (!config.windowTitle.empty()) {
            info = FindWindowByTitle(config.windowTitle);
            if (info.windowHandle != NULL && info.processId == info.processId) {
                return true;
            }
        }
        
        // Find window by process ID
        info = FindWindowByProcessId(info.processId);
        if (info.windowHandle != NULL) {
            return true;
        }
        
        Sleep(100);
    }
    
    return false;
}

ProcessInfo ApplicationLauncher::FindWindowByTitle(const std::string& titlePattern) {
    ProcessInfo info = {};
    
    struct EnumData {
        std::string pattern;
        ProcessInfo result;
    };
    
    EnumData data;
    data.pattern = titlePattern;
    data.result = {};
    
    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        EnumData* data = (EnumData*)lParam;
        
        // Check if window is visible
        if (!IsWindowVisible(hwnd)) return TRUE;
        
        // Get window text
        char title[256];
        GetWindowTextA(hwnd, title, sizeof(title));
        
        // Check if title matches pattern (simple substring match)
        if (strstr(title, data->pattern.c_str()) != NULL) {
            // Get process ID
            DWORD pid;
            GetWindowThreadProcessId(hwnd, &pid);
            
            data->result.windowHandle = hwnd;
            data->result.windowTitle = title;
            data->result.processId = pid;
            data->result.isRunning = true;
            GetWindowRect(hwnd, &data->result.windowRect);
            
            return FALSE;  // Stop enumeration
        }
        
        return TRUE;
    }, (LPARAM)&data);
    
    return data.result;
}

ProcessInfo ApplicationLauncher::FindWindowByProcessId(DWORD processId) {
    ProcessInfo info = {};
    
    struct EnumData {
        DWORD targetPid;
        ProcessInfo result;
    };
    
    EnumData data;
    data.targetPid = processId;
    data.result = {};
    
    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        EnumData* data = (EnumData*)lParam;
        
        if (!IsWindowVisible(hwnd)) return TRUE;
        
        DWORD pid;
        GetWindowThreadProcessId(hwnd, &pid);
        
        if (pid == data->targetPid) {
            char title[256];
            GetWindowTextA(hwnd, title, sizeof(title));
            
            data->result.windowHandle = hwnd;
            data->result.windowTitle = title;
            data->result.processId = pid;
            data->result.isRunning = true;
            GetWindowRect(hwnd, &data->result.windowRect);
            
            return FALSE;
        }
        
        return TRUE;
    }, (LPARAM)&data);
    
    return data.result;
}

bool ApplicationLauncher::Terminate(DWORD processId, int exitCode) {
    HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, processId);
    if (hProcess == NULL) return false;
    
    BOOL result = TerminateProcess(hProcess, exitCode);
    CloseHandle(hProcess);
    return result == TRUE;
}

bool ApplicationLauncher::IsRunning(DWORD processId) {
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, processId);
    if (hProcess == NULL) return false;
    
    DWORD exitCode;
    BOOL result = GetExitCodeProcess(hProcess, &exitCode);
    CloseHandle(hProcess);
    
    return result == TRUE && exitCode == STILL_ACTIVE;
}

bool ApplicationLauncher::BringToForeground(HWND hwnd) {
    if (!IsWindow(hwnd)) return false;
    
    // Show window if minimized
    if (IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    }
    
    // Bring to front
    SetForegroundWindow(hwnd);
    return true;
}

bool ApplicationLauncher::MaximizeWindow(HWND hwnd) {
    return ShowWindow(hwnd, SW_MAXIMIZE) == TRUE;
}

bool ApplicationLauncher::SetWindowPosition(HWND hwnd, int x, int y, int width, int height) {
    return SetWindowPos(hwnd, NULL, x, y, width, height, 
        SWP_NOZORDER | SWP_NOACTIVATE) == TRUE;
}

// ==================== Screen Analyzer Implementation ====================

ScreenAnalyzer::ScreenAnalyzer() {}
ScreenAnalyzer::~ScreenAnalyzer() {}

void ScreenAnalyzer::SetReferenceDirectory(const std::string& dir) {
    referenceDir_ = dir;
}

bool ScreenAnalyzer::ValidateParameters(const std::map<std::string, std::string>& params,
    std::string& errorMessage) const {
    auto it = params.find("method");
    if (it == params.end()) {
        errorMessage = "Missing 'method' parameter (template, text, color)";
        return false;
    }
    return true;
}

ActionResult ScreenAnalyzer::Execute(const TestStep& step, AutomationContext& context) {
    auto method = step.parameters.find("method");
    if (method == step.parameters.end()) {
        return ActionResult::Fail("Missing method parameter");
    }
    
    SearchRegion region;
    auto regionParam = step.parameters.find("region");
    if (regionParam != step.parameters.end()) {
        // Parse region: "x,y,w,h"
        int x, y, w, h;
        sscanf(regionParam->second.c_str(), "%d,%d,%d,%d", &x, &y, &w, &h);
        region = {x, y, w, h, false};
    }
    
    if (method->second == "template") {
        auto templateParam = step.parameters.find("template");
        if (templateParam == step.parameters.end()) {
            return ActionResult::Fail("Missing template parameter");
        }
        
        ImageTemplate tmpl;
        tmpl.imagePath = referenceDir_ + "/" + templateParam->second;
        tmpl.similarityThreshold = 0.8;
        
        auto threshold = step.parameters.find("threshold");
        if (threshold != step.parameters.end()) {
            tmpl.similarityThreshold = std::stod(threshold->second);
        }
        
        auto found = FindFirstTemplate(tmpl, region);
        if (found.confidence > 0) {
            context.variables["found_x"] = std::to_string(found.CenterX());
            context.variables["found_y"] = std::to_string(found.CenterY());
            return ActionResult::Ok("Found template at (" + std::to_string(found.CenterX()) + 
                ", " + std::to_string(found.CenterY()) + ") with confidence " + 
                std::to_string(found.confidence));
        } else {
            return ActionResult::Fail("Template not found");
        }
        
    } else if (method->second == "text") {
        auto text = step.parameters.find("text");
        if (text == step.parameters.end()) {
            return ActionResult::Fail("Missing text parameter");
        }
        
        auto found = FindFirstText(text->second, region);
        if (found.confidence > 0) {
            context.variables["found_x"] = std::to_string(found.CenterX());
            context.variables["found_y"] = std::to_string(found.CenterY());
            return ActionResult::Ok("Found text at (" + std::to_string(found.CenterX()) + 
                ", " + std::to_string(found.CenterY()) + ")");
        } else {
            return ActionResult::Fail("Text not found");
        }
        
    } else if (method->second == "color") {
        auto color = step.parameters.find("color");
        if (color == step.parameters.end()) {
            return ActionResult::Fail("Missing color parameter");
        }
        
        auto found = FindColor(color->second, 10, region);
        if (!found.empty()) {
            context.variables["found_x"] = std::to_string(found[0].CenterX());
            context.variables["found_y"] = std::to_string(found[0].CenterY());
            return ActionResult::Ok("Found color at (" + std::to_string(found[0].CenterX()) + 
                ", " + std::to_string(found[0].CenterY()) + ")");
        } else {
            return ActionResult::Fail("Color not found");
        }
    }
    
    return ActionResult::Fail("Unknown method: " + method->second);
}

std::vector<FoundObject> ScreenAnalyzer::FindTemplate(const ImageTemplate& template_, 
    const SearchRegion& region) {
    std::vector<FoundObject> results;
    
    // Capture screen or region
    std::string tempFile = referenceDir_ + "/_temp_capture.png";
    if (region.fullScreen || region.width == 0) {
        CaptureScreenToFile(tempFile);
    } else {
        CaptureRegionToFile(region.x, region.y, region.width, region.height, tempFile);
    }
    
    // Use OpenCV for template matching
    cv::Mat screen = cv::imread(tempFile);
    cv::Mat tmpl = cv::imread(template_.imagePath);
    
    if (screen.empty() || tmpl.empty()) {
        return results;
    }
    
    cv::Mat result;
    cv::matchTemplate(screen, tmpl, result, cv::TM_CCOEFF_NORMED);
    
    double minVal, maxVal;
    cv::Point minLoc, maxLoc;
    cv::minMaxLoc(result, &minVal, &maxVal, &minLoc, &maxLoc);
    
    if (maxVal >= template_.similarityThreshold) {
        FoundObject obj;
        obj.name = template_.name;
        obj.x = region.fullScreen ? maxLoc.x : region.x + maxLoc.x;
        obj.y = region.fullScreen ? maxLoc.y : region.y + maxLoc.y;
        obj.width = tmpl.cols;
        obj.height = tmpl.rows;
        obj.confidence = maxVal;
        obj.method = FindMethod::TemplateMatch;
        results.push_back(obj);
    }
    
    return results;
}

FoundObject ScreenAnalyzer::FindFirstTemplate(const ImageTemplate& template_, 
    const SearchRegion& region) {
    auto results = FindTemplate(template_, region);
    if (!results.empty()) {
        return results[0];
    }
    return {};
}

std::vector<FoundObject> ScreenAnalyzer::FindText(const std::string& text, 
    const SearchRegion& region) {
    std::vector<FoundObject> results;
    
    // Capture screen
    std::string tempFile = referenceDir_ + "/_temp_capture.png";
    if (region.fullScreen || region.width == 0) {
        CaptureScreenToFile(tempFile);
    } else {
        CaptureRegionToFile(region.x, region.y, region.width, region.height, tempFile);
    }
    
    // Use Tesseract OCR
    tesseract::TessBaseAPI tess;
    if (tess.Init(NULL, "eng") != 0) {
        return results;
    }
    
    cv::Mat img = cv::imread(tempFile, cv::IMREAD_GRAYSCALE);
    tess.SetImage(img.data, img.cols, img.rows, 1, img.step);
    
    // Get text and bounding boxes
    // Note: Full implementation would use Tesseract's iterator to get word positions
    char* utf8Text = tess.GetUTF8Text();
    std::string recognized(utf8Text);
    delete[] utf8Text;
    
    // Simple string search (real implementation would use position data)
    size_t pos = recognized.find(text);
    if (pos != std::string::npos) {
        FoundObject obj;
        obj.name = text;
        obj.confidence = 0.8;  // Estimated
        obj.method = FindMethod::OCR;
        // Position would come from Tesseract's bounding box data
        results.push_back(obj);
    }
    
    tess.End();
    return results;
}

std::string ScreenAnalyzer::ReadTextAt(const SearchRegion& region) {
    // Capture region
    std::string tempFile = referenceDir_ + "/_temp_capture.png";
    CaptureRegionToFile(region.x, region.y, region.width, region.height, tempFile);
    
    // OCR
    tesseract::TessBaseAPI tess;
    if (tess.Init(NULL, "eng") != 0) {
        return "";
    }
    
    cv::Mat img = cv::imread(tempFile, cv::IMREAD_GRAYSCALE);
    tess.SetImage(img.data, img.cols, img.rows, 1, img.step);
    
    char* text = tess.GetUTF8Text();
    std::string result(text);
    delete[] text;
    
    tess.End();
    return result;
}

std::vector<FoundObject> ScreenAnalyzer::FindColor(const std::string& colorHex, 
    int tolerance, const SearchRegion& region) {
    std::vector<FoundObject> results;
    
    // Parse hex color
    int r, g, b;
    sscanf(colorHex.c_str(), "%02x%02x%02x", &r, &g, &b);
    
    // Capture
    std::string tempFile = referenceDir_ + "/_temp_capture.png";
    if (region.fullScreen || region.width == 0) {
        CaptureScreenToFile(tempFile);
    } else {
        CaptureRegionToFile(region.x, region.y, region.width, region.height, tempFile);
    }
    
    cv::Mat img = cv::imread(tempFile);
    if (img.empty()) return results;
    
    // Create mask for color range
    cv::Scalar lower(std::max(0, b - tolerance), std::max(0, g - tolerance), 
        std::max(0, r - tolerance));
    cv::Scalar upper(std::min(255, b + tolerance), std::min(255, g + tolerance), 
        std::min(255, r + tolerance));
    
    cv::Mat mask;
    cv::inRange(img, lower, upper, mask);
    
    // Find contours
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
    
    for (const auto& contour : contours) {
        cv::Rect rect = cv::boundingRect(contour);
        if (rect.area() > 100) {  // Minimum size
            FoundObject obj;
            obj.x = region.fullScreen ? rect.x : region.x + rect.x;
            obj.y = region.fullScreen ? rect.y : region.y + rect.y;
            obj.width = rect.width;
            obj.height = rect.height;
            obj.confidence = 1.0;
            obj.method = FindMethod::ColorDetection;
            results.push_back(obj);
        }
    }
    
    return results;
}

bool ScreenAnalyzer::CaptureScreenToFile(const std::string& filepath) {
    // Get screen dimensions
    int screenX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int screenY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int screenWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    
    return CaptureRegionToFile(screenX, screenY, screenWidth, screenHeight, filepath);
}

bool ScreenAnalyzer::CaptureRegionToFile(int x, int y, int w, int h, 
    const std::string& filepath) {
    HDC hScreen = GetDC(NULL);
    HDC hDC = CreateCompatibleDC(hScreen);
    HBITMAP hBitmap = CreateCompatibleBitmap(hScreen, w, h);
    SelectObject(hDC, hBitmap);
    
    BitBlt(hDC, 0, 0, w, h, hScreen, x, y, SRCCOPY);
    
    // Save to file
    BITMAPINFOHEADER bmi = {};
    bmi.biSize = sizeof(BITMAPINFOHEADER);
    bmi.biWidth = w;
    bmi.biHeight = -h;  // Top-down
    bmi.biPlanes = 1;
    bmi.biBitCount = 24;
    bmi.biCompression = BI_RGB;
    
    std::vector<BYTE> buffer(w * h * 3);
    GetDIBits(hDC, hBitmap, 0, h, buffer.data(), (BITMAPINFO*)&bmi, DIB_RGB_COLORS);
    
    // Write BMP
    std::ofstream file(filepath, std::ios::binary);
    if (!file) return false;
    
    BITMAPFILEHEADER bmfh = {};
    bmfh.bfType = 'MB';
    bmfh.bfSize = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + buffer.size();
    bmfh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    
    file.write((char*)&bmfh, sizeof(bmfh));
    file.write((char*)&bmi, sizeof(bmi));
    file.write((char*)buffer.data(), buffer.size());
    
    DeleteObject(hBitmap);
    DeleteDC(hDC);
    ReleaseDC(NULL, hScreen);
    
    return true;
}

// ==================== Smart Click Handler ====================

bool SmartClickHandler::ValidateParameters(const std::map<std::string, std::string>& params,
    std::string& errorMessage) const {
    // Must have one of: accessibility_name, template, x/y, text
    bool hasAccessibility = params.find("accessibility_name") != params.end();
    bool hasTemplate = params.find("template") != params.end();
    bool hasCoordinates = params.find("x") != params.end() && params.find("y") != params.end();
    bool hasText = params.find("text") != params.end();
    
    if (!hasAccessibility && !hasTemplate && !hasCoordinates && !hasText) {
        errorMessage = "Must specify one of: accessibility_name, template, x/y coordinates, or text";
        return false;
    }
    return true;
}

ActionResult SmartClickHandler::Execute(const TestStep& step, AutomationContext& context) {
    // Try methods in order: accessibility -> template -> text -> coordinates
    
    auto acc = step.parameters.find("accessibility_name");
    if (acc != step.parameters.end()) {
        // Use UI automation
        UIAutomationProvider provider;
        provider.Initialize();
        
        UIElementCriteria criteria;
        criteria.name = acc->second;
        
        auto element = provider.FindFirstElement(criteria);
        if (element.nativeHandle != nullptr) {
            provider.ClickElement(element);
            return ActionResult::Ok("Clicked element by accessibility: " + acc->second);
        }
    }
    
    auto tmpl = step.parameters.find("template");
    if (tmpl != step.parameters.end()) {
        // Use screen analyzer
        ScreenAnalyzer analyzer;
        ImageTemplate itmpl;
        itmpl.imagePath = tmpl->second;
        
        auto found = analyzer.FindFirstTemplate(itmpl);
        if (found.confidence > 0) {
            context.driverInterface->InjectMouseMove(found.CenterX(), found.CenterY(), true);
            context.driverInterface->InjectMouseButton(1, true);
            Sleep(50);
            context.driverInterface->InjectMouseButton(1, false);
            return ActionResult::Ok("Clicked by template: " + tmpl->second);
        }
    }
    
    auto text = step.parameters.find("text");
    if (text != step.parameters.end()) {
        ScreenAnalyzer analyzer;
        auto found = analyzer.FindFirstText(text->second);
        if (found.confidence > 0) {
            context.driverInterface->InjectMouseMove(found.CenterX(), found.CenterY(), true);
            context.driverInterface->InjectMouseButton(1, true);
            Sleep(50);
            context.driverInterface->InjectMouseButton(1, false);
            return ActionResult::Ok("Clicked by text: " + text->second);
        }
    }
    
    auto x = step.parameters.find("x");
    auto y = step.parameters.find("y");
    if (x != step.parameters.end() && y != step.parameters.end()) {
        int ix = std::stoi(context.InterpolateVariables(x->second));
        int iy = std::stoi(context.InterpolateVariables(y->second));
        context.driverInterface->InjectMouseMove(ix, iy, true);
        context.driverInterface->InjectMouseButton(1, true);
        Sleep(50);
        context.driverInterface->InjectMouseButton(1, false);
        return ActionResult::Ok("Clicked at coordinates (" + std::to_string(ix) + ", " + std::to_string(iy) + ")");
    }
    
    return ActionResult::Fail("Failed to find element to click");
}

} // namespace GameTesting
} // namespace Automation
} // namespace KVMDrivers
