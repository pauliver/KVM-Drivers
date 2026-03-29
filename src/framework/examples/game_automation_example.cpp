// game_automation_example.cpp - Complete game automation example

#include "../framework/game_automation.h"
#include <iostream>

using namespace KVMDrivers::Automation;
using namespace KVMDrivers::Automation::GameTesting;

void Example_LaunchAndNavigate() {
    std::cout << "\n========================================\n";
    std::cout << "Example: Launch Game and Navigate Menu\n";
    std::cout << "========================================\n\n";
    
    // Create base runner
    TestRunner runner;
    runner.Initialize();
    
    // Extend with game testing capabilities
    GameTestFramework gameTest(runner);
    gameTest.RegisterGameHandlers();
    gameTest.SetReferenceImageDirectory("./refs/mygame");
    
    // Create game automation config
    GameAutomationConfig config;
    config.gameName = "MyGame";
    config.gameVersion = "1.0";
    config.launch.executablePath = "C:\\Games\\MyGame\\Game.exe";
    config.launch.windowTitle = "MyGame";
    config.launch.waitForWindow = true;
    
    // Add state signatures
    GameStateSignature menuState;
    menuState.state = GameState::MainMenu;
    menuState.name = "main_menu";
    ImageTemplate menuMarker;
    menuMarker.imagePath = "./refs/mygame/main_menu_title.png";
    menuMarker.similarityThreshold = 0.9;
    menuState.visualMarkers.push_back(menuMarker);
    config.stateSignatures.push_back(menuState);
    
    // Create automation steps
    // Step 1: Wait for main menu
    auto step1 = std::make_shared<TestStep>();
    step1->actionType = ActionType::Custom;
    step1->parameters["action_name"] = "wait_for_state";
    step1->parameters["state"] = "main_menu";
    step1->parameters["timeout"] = "60000";
    config.automationSteps.push_back(step1);
    
    // Step 2: Click New Game
    auto step2 = std::make_shared<TestStep>();
    step2->actionType = ActionType::Custom;
    step2->parameters["action_name"] = "smart_click";
    step2->parameters["accessibility_name"] = "New Game";
    step2->parameters["template"] = "./refs/mygame/new_game_button.png";
    config.automationSteps.push_back(step2);
    
    // Step 3: Wait for loading
    auto step3 = std::make_shared<TestStep>();
    step3->actionType = ActionType::Custom;
    step3->parameters["action_name"] = "wait_for_loading";
    step3->parameters["timeout"] = "120000";
    config.automationSteps.push_back(step3);
    
    // Run the test
    TestResult result = gameTest.RunGameTest(config);
    
    std::cout << "Game Test Result: " << (result.overallSuccess ? "PASSED" : "FAILED") << "\n";
    std::cout << "Duration: " << result.totalDuration.count() << "ms\n";
}

void Example_SmartClick() {
    std::cout << "\n========================================\n";
    std::cout << "Example: Smart Click (Multi-Method)\n";
    std::cout << "========================================\n\n";
    
    TestRunner runner;
    runner.Initialize();
    
    // Register smart click handler
    runner.RegisterHandler(std::make_shared<SmartClickHandler>());
    
    // Try to click a button - will try accessibility first, then image, then text
    TestStep step;
    step.description = "Click Start Button";
    step.actionType = ActionType::Custom;
    step.parameters["action_name"] = "smart_click";
    step.parameters["accessibility_name"] = "Start Button";
    step.parameters["template"] = "./refs/start_button.png";
    step.parameters["text"] = "Start";
    step.parameters["x"] = "960";  // Fallback coordinates
    step.parameters["y"] = "540";
    
    ActionResult result = runner.ExecuteAction(step);
    
    std::cout << "Smart Click: " << result.message << "\n";
}

void Example_ScreenAnalysis() {
    std::cout << "\n========================================\n";
    std::cout << "Example: Screen Analysis (Find by Image)\n";
    std::cout << "========================================\n\n";
    
    ScreenAnalyzer analyzer;
    analyzer.SetReferenceDirectory("./refs");
    
    // Find button by template
    ImageTemplate buttonTemplate;
    buttonTemplate.name = "submit_button";
    buttonTemplate.imagePath = "./refs/submit_button.png";
    buttonTemplate.similarityThreshold = 0.85;
    
    SearchRegion region;
    region.fullScreen = true;
    
    auto found = analyzer.FindFirstTemplate(buttonTemplate, region);
    
    if (found.confidence > 0) {
        std::cout << "Found button at (" << found.CenterX() << ", " << found.CenterY() << ")\n";
        std::cout << "Confidence: " << (found.confidence * 100) << "%\n";
        std::cout << "Size: " << found.width << "x" << found.height << "\n";
    } else {
        std::cout << "Button not found on screen\n";
    }
    
    // Find by text using OCR
    auto textObjects = analyzer.FindText("Submit", region);
    std::cout << "Found " << textObjects.size() << " text occurrences\n";
    
    // Find by color (e.g., red health bar)
    auto colorObjects = analyzer.FindColor("FF0000", 20, region);
    std::cout << "Found " << colorObjects.size() << " red regions\n";
}

void Example_UIAutomation() {
    std::cout << "\n========================================\n";
    std::cout << "Example: UI Automation (Accessibility)\n";
    std::cout << "========================================\n\n";
    
    UIAutomationProvider provider;
    if (!provider.Initialize()) {
        std::cerr << "Failed to initialize UI Automation\n";
        return;
    }
    
    // Find all buttons
    UIElementCriteria criteria;
    criteria.controlType = "Button";
    criteria.visible = true;
    criteria.enabled = true;
    
    auto buttons = provider.FindElements(criteria, 20);
    
    std::cout << "Found " << buttons.size() << " buttons:\n";
    for (const auto& btn : buttons) {
        std::cout << "  - \"" << btn.name << "\" at (" << btn.CenterX() << ", " << btn.CenterY() << ")\n";
    }
    
    // Click a specific button by name
    criteria.name = "OK";
    auto okButton = provider.FindFirstElement(criteria);
    
    if (okButton.nativeHandle != nullptr) {
        provider.ClickElement(okButton);
        std::cout << "Clicked OK button\n";
    }
    
    // Type into a text field
    criteria.name = "Username";
    criteria.controlType = "Edit";
    auto editField = provider.FindFirstElement(criteria);
    
    if (editField.nativeHandle != nullptr) {
        provider.SendText(editField, "testuser");
        std::cout << "Entered username\n";
    }
}

void Example_GameStateDetection() {
    std::cout << "\n========================================\n";
    std::cout << "Example: Game State Detection\n";
    std::cout << "========================================\n\n";
    
    GameStateDetector detector;
    
    // Define state signatures
    GameStateSignature loadingState;
    loadingState.state = GameState::Loading;
    loadingState.name = "loading";
    ImageTemplate loadingSpinner;
    loadingSpinner.imagePath = "./refs/loading_spinner.png";
    loadingSpinner.similarityThreshold = 0.8;
    loadingState.visualMarkers.push_back(loadingSpinner);
    loadingState.textMarkers.push_back("Loading...");
    detector.AddStateSignature(loadingState);
    
    GameStateSignature gameplayState;
    gameplayState.state = GameState::InGame;
    gameplayState.name = "gameplay";
    ImageTemplate hudMarker;
    hudMarker.imagePath = "./refs/hud_health.png";
    hudMarker.similarityThreshold = 0.85;
    gameplayState.visualMarkers.push_back(hudMarker);
    detector.AddStateSignature(gameplayState);
    
    // Detect current state
    auto result = detector.DetectCurrentState();
    
    std::cout << "Current State: ";
    switch (result.state) {
    case GameState::Loading: std::cout << "Loading"; break;
    case GameState::MainMenu: std::cout << "Main Menu"; break;
    case GameState::InGame: std::cout << "In Game"; break;
    case GameState::PauseMenu: std::cout << "Paused"; break;
    default: std::cout << "Unknown"; break;
    }
    std::cout << " (confidence: " << (result.confidence * 100) << "%)\n";
    
    // Wait for loading to complete
    std::cout << "Waiting for loading to complete...\n";
    if (detector.WaitForLoadingToComplete(120000)) {
        std::cout << "Loading complete!\n";
    } else {
        std::cout << "Timeout waiting for loading\n";
    }
    
    // Wait for gameplay to start
    std::cout << "Waiting for gameplay...\n";
    if (detector.WaitForGameplayStart(60000)) {
        std::cout << "Gameplay started!\n";
    } else {
        std::cout << "Timeout waiting for gameplay\n";
    }
}

void Example_LoadFromYaml() {
    std::cout << "\n========================================\n";
    std::cout << "Example: Load Game Automation from YAML\n";
    std::cout << "========================================\n\n";
    
    // Load game automation config
    auto config = GameAutomationLoader::LoadFromFile("tests/examples/game_automation_example.yaml");
    
    std::cout << "Game: " << config.gameName << "\n";
    std::cout << "Version: " << config.gameVersion << "\n";
    std::cout << "Executable: " << config.launch.executablePath << "\n";
    std::cout << "State Signatures: " << config.stateSignatures.size() << "\n";
    std::cout << "Automation Steps: " << config.automationSteps.size() << "\n";
    
    // Convert to standard test suite
    TestSuite suite = GameAutomationLoader::ToTestSuite(config);
    
    // Run with game test framework
    TestRunner runner;
    runner.Initialize();
    
    GameTestFramework gameTest(runner);
    gameTest.RegisterGameHandlers();
    
    TestResult result = gameTest.RunGameTest(config);
    
    std::cout << "\nTest " << (result.overallSuccess ? "PASSED" : "FAILED") << "\n";
    std::cout << "Total Steps: " << result.TotalSteps() << "\n";
    std::cout << "Passed: " << result.PassedSteps() << "\n";
    std::cout << "Failed: " << result.FailedSteps() << "\n";
    
    // Save results
    result.SaveToFile("game_test_result.json");
    result.SaveToFile("game_test_result.md");
}

void Example_PlayGame() {
    std::cout << "\n========================================\n";
    std::cout << "Example: Automated Gameplay\n";
    std::cout << "========================================\n\n";
    
    TestRunner runner;
    runner.Initialize();
    
    // Create a test that plays the game
    TestSuite suite;
    suite.WithName("Automated Gameplay Test")
         .WithDescription("Play game for 30 seconds with random inputs");
    
    // Launch game
    suite.AddTestStep(
        std::make_shared<TestStep>()
        ->WithDescription("Launch game")
        .WithAction(ActionType::Custom)
        .WithParam("action_name", "launch")
        .WithParam("executable", "C:\\Games\\MyGame\\Game.exe")
        .WithParam("timeout", "60000")
        .WithDelay(5000)
    );
    
    // Wait for gameplay
    suite.AddTestStep(
        std::make_shared<TestStep>()
        ->WithDescription("Wait for gameplay")
        .WithAction(ActionType::Custom)
        .WithParam("action_name", "wait_for_state")
        .WithParam("state", "gameplay")
        .WithParam("timeout", "120000")
    );
    
    // Play the game
    suite.AddTestStep(
        std::make_shared<TestStep>()
        ->WithDescription("Play game")
        .WithAction(ActionType::Custom)
        .WithParam("action_name", "play_game")
        .WithParam("duration", "30000")
        // Movement
        .WithParam("move_w", "true")
        .WithParam("move_a", "true")
        .WithParam("move_s", "true")
        .WithParam("move_d", "true")
        // Actions
        .WithParam("action_space", "0.3")  // 30% chance
        .WithParam("action_lmb", "0.5")   // 50% chance
        .WithParam("action_rmb", "0.2")   // 20% chance
        // Camera
        .WithParam("camera_enabled", "true")
        .WithParam("camera_pattern", "look_around")
    );
    
    // Take screenshot
    suite.AddTestStep(
        std::make_shared<TestStep>()
        ->WithDescription("Capture final state")
        .WithAction(ActionType::Screenshot)
        .WithParam("name", "gameplay_final")
    );
    
    // Close game
    suite.AddTeardownStep(
        std::make_shared<TestStep>()
        ->WithDescription("Close game")
        .WithAction(ActionType::KeyCombo)
        .WithParam("keys", "Alt+F4")
    );
    
    TestResult result = runner.RunTest(suite);
    
    std::cout << "Gameplay test completed: " << (result.overallSuccess ? "PASSED" : "FAILED") << "\n";
}

int main(int argc, char* argv[]) {
    std::cout << "KVM-Drivers Game Automation Examples\n";
    std::cout << "===================================\n\n";
    
    if (argc > 1) {
        std::string arg = argv[1];
        
        if (arg == "--launch") {
            Example_LaunchAndNavigate();
        } else if (arg == "--smart") {
            Example_SmartClick();
        } else if (arg == "--screen") {
            Example_ScreenAnalysis();
        } else if (arg == "--ui") {
            Example_UIAutomation();
        } else if (arg == "--state") {
            Example_GameStateDetection();
        } else if (arg == "--yaml") {
            Example_LoadFromYaml();
        } else if (arg == "--play") {
            Example_PlayGame();
        } else if (arg == "--help") {
            std::cout << "Usage: game_automation_example [option]\n\n";
            std::cout << "Options:\n";
            std::cout << "  --launch  Launch game and navigate menu\n";
            std::cout << "  --smart   Smart click with fallback methods\n";
            std::cout << "  --screen  Screen analysis (image/text/color)\n";
            std::cout << "  --ui      UI automation (accessibility)\n";
            std::cout << "  --state   Game state detection\n";
            std::cout << "  --yaml    Load automation from YAML\n";
            std::cout << "  --play    Automated gameplay\n";
            std::cout << "  --help    Show this help\n";
        }
    } else {
        // Run all examples
        Example_LaunchAndNavigate();
        Example_SmartClick();
        Example_ScreenAnalysis();
        Example_UIAutomation();
        Example_GameStateDetection();
        Example_PlayGame();
    }
    
    return 0;
}
