// game_automation.h - Game Testing Extensions for KVM-Drivers Automation Framework
// Provides application launching, UI automation, and game-specific actions

#pragma once

#include "automation_framework.h"
#include <windows.h>
#include <uiautomation.h>
#include <msctf.h>
#include <string>
#include <vector>
#include <functional>
#include <memory>

namespace KVMDrivers {
namespace Automation {
namespace GameTesting {

// Forward declarations
class ApplicationLauncher;
class UIAutomationProvider;
class ScreenAnalyzer;
class GameStateDetector;

// ==================== Application Launcher ====================

struct LaunchConfig {
    std::string executablePath;      // Path to .exe
    std::string workingDirectory;    // Working directory
    std::vector<std::string> arguments;  // Command line args
    std::string windowTitle;         // Expected window title (for detection)
    int launchTimeoutMs = 30000;     // Max time to wait for launch
    bool waitForWindow = true;       // Wait for window to appear
    bool maximizeWindow = false;     // Maximize after launch
    bool setForeground = true;         // Bring to front
};

struct ProcessInfo {
    DWORD processId;
    HWND windowHandle;
    std::string windowTitle;
    RECT windowRect;
    bool isRunning;
};

class ApplicationLauncher : public ITestActionHandler {
public:
    ApplicationLauncher();
    ~ApplicationLauncher();
    
    // ITestActionHandler implementation
    std::string GetName() const override { return "ApplicationLauncher"; }
    std::string GetDescription() const override { 
        return "Launch applications and wait for windows"; 
    }
    ActionType GetActionType() const override { return ActionType::Custom; }
    
    bool ValidateParameters(const std::map<std::string, std::string>& params,
        std::string& errorMessage) const override;
    ActionResult Execute(const TestStep& step, AutomationContext& context) override;
    bool IsRetryable() const override { return true; }
    
    // Direct API
    ProcessInfo Launch(const LaunchConfig& config);
    bool Terminate(DWORD processId, int exitCode = 0);
    bool IsRunning(DWORD processId);
    ProcessInfo FindWindowByTitle(const std::string& titlePattern);
    ProcessInfo FindWindowByProcessId(DWORD processId);
    bool BringToForeground(HWND hwnd);
    bool MaximizeWindow(HWND hwnd);
    bool SetWindowPosition(HWND hwnd, int x, int y, int width, int height);
    
private:
    bool WaitForWindow(const LaunchConfig& config, ProcessInfo& info);
};

// ==================== UI Automation Provider ====================

// UI Element description
struct UIElementCriteria {
    std::string automationId;      // Unique ID from accessibility tree
    std::string name;            // Display name/text
    std::string className;       // Window class name
    std::string controlType;     // Button, Edit, Menu, etc.
    std::string helpText;          // Tooltip/help text
    bool enabled = true;         // Must be enabled
    bool visible = true;         // Must be visible
};

// Found UI element
struct UIElement {
    std::string automationId;
    std::string name;
    std::string controlType;
    std::string className;
    RECT boundingRect;
    bool enabled;
    bool visible;
    int processId;
    void* nativeHandle;  // IUIAutomationElement* (opaque pointer)
    
    // Calculated center point for clicking
    int CenterX() const { return (boundingRect.left + boundingRect.right) / 2; }
    int CenterY() const { return (boundingRect.top + boundingRect.bottom) / 2; }
};

class UIAutomationProvider : public ITestActionHandler {
public:
    UIAutomationProvider();
    ~UIAutomationProvider();
    
    // Initialize COM and UI Automation
    bool Initialize();
    void Shutdown();
    
    // ITestActionHandler implementation
    std::string GetName() const override { return "UIAutomationProvider"; }
    std::string GetDescription() const override { 
        return "Find and interact with UI elements using accessibility"; 
    }
    ActionType GetActionType() const override { return ActionType::Custom; }
    
    bool ValidateParameters(const std::map<std::string, std::string>& params,
        std::string& errorMessage) const override;
    ActionResult Execute(const TestStep& step, AutomationContext& context) override;
    
    // Direct API
    std::vector<UIElement> FindElements(const UIElementCriteria& criteria, 
        int maxResults = 10);
    UIElement FindFirstElement(const UIElementCriteria& criteria);
    bool ClickElement(const UIElement& element);
    bool SendText(const UIElement& element, const std::string& text);
    std::string GetText(const UIElement& element);
    bool InvokeAction(const UIElement& element, const std::string& actionName);
    
    // Element tree navigation
    std::vector<UIElement> GetChildren(const UIElement& parent);
    UIElement GetParent(const UIElement& element);
    UIElement GetRootElement();
    
    // Wait for element to appear
    UIElement WaitForElement(const UIElementCriteria& criteria, int timeoutMs);
    
    // Screenshot with element highlighting
    bool CaptureWithHighlight(const std::vector<UIElement>& elements, 
        const std::string& filepath);
    
    // MSAA (legacy) support
    UIElement FindElementMSAA(const UIElementCriteria& criteria);

private:
    void* automation_;  // IUIAutomation*
    bool initialized_;
};

// ==================== Screen Analyzer ====================

enum class FindMethod {
    TemplateMatch,    // Image template matching
    ColorDetection,   // Color-based detection
    OCR,              // Text recognition
    EdgeDetection,    // Shape detection
    MLModel,          // Machine learning model
    Accessibility     // UI Automation tree
};

struct ImageTemplate {
    std::string name;
    std::string imagePath;           // Reference image
    double similarityThreshold;      // 0.0 - 1.0
    bool allowScale;                 // Allow different sizes
    bool allowRotation;              // Allow rotation
};

struct SearchRegion {
    int x, y, width, height;
    bool fullScreen = false;
};

struct FoundObject {
    std::string name;
    int x, y, width, height;
    double confidence;  // 0.0 - 1.0
    FindMethod method;
    
    int CenterX() const { return x + width / 2; }
    int CenterY() const { return y + height / 2; }
};

class ScreenAnalyzer : public ITestActionHandler {
public:
    ScreenAnalyzer();
    ~ScreenAnalyzer();
    
    // ITestActionHandler implementation
    std::string GetName() const override { return "ScreenAnalyzer"; }
    std::string GetDescription() const override { 
        return "Analyze screen to find objects by image or text"; 
    }
    ActionType GetActionType() const override { return ActionType::Custom; }
    
    bool ValidateParameters(const std::map<std::string, std::string>& params,
        std::string& errorMessage) const override;
    ActionResult Execute(const TestStep& step, AutomationContext& context) override;
    
    // Direct API
    void SetReferenceDirectory(const std::string& dir);
    
    // Template matching
    std::vector<FoundObject> FindTemplate(const ImageTemplate& template_, 
        const SearchRegion& region = {});
    FoundObject FindFirstTemplate(const ImageTemplate& template_, 
        const SearchRegion& region = {});
    
    // Text finding (OCR)
    std::vector<FoundObject> FindText(const std::string& text, 
        const SearchRegion& region = {});
    FoundObject FindFirstText(const std::string& text, 
        const SearchRegion& region = {});
    std::string ReadTextAt(const SearchRegion& region);
    
    // Color detection
    std::vector<FoundObject> FindColor(const std::string& colorHex, 
        int tolerance = 10, const SearchRegion& region = {});
    
    // Wait for object
    FoundObject WaitForTemplate(const ImageTemplate& template_, 
        int timeoutMs, const SearchRegion& region = {});
    FoundObject WaitForText(const std::string& text, 
        int timeoutMs, const SearchRegion& region = {});
    
    // Multi-method search (tries multiple approaches)
    FoundObject FindObject(const std::string& name, 
        const std::vector<FindMethod>& methods, int timeoutMs);
    
    // Screenshot analysis
    bool CaptureRegion(const SearchRegion& region, const std::string& filepath);
    
    // Visual debugging
    bool CaptureWithAnnotations(const std::vector<FoundObject>& objects,
        const std::string& filepath);

private:
    std::string referenceDir_;
    
    // Internal capture
    bool CaptureScreenToFile(const std::string& filepath);
    bool CaptureRegionToFile(int x, int y, int w, int h, const std::string& filepath);
};

// ==================== Game State Detector ====================

enum class GameState {
    Unknown,
    Loading,        // Loading screen
    MainMenu,       // Main menu
    PauseMenu,      // Pause/menu screen
    InGame,         // Active gameplay
    Cutscene,       // Non-interactive cinematic
    Dialog,         // Dialog/conversation
    Inventory,      // Inventory/character screen
    Map,            // Map screen
    DeathScreen,    // Game over/death
    VictoryScreen   // Victory/completion
};

struct GameStateSignature {
    GameState state;
    std::string name;
    std::vector<ImageTemplate> visualMarkers;  // Images that indicate this state
    std::vector<std::string> textMarkers;      // Text that indicates this state
    std::string colorProfile;                  // Dominant colors
    double confidenceThreshold;
};

struct GameStateResult {
    GameState state;
    double confidence;
    std::string detectedBy;  // Which signature matched
    std::chrono::milliseconds detectionTime;
};

class GameStateDetector : public ITestActionHandler {
public:
    GameStateDetector();
    ~GameStateDetector();
    
    // ITestActionHandler implementation
    std::string GetName() const override { return "GameStateDetector"; }
    std::string GetDescription() const override { 
        return "Detect current game state (loading, menu, gameplay)"; 
    }
    ActionType GetActionType() const override { return ActionType::Custom; }
    
    bool ValidateParameters(const std::map<std::string, std::string>& params,
        std::string& errorMessage) const override;
    ActionResult Execute(const TestStep& step, AutomationContext& context) override;
    
    // Configuration
    void LoadStateSignatures(const std::string& jsonFile);
    void AddStateSignature(const GameStateSignature& signature);
    
    // Detection
    GameStateResult DetectCurrentState();
    bool WaitForState(GameState state, int timeoutMs, double minConfidence = 0.8);
    bool WaitForStateChange(int timeoutMs);
    bool IsStateStable(GameState state, int stableDurationMs);
    
    // Polling
    void StartStatePolling(int intervalMs, 
        std::function<void(const GameStateResult&)> callback);
    void StopStatePolling();
    
    // Specific checks
    bool IsLoading();
    bool IsInMenu();
    bool IsInGameplay();
    bool IsPaused();
    
    // Transition detection
    bool WaitForLoadingToComplete(int timeoutMs);
    bool WaitForMenuToLoad(int timeoutMs);
    bool WaitForGameplayStart(int timeoutMs);
    
    // Performance monitoring
    double GetAverageDetectionTime() const;
    int GetDetectionCount() const;

private:
    std::vector<GameStateSignature> signatures_;
    void* pollingThread_;
    bool pollingActive_;
    int detectionCount_;
    double totalDetectionTimeMs_;
};

// ==================== Game Automation Script Format ====================

// Extended YAML format for game testing
// Example:
// game: "MyGame.exe"
// version: "1.0"
// 
// launch:
//   executable: "C:\\Games\\MyGame\\MyGame.exe"
//   args: ["-windowed", "-res 1920x1080"]
//   timeout: 30000
//   wait_for_window: true
// 
// states:
//   - name: main_menu
//     markers:
//       - image: "refs/main_menu_title.png"
//       - text: "Press Any Key"
// 
// automation:
//   - action: wait_for_state
//     state: main_menu
//     timeout: 60000
//     
//   - action: click_element
//     by: accessibility
//     name: "Start Game"
//     
//   - action: click_element
//     by: template
//     template: "refs/start_button.png"
//     
//   - action: wait_for_loading
//     timeout: 120000
//     
//   - action: play_game
//     duration: 30000
//     inputs:
//       - keys: ["w", "a", "s", "d"]
//       - mouse_look: true

struct GameAutomationConfig {
    std::string gameName;
    std::string gameVersion;
    LaunchConfig launch;
    std::vector<GameStateSignature> stateSignatures;
    std::vector<std::shared_ptr<TestStep>> automationSteps;
};

class GameAutomationLoader {
public:
    // Load game automation script from YAML
    static GameAutomationConfig LoadFromFile(const std::string& filepath);
    static GameAutomationConfig LoadFromString(const std::string& yamlContent);
    
    // Convert to standard TestSuite
    static TestSuite ToTestSuite(const GameAutomationConfig& config);
    
    // Save game automation config
    static bool SaveToFile(const GameAutomationConfig& config, 
        const std::string& filepath);
};

// ==================== Game-Specific Action Handlers ====================

// Smart click - tries accessibility first, then image, then coordinates
class SmartClickHandler : public ITestActionHandler {
public:
    std::string GetName() const override { return "SmartClickHandler"; }
    std::string GetDescription() const override { 
        return "Smart click using accessibility, image, or coordinates"; 
    }
    ActionType GetActionType() const override { return ActionType::Custom; }
    
    bool ValidateParameters(const std::map<std::string, std::string>& params,
        std::string& errorMessage) const override;
    ActionResult Execute(const TestStep& step, AutomationContext& context) override;
};

// Wait for loading with progress detection
class WaitForLoadingHandler : public ITestActionHandler {
public:
    std::string GetName() const override { return "WaitForLoadingHandler"; }
    std::string GetDescription() const override { 
        return "Wait for loading screen to complete"; 
    }
    ActionType GetActionType() const override { return ActionType::Custom; }
    
    bool ValidateParameters(const std::map<std::string, std::string>& params,
        std::string& errorMessage) const override;
    ActionResult Execute(const TestStep& step, AutomationContext& context) override;
};

// Navigate menu using accessibility tree
class NavigateMenuHandler : public ITestActionHandler {
public:
    std::string GetName() const override { return "NavigateMenuHandler"; }
    std::string GetDescription() const override { 
        return "Navigate menu using arrow keys or accessibility"; 
    }
    ActionType GetActionType() const override { return ActionType::Custom; }
    
    bool ValidateParameters(const std::map<std::string, std::string>& params,
        std::string& errorMessage) const override;
    ActionResult Execute(const TestStep& step, AutomationContext& context) override;
};

// Play game - automated gameplay with configurable inputs
class PlayGameHandler : public ITestActionHandler {
public:
    std::string GetName() const override { return "PlayGameHandler"; }
    std::string GetDescription() const override { 
        return "Automated gameplay with scripted inputs"; 
    }
    ActionType GetActionType() const override { return ActionType::Custom; }
    
    bool ValidateParameters(const std::map<std::string, std::string>& params,
        std::string& errorMessage) const override;
    ActionResult Execute(const TestStep& step, AutomationContext& context) override;
};

// ==================== Game Test Framework Extension ====================

class GameTestFramework {
public:
    GameTestFramework(TestRunner& baseRunner);
    ~GameTestFramework();
    
    // Register all game-specific handlers
    void RegisterGameHandlers();
    
    // Launch and run game automation
    TestResult RunGameTest(const GameAutomationConfig& config);
    
    // Quick game smoke test
    static TestResult QuickGameTest(const std::string& executablePath,
        int playDurationMs = 30000);
    
    // Reference image management
    void SetReferenceImageDirectory(const std::string& dir);
    void CaptureReferenceImage(const std::string& name, 
        const UIElementCriteria& element = {});
    void CaptureReferenceImageAt(int x, int y, int w, int h, 
        const std::string& name);

private:
    TestRunner& runner_;
    ApplicationLauncher* launcher_;
    UIAutomationProvider* uiProvider_;
    ScreenAnalyzer* screenAnalyzer_;
    GameStateDetector* stateDetector_;
};

} // namespace GameTesting
} // namespace Automation
} // namespace KVMDrivers
