// KVMService.cpp - Core Windows Service
#include <windows.h>
#include <shlobj.h>
#include "service.h"
#include "../../common/logging/unified_logger.h"

// Declared in service.cpp; exposed so wmain can call it for standalone mode too.
extern void InitServiceLogger();
extern LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep);

int wmain(int argc, wchar_t* argv[]) {
    if (argc > 1 && _wcsicmp(argv[1], L"install") == 0) {
        InstallService();
        return 0;
    }
    if (argc > 1 && _wcsicmp(argv[1], L"uninstall") == 0) {
        UninstallService();
        return 0;
    }

    // Logger and crash handler active for all code paths from here on.
    InitServiceLogger();
    SetUnhandledExceptionFilter(CrashHandler);
    KVM_LOG_INFO("Main", "KVMService starting  argc=%d", argc);

    // --standalone: run directly from the command line without SCM.
    bool standalone = false;
    for (int i = 1; i < argc; i++) {
        if (_wcsicmp(argv[i], L"--standalone") == 0 ||
            _wcsicmp(argv[i], L"-standalone")  == 0) {
            standalone = true;
            break;
        }
    }

    if (standalone) {
        KVM_LOG_INFO("Main", "Running in STANDALONE mode (not registered with SCM)");

        if (!InitializeDriverInterface()) {
            KVM_LOG_WARN("Main", "Driver interface init failed — will use SendInput fallback");
        }
        if (!StartProtocolServers()) {
            KVM_LOG_FATAL("Main", "Failed to start protocol servers — exiting");
            UserLogger_FlushSync();
            UserLogger_Shutdown();
            return 1;
        }

        KVM_LOG_INFO("Main", "Servers running in standalone mode. Waiting for Ctrl+C or Enter.");

        SetConsoleCtrlHandler([](DWORD sig) -> BOOL {
            KVM_LOG_INFO("Main", "Console control signal %lu received — stopping", sig);
            StopProtocolServers();
            CleanupDriverInterface();
            UserLogger_FlushSync();
            UserLogger_Shutdown();
            ExitProcess(0);
            return TRUE;
        }, TRUE);

        // Block until Enter key for interactive console use
        (void)getchar();

        KVM_LOG_INFO("Main", "Stopping (Enter pressed)");
        StopProtocolServers();
        CleanupDriverInterface();
        UserLogger_FlushSync();
        UserLogger_Shutdown();
        return 0;
    }

    // Normal path: hand off to SCM.
    KVM_LOG_DEBUG("Main", "Handing off to StartServiceCtrlDispatcher");
    // Note: SCM will call ServiceMain which re-initializes the logger with the
    // same file path, updating level/categories only (no file stomp).
    SERVICE_TABLE_ENTRY dispatchTable[] = {
        { (LPWSTR)L"KVMService", ServiceMain },
        { NULL, NULL }
    };

    if (!StartServiceCtrlDispatcher(dispatchTable)) {
        DWORD err = GetLastError();
        if (err == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
            KVM_LOG_ERROR("Main",
                "Not running as a Windows service — use --standalone for CLI mode");
        } else {
            KVM_LOG_FATAL("Main", "StartServiceCtrlDispatcher failed: %lu", err);
        }
        UserLogger_FlushSync();
        UserLogger_Shutdown();
        return 1;
    }

    return 0;
}
