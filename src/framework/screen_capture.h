// screen_capture.h - Screen capture and image comparison for test assertions
#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <memory>
#include <opencv2/opencv.hpp>

namespace KVMDrivers {
namespace Automation {

// Screen capture options
struct CaptureOptions {
    bool fullScreen = true;
    int x = 0, y = 0, width = 0, height = 0;  // Used if fullScreen=false
    bool captureCursor = false;
    bool captureAlpha = false;
};

// Image comparison options
struct ComparisonOptions {
    double similarityThreshold = 0.95;  // 0.0 - 1.0
    bool ignoreCursor = true;
    bool ignoreDateTime = false;  // Ignore regions with changing timestamps
    double pixelTolerance = 0.02;  // Per-pixel color tolerance (0.0 - 1.0)
    std::vector<cv::Rect> ignoreRegions;  // Regions to ignore in comparison
};

// Comparison result
struct ComparisonResult {
    bool match;
    double similarity;  // 0.0 - 1.0
    int diffPixelCount;
    double diffPercentage;
    cv::Mat diffImage;    // Visual diff (red = different)
    std::string message;
    
    ComparisonResult() : match(false), similarity(0.0), diffPixelCount(0), diffPercentage(0.0) {}
};

// Screen capture and comparison utility
class ScreenCapture {
public:
    ScreenCapture();
    ~ScreenCapture();
    
    // Capture screen to file
    bool CaptureToFile(const std::string& filepath, const CaptureOptions& options = {});
    
    // Capture to OpenCV Mat
    cv::Mat CaptureToMat(const CaptureOptions& options = {});
    
    // Capture specific window
    bool CaptureWindow(HWND hwnd, const std::string& filepath);
    cv::Mat CaptureWindow(HWND hwnd);
    
    // Compare two images
    static ComparisonResult CompareImages(const cv::Mat& reference, 
        const cv::Mat& actual, const ComparisonOptions& options = {});
    
    // Compare image files
    static ComparisonResult CompareFiles(const std::string& referencePath,
        const std::string& actualPath, const ComparisonOptions& options = {});
    
    // Find image within screen (template matching)
    static bool FindImage(const cv::Mat& screen, const cv::Mat& templateImg,
        double threshold, cv::Point& location, double& confidence);
    
    // Highlight differences in image
    static cv::Mat CreateDiffVisualization(const cv::Mat& reference,
        const cv::Mat& actual, const ComparisonResult& result);
    
    // Utilities
    static bool LoadImage(const std::string& path, cv::Mat& image);
    static bool SaveImage(const std::string& path, const cv::Mat& image);
    static cv::Mat ResizeImage(const cv::Mat& image, int width, int height);
    
private:
    HDC screenDC_;
    HDC memoryDC_;
    
    bool InitializeDC();
    void CleanupDC();
};

// Test assertion handler for screen comparison
class ScreenComparisonHandler : public ITestActionHandler {
public:
    std::string GetName() const override { return "ScreenComparisonHandler"; }
    std::string GetDescription() const override { return "Compare screen capture to reference image"; }
    ActionType GetActionType() const override { return ActionType::AssertImage; }
    
    bool ValidateParameters(const std::map<std::string, std::string>& params,
        std::string& errorMessage) const override;
    ActionResult Execute(const TestStep& step, AutomationContext& context) override;
    
private:
    std::string GetReferenceDir(AutomationContext& context) const;
};

// Result reporting system
class TestResultReporter {
public:
    // Report formats
    enum class Format {
        Json,
        Xml,
        Html,
        Markdown,
        JUnit,
        Console
    };
    
    // Generate report from test result
    static std::string GenerateReport(const TestResult& result, Format format);
    
    // Save report to file
    static bool SaveReport(const TestResult& result, const std::string& filepath, Format format);
    
    // Generate batch report from multiple results
    static std::string GenerateBatchReport(const std::vector<TestResult>& results, Format format);
    static bool SaveBatchReport(const std::vector<TestResult>& results, 
        const std::string& filepath, Format format);
    
    // Console output with colors
    static void PrintToConsole(const TestResult& result, bool useColors = true);
    static void PrintToConsole(const std::vector<TestResult>& results, bool useColors = true);
    
private:
    static std::string GenerateJson(const TestResult& result);
    static std::string GenerateXml(const TestResult& result);
    static std::string GenerateHtml(const TestResult& result);
    static std::string GenerateMarkdown(const TestResult& result);
    static std::string GenerateJUnit(const TestResult& result);
    
    static std::string EscapeJson(const std::string& str);
    static std::string EscapeXml(const std::string& str);
    static std::string EscapeHtml(const std::string& str);
};

// Parallel test execution
class ParallelTestRunner {
public:
    struct Config {
        int maxWorkers = 4;  // Maximum parallel tests
        int timeoutPerTest = 300;  // Seconds
        bool stopOnFirstFailure = false;
        bool collectAllResults = true;
    };
    
    ParallelTestRunner(const Config& config = {});
    ~ParallelTestRunner();
    
    // Add test to queue
    void AddTest(const TestSuite& suite);
    void AddTests(const std::vector<TestSuite>& suites);
    
    // Execute all tests in parallel
    std::vector<TestResult> RunAll();
    
    // Get progress
    int GetCompletedCount() const;
    int GetTotalCount() const;
    int GetRunningCount() const;
    int GetFailedCount() const;
    
    // Events
    std::function<void(int completed, int total, const std::string& current)> onProgress;
    std::function<void(const TestResult& result)> onTestComplete;
    std::function<void(const std::string& error)> onError;

private:
    Config config_;
    std::vector<TestSuite> tests_;
    std::vector<TestResult> results_;
    
    int completed_ = 0;
    int failed_ = 0;
    
    void RunTestWorker(int index);
};

} // namespace Automation
} // namespace KVMDrivers
