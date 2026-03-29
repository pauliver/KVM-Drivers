// screen_capture.cpp - Screen capture and image comparison implementation

#include "screen_capture.h"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <json/json.h>
#include <gdiplus.h>

#pragma comment(lib, "gdiplus.lib")

namespace KVMDrivers {
namespace Automation {

// ==================== ScreenCapture Implementation ====================

ScreenCapture::ScreenCapture() : screenDC_(nullptr), memoryDC_(nullptr) {
    InitializeDC();
}

ScreenCapture::~ScreenCapture() {
    CleanupDC();
}

bool ScreenCapture::InitializeDC() {
    screenDC_ = GetDC(nullptr);
    memoryDC_ = CreateCompatibleDC(screenDC_);
    return screenDC_ != nullptr && memoryDC_ != nullptr;
}

void ScreenCapture::CleanupDC() {
    if (memoryDC_) {
        DeleteDC(memoryDC_);
        memoryDC_ = nullptr;
    }
    if (screenDC_) {
        ReleaseDC(nullptr, screenDC_);
        screenDC_ = nullptr;
    }
}

bool ScreenCapture::CaptureToFile(const std::string& filepath, const CaptureOptions& options) {
    cv::Mat image = CaptureToMat(options);
    if (image.empty()) return false;
    return SaveImage(filepath, image);
}

cv::Mat ScreenCapture::CaptureToMat(const CaptureOptions& options) {
    int x = options.x;
    int y = options.y;
    int width = options.width;
    int height = options.height;
    
    if (options.fullScreen || width == 0 || height == 0) {
        x = GetSystemMetrics(SM_XVIRTUALSCREEN);
        y = GetSystemMetrics(SM_YVIRTUALSCREEN);
        width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    }
    
    // Create bitmap
    HBITMAP hBitmap = CreateCompatibleBitmap(screenDC_, width, height);
    if (!hBitmap) return cv::Mat();
    
    HBITMAP hOldBitmap = (HBITMAP)SelectObject(memoryDC_, hBitmap);
    
    // Copy screen to bitmap
    BitBlt(memoryDC_, 0, 0, width, height, screenDC_, x, y, SRCCOPY);
    
    // Capture cursor if requested
    if (options.captureCursor) {
        CURSORINFO cursorInfo = { sizeof(CURSORINFO) };
        if (GetCursorInfo(&cursorInfo) && cursorInfo.flags == CURSOR_SHOWING) {
            ICONINFO iconInfo;
            if (GetIconInfo(cursorInfo.hCursor, &iconInfo)) {
                int cursorX = cursorInfo.ptScreenPos.x - x - (int)iconInfo.xHotspot;
                int cursorY = cursorInfo.ptScreenPos.y - y - (int)iconInfo.yHotspot;
                DrawIcon(memoryDC_, cursorX, cursorY, cursorInfo.hCursor);
                DeleteObject(iconInfo.hbmMask);
                DeleteObject(iconInfo.hbmColor);
            }
        }
    }
    
    // Get bitmap bits
    BITMAPINFOHEADER bmi = {};
    bmi.biSize = sizeof(BITMAPINFOHEADER);
    bmi.biWidth = width;
    bmi.biHeight = -height;  // Top-down
    bmi.biPlanes = 1;
    bmi.biBitCount = 32;
    bmi.biCompression = BI_RGB;
    
    cv::Mat image(height, width, CV_8UC4);  // BGRA
    GetDIBits(memoryDC_, hBitmap, 0, height, image.data, (BITMAPINFO*)&bmi, DIB_RGB_COLORS);
    
    // Convert BGRA to BGR (drop alpha)
    cv::Mat result(height, width, CV_8UC3);
    cv::cvtColor(image, result, cv::COLOR_BGRA2BGR);
    
    // Cleanup
    SelectObject(memoryDC_, hOldBitmap);
    DeleteObject(hBitmap);
    
    return result;
}

bool ScreenCapture::CaptureWindow(HWND hwnd, const std::string& filepath) {
    cv::Mat image = CaptureWindow(hwnd);
    if (image.empty()) return false;
    return SaveImage(filepath, image);
}

cv::Mat ScreenCapture::CaptureWindow(HWND hwnd) {
    if (!IsWindow(hwnd)) return cv::Mat();
    
    RECT rect;
    if (!GetWindowRect(hwnd, &rect)) return cv::Mat();
    
    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;
    
    CaptureOptions options;
    options.fullScreen = false;
    options.x = rect.left;
    options.y = rect.top;
    options.width = width;
    options.height = height;
    
    return CaptureToMat(options);
}

ComparisonResult ScreenCapture::CompareImages(const cv::Mat& reference, 
    const cv::Mat& actual, const ComparisonOptions& options) {
    ComparisonResult result;
    
    // Validate images
    if (reference.empty() || actual.empty()) {
        result.message = "One or both images are empty";
        return result;
    }
    
    // Ensure same size
    cv::Mat refResized, actResized;
    if (reference.size() != actual.size()) {
        cv::resize(actual, actResized, reference.size());
    } else {
        actResized = actual;
    }
    refResized = reference;
    
    // Convert to same format
    if (refResized.channels() == 4) {
        cv::cvtColor(refResized, refResized, cv::COLOR_BGRA2BGR);
    }
    if (actResized.channels() == 4) {
        cv::cvtColor(actResized, actResized, cv::COLOR_BGRA2BGR);
    }
    
    // Compute difference
    cv::Mat diff;
    cv::absdiff(refResized, actResized, diff);
    
    // Apply tolerance
    cv::Mat mask;
    double threshold = options.pixelTolerance * 255.0;
    cv::threshold(diff, mask, threshold, 255, cv::THRESH_BINARY);
    
    // Count different pixels
    result.diffPixelCount = cv::countNonZero(mask);
    int totalPixels = refResized.rows * refResized.cols;
    result.diffPercentage = (result.diffPixelCount * 100.0) / totalPixels;
    
    // Compute similarity (SSIM)
    cv::Mat refGray, actGray;
    cv::cvtColor(refResized, refGray, cv::COLOR_BGR2GRAY);
    cv::cvtColor(actResized, actGray, cv::COLOR_BGR2GRAY);
    
    cv::Mat ssimMap;
    cv::Mat mu1, mu2, sigma1, sigma2, sigma12;
    
    const double C1 = 6.5025, C2 = 58.5225;
    cv::GaussianBlur(refGray, mu1, cv::Size(11, 11), 1.5);
    cv::GaussianBlur(actGray, mu2, cv::Size(11, 11), 1.5);
    
    cv::Mat mu1_2 = mu1.mul(mu1);
    cv::Mat mu2_2 = mu2.mul(mu2);
    cv::Mat mu1_mu2 = mu1.mul(mu2);
    
    cv::GaussianBlur(refGray.mul(refGray), sigma1, cv::Size(11, 11), 1.5);
    cv::GaussianBlur(actGray.mul(actGray), sigma2, cv::Size(11, 11), 1.5);
    cv::GaussianBlur(refGray.mul(actGray), sigma12, cv::Size(11, 11), 1.5);
    
    cv::Mat t1 = 2 * mu1_mu2 + C1;
    cv::Mat t2 = 2 * sigma12 + C2;
    cv::Mat t3 = t1.mul(t2);
    
    t1 = mu1_2 + mu2_2 + C1;
    t2 = sigma1 + sigma2 + C2;
    t1 = t1.mul(t2);
    
    cv::Mat ssimMat;
    cv::divide(t3, t1, ssimMat);
    cv::Scalar mssim = cv::mean(ssimMat);
    result.similarity = mssim[0];
    
    // Determine match
    result.match = result.similarity >= options.similarityThreshold;
    
    // Create diff visualization
    result.diffImage = CreateDiffVisualization(refResized, actResized, result);
    
    // Build message
    std::stringstream msg;
    msg << std::fixed << std::setprecision(2);
    msg << "Similarity: " << (result.similarity * 100.0) << "%, ";
    msg << "Diff pixels: " << result.diffPixelCount << " (" << result.diffPercentage << "%)";
    result.message = msg.str();
    
    return result;
}

ComparisonResult ScreenCapture::CompareFiles(const std::string& referencePath,
    const std::string& actualPath, const ComparisonOptions& options) {
    cv::Mat reference, actual;
    
    if (!LoadImage(referencePath, reference)) {
        ComparisonResult result;
        result.message = "Failed to load reference image: " + referencePath;
        return result;
    }
    
    if (!LoadImage(actualPath, actual)) {
        ComparisonResult result;
        result.message = "Failed to load actual image: " + actualPath;
        return result;
    }
    
    return CompareImages(reference, actual, options);
}

bool ScreenCapture::FindImage(const cv::Mat& screen, const cv::Mat& templateImg,
    double threshold, cv::Point& location, double& confidence) {
    if (screen.empty() || templateImg.empty()) return false;
    if (templateImg.cols > screen.cols || templateImg.rows > screen.rows) return false;
    
    cv::Mat result;
    cv::matchTemplate(screen, templateImg, result, cv::TM_CCOEFF_NORMED);
    
    double minVal, maxVal;
    cv::Point minLoc, maxLoc;
    cv::minMaxLoc(result, &minVal, &maxVal, &minLoc, &maxLoc);
    
    confidence = maxVal;
    location = maxLoc;
    
    return maxVal >= threshold;
}

cv::Mat ScreenCapture::CreateDiffVisualization(const cv::Mat& reference,
    const cv::Mat& actual, const ComparisonResult& result) {
    cv::Mat diff;
    cv::absdiff(reference, actual, diff);
    
    // Threshold to create mask
    cv::Mat mask;
    cv::threshold(diff, mask, 30, 255, cv::THRESH_BINARY);
    
    // Create red overlay for differences
    cv::Mat redOverlay(actual.size(), actual.type(), cv::Scalar(0, 0, 255));
    
    // Blend actual with red where differences exist
    cv::Mat blended;
    double alpha = 0.7;
    cv::addWeighted(actual, alpha, redOverlay, 1.0 - alpha, 0.0, blended, -1);
    
    // Copy blended only where mask is non-zero
    cv::Mat output = actual.clone();
    blended.copyTo(output, mask);
    
    return output;
}

bool ScreenCapture::LoadImage(const std::string& path, cv::Mat& image) {
    image = cv::imread(path, cv::IMREAD_UNCHANGED);
    return !image.empty();
}

bool ScreenCapture::SaveImage(const std::string& path, const cv::Mat& image) {
    if (image.empty()) return false;
    
    // Ensure directory exists
    size_t lastSlash = path.find_last_of("/\\");
    if (lastSlash != std::string::npos) {
        std::string dir = path.substr(0, lastSlash);
        CreateDirectoryA(dir.c_str(), nullptr);
    }
    
    return cv::imwrite(path, image);
}

cv::Mat ScreenCapture::ResizeImage(const cv::Mat& image, int width, int height) {
    if (image.empty()) return cv::Mat();
    cv::Mat resized;
    cv::resize(image, resized, cv::Size(width, height));
    return resized;
}

// ==================== ScreenComparisonHandler Implementation ====================

bool ScreenComparisonHandler::ValidateParameters(const std::map<std::string, std::string>& params,
    std::string& errorMessage) const {
    auto it = params.find("reference");
    if (it == params.end()) {
        errorMessage = "Missing 'reference' parameter (path to reference image)";
        return false;
    }
    return true;
}

ActionResult ScreenComparisonHandler::Execute(const TestStep& step, AutomationContext& context) {
    auto refIt = step.parameters.find("reference");
    if (refIt == step.parameters.end()) {
        return ActionResult::Fail("Missing reference parameter");
    }
    
    std::string referencePath = GetReferenceDir(context) + "/" + refIt->second;
    
    // Get actual image path or capture screen
    std::string actualPath;
    auto actualIt = step.parameters.find("actual");
    
    if (actualIt != step.parameters.end()) {
        actualPath = GetReferenceDir(context) + "/" + actualIt->second;
    } else {
        // Capture current screen
        actualPath = context.currentTestConfig->screenshotDir + "/capture_" + 
            Utils::GetTimestamp() + ".png";
        
        ScreenCapture capture;
        CaptureOptions options;
        options.fullScreen = true;
        
        if (!capture.CaptureToFile(actualPath, options)) {
            return ActionResult::Fail("Failed to capture screen");
        }
    }
    
    // Parse options
    ComparisonOptions compOptions;
    auto thresholdIt = step.parameters.find("threshold");
    if (thresholdIt != step.parameters.end()) {
        compOptions.similarityThreshold = std::stod(thresholdIt->second);
    }
    
    // Compare
    ComparisonResult result = ScreenCapture::CompareFiles(referencePath, actualPath, compOptions);
    
    // Save diff image if mismatch
    if (!result.match && !result.diffImage.empty()) {
        std::string diffPath = context.currentTestConfig->screenshotDir + "/diff_" + 
            Utils::GetTimestamp() + ".png";
        ScreenCapture::SaveImage(diffPath, result.diffImage);
    }
    
    // Build result
    if (result.match) {
        return ActionResult::Ok("Images match: " + result.message);
    } else {
        return ActionResult::Fail("Images differ: " + result.message);
    }
}

std::string ScreenComparisonHandler::GetReferenceDir(AutomationContext& context) const {
    if (context.currentTestConfig) {
        return context.currentTestConfig->screenshotDir + "/refs";
    }
    return "./refs";
}

// ==================== TestResultReporter Implementation ====================

std::string TestResultReporter::GenerateReport(const TestResult& result, Format format) {
    switch (format) {
    case Format::Json: return GenerateJson(result);
    case Format::Xml: return GenerateXml(result);
    case Format::Html: return GenerateHtml(result);
    case Format::Markdown: return GenerateMarkdown(result);
    case Format::JUnit: return GenerateJUnit(result);
    case Format::Console: return "";  // Handled separately
    default: return GenerateJson(result);
    }
}

bool TestResultReporter::SaveReport(const TestResult& result, const std::string& filepath, Format format) {
    std::string content = GenerateReport(result, format);
    if (content.empty()) return false;
    
    std::ofstream file(filepath);
    if (!file.is_open()) return false;
    file << content;
    return true;
}

std::string TestResultReporter::GenerateJson(const TestResult& result) {
    Json::Value root;
    root["testName"] = result.testName;
    root["overallSuccess"] = result.overallSuccess;
    root["totalDurationMs"] = (int)result.totalDuration.count();
    root["totalSteps"] = result.TotalSteps();
    root["passedSteps"] = result.PassedSteps();
    root["failedSteps"] = result.FailedSteps();
    root["errorMessage"] = result.errorMessage;
    root["timestamp"] = Utils::GetTimestamp();
    
    // Statistics
    Json::Value stats;
    stats["totalActions"] = result.statistics.totalActions;
    stats["passedActions"] = result.statistics.passedActions;
    stats["failedActions"] = result.statistics.failedActions;
    stats["avgActionDurationMs"] = (int)result.statistics.avgActionDuration.count();
    root["statistics"] = stats;
    
    // Steps
    Json::Value steps(Json::arrayValue);
    for (const auto& r : result.testResults) {
        Json::Value step;
        step["id"] = r.stepId;
        step["description"] = r.description;
        step["success"] = r.actionResult.success;
        step["message"] = r.actionResult.message;
        step["durationMs"] = (int)r.duration.count();
        step["retryAttempts"] = r.retryAttempts;
        if (!r.screenshotPath.empty()) {
            step["screenshot"] = r.screenshotPath;
        }
        steps.append(step);
    }
    root["steps"] = steps;
    
    Json::StreamWriterBuilder builder;
    return Json::writeString(builder, root);
}

std::string TestResultReporter::GenerateXml(const TestResult& result) {
    std::ostringstream xml;
    xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    xml << "<test_result>\n";
    xml << "  <test_name>" << EscapeXml(result.testName) << "</test_name>\n";
    xml << "  <overall_success>" << (result.overallSuccess ? "true" : "false") << "</overall_success>\n";
    xml << "  <total_duration_ms>" << result.totalDuration.count() << "</total_duration_ms>\n";
    xml << "  <timestamp>" << Utils::GetTimestamp() << "</timestamp>\n";
    xml << "  <total_steps>" << result.TotalSteps() << "</total_steps>\n";
    xml << "  <passed_steps>" << result.PassedSteps() << "</passed_steps>\n";
    xml << "  <failed_steps>" << result.FailedSteps() << "</failed_steps>\n";
    
    if (!result.errorMessage.empty()) {
        xml << "  <error>" << EscapeXml(result.errorMessage) << "</error>\n";
    }
    
    xml << "  <steps>\n";
    for (const auto& r : result.testResults) {
        xml << "    <step>\n";
        xml << "      <id>" << r.stepId << "</id>\n";
        xml << "      <description>" << EscapeXml(r.description) << "</description>\n";
        xml << "      <success>" << (r.actionResult.success ? "true" : "false") << "</success>\n";
        xml << "      <message>" << EscapeXml(r.actionResult.message) << "</message>\n";
        xml << "      <duration_ms>" << r.duration.count() << "</duration_ms>\n";
        xml << "    </step>\n";
    }
    xml << "  </steps>\n";
    xml << "</test_result>\n";
    return xml.str();
}

std::string TestResultReporter::GenerateHtml(const TestResult& result) {
    std::ostringstream html;
    html << "<!DOCTYPE html>\n<html>\n<head>\n";
    html << "<title>Test Result: " << EscapeHtml(result.testName) << "</title>\n";
    html << "<style>\n";
    html << "body { font-family: Arial, sans-serif; margin: 20px; background: #f5f5f5; }\n";
    html << ".container { max-width: 1200px; margin: 0 auto; background: white; padding: 20px; border-radius: 8px; }\n";
    html << ".header { margin-bottom: 20px; padding-bottom: 10px; border-bottom: 2px solid #ddd; }\n";
    html << ".success { color: #28a745; }\n";
    html << ".failure { color: #dc3545; }\n";
    html << "table { width: 100%; border-collapse: collapse; margin-top: 20px; }\n";
    html << "th, td { padding: 10px; text-align: left; border-bottom: 1px solid #ddd; }\n";
    html << "th { background: #f8f9fa; }\n";
    html << ".step-success { background: #d4edda; }\n";
    html << ".step-failure { background: #f8d7da; }\n";
    html << "</style>\n</head>\n<body>\n";
    
    html << "<div class='container'>\n";
    html << "<div class='header'>\n";
    html << "<h1>Test Result: " << EscapeHtml(result.testName) << "</h1>\n";
    html << "<h2 class='" << (result.overallSuccess ? "success" : "failure") << "'>";
    html << (result.overallSuccess ? "✓ PASSED" : "✗ FAILED") << "</h2>\n";
    html << "<p>Duration: " << result.totalDuration.count() << "ms</p>\n";
    html << "<p>Steps: " << result.PassedSteps() << "/" << result.TotalSteps() << " passed</p>\n";
    html << "</div>\n";
    
    html << "<table>\n";
    html << "<tr><th>ID</th><th>Description</th><th>Status</th><th>Duration</th><th>Message</th></tr>\n";
    for (const auto& r : result.testResults) {
        html << "<tr class='" << (r.actionResult.success ? "step-success" : "step-failure") << "'>\n";
        html << "<td>" << r.stepId << "</td>\n";
        html << "<td>" << EscapeHtml(r.description) << "</td>\n";
        html << "<td>" << (r.actionResult.success ? "✓" : "✗") << "</td>\n";
        html << "<td>" << r.duration.count() << "ms</td>\n";
        html << "<td>" << EscapeHtml(r.actionResult.message) << "</td>\n";
        html << "</tr>\n";
    }
    html << "</table>\n";
    html << "</div>\n</body>\n</html>\n";
    
    return html.str();
}

std::string TestResultReporter::GenerateMarkdown(const TestResult& result) {
    std::ostringstream md;
    md << "# Test Result: " << result.testName << "\n\n";
    md << "**Status:** " << (result.overallSuccess ? "✅ PASSED" : "❌ FAILED") << "\n\n";
    md << "**Duration:** " << result.totalDuration.count() << "ms\n\n";
    md << "**Summary:** " << result.PassedSteps() << "/" << result.TotalSteps() << " steps passed\n\n";
    
    if (!result.errorMessage.empty()) {
        md << "**Error:** " << result.errorMessage << "\n\n";
    }
    
    md << "## Steps\n\n";
    md << "| ID | Description | Status | Duration | Message |\n";
    md << "|---|---|---|---|---|\n";
    for (const auto& r : result.testResults) {
        md << "| " << r.stepId << " | " << r.description << " | " 
           << (r.actionResult.success ? "✅" : "❌") << " | " 
           << r.duration.count() << "ms | " << r.actionResult.message << " |\n";
    }
    return md.str();
}

std::string TestResultReporter::GenerateJUnit(const TestResult& result) {
    std::ostringstream xml;
    xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    xml << "<testsuite name=\"" << EscapeXml(result.testName) << "\" ";
    xml << "tests=\"" << result.TotalSteps() << "\" ";
    xml << "failures=\"" << result.FailedSteps() << "\" ";
    xml << "time=\"" << (result.totalDuration.count() / 1000.0) << "\">\n";
    
    for (const auto& r : result.testResults) {
        xml << "  <testcase name=\"" << EscapeXml(r.description) << "\" ";
        xml << "time=\"" << (r.duration.count() / 1000.0) << "\">\n";
        if (!r.actionResult.success) {
            xml << "    <failure message=\"" << EscapeXml(r.actionResult.message) << "\"/>\n";
        }
        xml << "  </testcase>\n";
    }
    
    xml << "</testsuite>\n";
    return xml.str();
}

std::string TestResultReporter::EscapeJson(const std::string& str) {
    std::string result;
    for (char c : str) {
        switch (c) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default: result += c;
        }
    }
    return result;
}

std::string TestResultReporter::EscapeXml(const std::string& str) {
    std::string result;
    for (char c : str) {
        switch (c) {
        case '<': result += "&lt;"; break;
        case '>': result += "&gt;"; break;
        case '&': result += "&amp;"; break;
        case '"': result += "&quot;"; break;
        case '\'': result += "&apos;"; break;
        default: result += c;
        }
    }
    return result;
}

std::string TestResultReporter::EscapeHtml(const std::string& str) {
    return EscapeXml(str);
}

void TestResultReporter::PrintToConsole(const TestResult& result, bool useColors) {
    // ANSI color codes
    const char* GREEN = useColors ? "\033[32m" : "";
    const char* RED = useColors ? "\033[31m" : "";
    const char* YELLOW = useColors ? "\033[33m" : "";
    const char* RESET = useColors ? "\033[0m" : "";
    
    std::cout << "\n========================================\n";
    std::cout << "Test Result: " << result.testName << "\n";
    std::cout << "========================================\n\n";
    
    if (result.overallSuccess) {
        std::cout << GREEN << "✓ PASSED" << RESET << "\n";
    } else {
        std::cout << RED << "✗ FAILED" << RESET << "\n";
    }
    
    std::cout << "Duration: " << result.totalDuration.count() << "ms\n";
    std::cout << "Steps: " << result.PassedSteps() << "/" << result.TotalSteps() << " passed\n\n";
    
    std::cout << "Steps:\n";
    for (const auto& r : result.testResults) {
        if (r.actionResult.success) {
            std::cout << GREEN << "  ✓ " << RESET;
        } else {
            std::cout << RED << "  ✗ " << RESET;
        }
        std::cout << "Step " << r.stepId << ": " << r.description << " (" 
                  << r.duration.count() << "ms)\n";
        if (!r.actionResult.success) {
            std::cout << "    Error: " << r.actionResult.message << "\n";
        }
    }
    std::cout << "\n";
}

// ==================== ParallelTestRunner Implementation ====================

ParallelTestRunner::ParallelTestRunner(const Config& config) : config_(config) {}

ParallelTestRunner::~ParallelTestRunner() {}

void ParallelTestRunner::AddTest(const TestSuite& suite) {
    tests_.push_back(suite);
}

void ParallelTestRunner::AddTests(const std::vector<TestSuite>& suites) {
    tests_.insert(tests_.end(), suites.begin(), suites.end());
}

std::vector<TestResult> ParallelTestRunner::RunAll() {
    results_.clear();
    results_.resize(tests_.size());
    completed_ = 0;
    failed_ = 0;
    
    // Create thread pool
    std::vector<std::thread> workers;
    int numWorkers = std::min(config_.maxWorkers, (int)tests_.size());
    
    std::atomic<int> nextIndex{0};
    
    for (int i = 0; i < numWorkers; i++) {
        workers.emplace_back([this, &nextIndex]() {
            while (true) {
                int index = nextIndex++;
                if (index >= (int)tests_.size()) break;
                
                RunTestWorker(index);
                
                if (config_.stopOnFirstFailure && failed_ > 0) {
                    break;
                }
            }
        });
    }
    
    // Wait for all workers
    for (auto& worker : workers) {
        worker.join();
    }
    
    return results_;
}

void ParallelTestRunner::RunTestWorker(int index) {
    TestRunner runner;
    if (!runner.Initialize()) {
        TestResult result;
        result.testName = tests_[index].config.name;
        result.overallSuccess = false;
        result.errorMessage = "Failed to initialize test runner";
        results_[index] = result;
        completed_++;
        failed_++;
        return;
    }
    
    // Run test
    TestResult result = runner.RunTest(tests_[index]);
    results_[index] = result;
    
    completed_++;
    if (!result.overallSuccess) {
        failed_++;
    }
    
    if (onProgress) {
        onProgress(completed_, (int)tests_.size(), result.testName);
    }
    
    if (onTestComplete) {
        onTestComplete(result);
    }
}

int ParallelTestRunner::GetCompletedCount() const { return completed_; }
int ParallelTestRunner::GetTotalCount() const { return (int)tests_.size(); }
int ParallelTestRunner::GetRunningCount() const { 
    return GetTotalCount() - GetCompletedCount(); 
}
int ParallelTestRunner::GetFailedCount() const { return failed_; }

} // namespace Automation
} // namespace KVMDrivers
