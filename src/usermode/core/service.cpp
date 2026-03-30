// service.cpp - Windows Service Implementation for KVMService
#include "service.h"
#include <iostream>
#include "../remote/native/websocket_server_async.h"
#include "../remote/vnc/vnc_server.h"

static void* g_wsServer  = nullptr;
static KVMDrivers::Remote::VNCServer* g_vncServer = nullptr;

SERVICE_STATUS g_ServiceStatus = { 0 };
SERVICE_STATUS_HANDLE g_StatusHandle = NULL;
HANDLE g_ServiceStopEvent = INVALID_HANDLE_VALUE;

VOID WINAPI ServiceMain(DWORD argc, LPWSTR* argv) {
    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    g_StatusHandle = RegisterServiceCtrlHandler(L"KVMService", ServiceCtrlHandler);
    if (g_StatusHandle == NULL) {
        return;
    }

    // Tell SCM we're starting
    g_ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_ServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    g_ServiceStatus.dwCurrentState = SERVICE_START_PENDING;
    g_ServiceStatus.dwWin32ExitCode = 0;
    g_ServiceStatus.dwServiceSpecificExitCode = 0;
    g_ServiceStatus.dwCheckPoint = 0;
    g_ServiceStatus.dwWaitHint = 0;

    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);

    // Create stop event
    g_ServiceStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (g_ServiceStopEvent == NULL) {
        g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
        g_ServiceStatus.dwWin32ExitCode = GetLastError();
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
        return;
    }

    // Initialize driver communication
    if (!InitializeDriverInterface()) {
        g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
        g_ServiceStatus.dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR;
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
        return;
    }

    // Start protocol servers
    if (!StartProtocolServers()) {
        g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
        g_ServiceStatus.dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR;
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
        return;
    }

    // Signal we're running
    g_ServiceStatus.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);

    // Service loop
    while (WaitForSingleObject(g_ServiceStopEvent, 100) != WAIT_OBJECT_0) {
        // Process driver events, protocol messages, etc.
        ProcessServiceTasks();
    }

    // Cleanup
    StopProtocolServers();
    CleanupDriverInterface();

    g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
}

VOID WINAPI ServiceCtrlHandler(DWORD CtrlCode) {
    switch (CtrlCode) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        g_ServiceStatus.dwCurrentState = SERVICE_STOP_PENDING;
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
        SetEvent(g_ServiceStopEvent);
        break;

    default:
        break;
    }
}

VOID InstallService() {
    SC_HANDLE scm = OpenSCManager(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (scm == NULL) {
        std::wcerr << L"OpenSCManager failed: " << GetLastError() << std::endl;
        return;
    }

    WCHAR path[MAX_PATH];
    GetModuleFileName(NULL, path, MAX_PATH);

    SC_HANDLE service = CreateService(
        scm,
        L"KVMService",
        L"KVM Remote Control Service",
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL,
        path,
        NULL, NULL, NULL, NULL, NULL
    );

    if (service == NULL) {
        std::wcerr << L"CreateService failed: " << GetLastError() << std::endl;
        CloseServiceHandle(scm);
        return;
    }

    std::wcout << L"Service installed successfully" << std::endl;
    CloseServiceHandle(service);
    CloseServiceHandle(scm);
}

VOID UninstallService() {
    SC_HANDLE scm = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
    if (scm == NULL) {
        return;
    }

    SC_HANDLE service = OpenService(scm, L"KVMService", DELETE | SERVICE_STOP);
    if (service == NULL) {
        CloseServiceHandle(scm);
        return;
    }

    SERVICE_STATUS status;
    ControlService(service, SERVICE_CONTROL_STOP, &status);
    DeleteService(service);

    std::wcout << L"Service uninstalled" << std::endl;
    CloseServiceHandle(service);
    CloseServiceHandle(scm);
}

BOOL InitializeDriverInterface() {
    // Each server manages its own DriverInterface; nothing global needed here.
    return TRUE;
}

VOID CleanupDriverInterface() {
    // Servers own their DriverInterface instances; cleaned up in StopProtocolServers.
}

BOOL StartProtocolServers() {
    // --- Async WebSocket server (JSON-RPC input injection, port 8443) ---
    g_wsServer = WsAsync_Create(8443);
    if (!g_wsServer || !WsAsync_Start(g_wsServer)) {
        std::wcerr << L"[Service] Failed to start WebSocket server on port 8443" << std::endl;
        if (g_wsServer) { WsAsync_Destroy(g_wsServer); g_wsServer = nullptr; }
        return FALSE;
    }
    std::wcout << L"[Service] WebSocket server started on port 8443" << std::endl;

    // --- VNC server (RFB 3.8, port 5900) ---
    g_vncServer = new KVMDrivers::Remote::VNCServer();
    if (!g_vncServer->Start()) {
        std::wcerr << L"[Service] Failed to start VNC server on port 5900 (non-fatal)" << std::endl;
        delete g_vncServer;
        g_vncServer = nullptr;
        // Non-fatal: service still operates via WebSocket
    } else {
        std::wcout << L"[Service] VNC server started on port 5900" << std::endl;
    }

    return TRUE;
}

VOID StopProtocolServers() {
    if (g_wsServer) {
        WsAsync_Stop(g_wsServer);
        WsAsync_Destroy(g_wsServer);
        g_wsServer = nullptr;
        std::wcout << L"[Service] WebSocket server stopped" << std::endl;
    }
    if (g_vncServer) {
        g_vncServer->Stop();
        delete g_vncServer;
        g_vncServer = nullptr;
        std::wcout << L"[Service] VNC server stopped" << std::endl;
    }
}

VOID ProcessServiceTasks() {
    // Lightweight 100ms tick: currently no pending background tasks.
    // Connection approval polling is handled inside ConnectionAuthGate::Evaluate()
    // which blocks its own accept thread; no action needed here.
}
