// service.cpp - Windows Service Implementation for KVMService
#include "service.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <thread>
#include <atomic>
#include <vector>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <shlobj.h>
#include <dbghelp.h>
#include "../remote/native/websocket_server_async.h"
#include "../remote/vnc/vnc_server.h"
#include "../../common/logging/unified_logger.h"

#pragma comment(lib, "dbghelp.lib")

// ── Log file path (%%PROGRAMDATA%%\KVM-Drivers\KVMService.log) ────────────────
void InitServiceLogger()
{
    wchar_t appDataW[MAX_PATH] = {};
    char    appDataA[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_COMMON_APPDATA, NULL, 0, appDataW))) {
        // Ensure directory exists
        std::wstring dir = std::wstring(appDataW) + L"\\KVM-Drivers";
        CreateDirectoryW(dir.c_str(), NULL);
        // Build narrow path for fopen
        WideCharToMultiByte(CP_UTF8, 0, appDataW, -1, appDataA, MAX_PATH, NULL, NULL);
    }
    char logPath[MAX_PATH];
    snprintf(logPath, sizeof(logPath), "%s\\KVM-Drivers\\KVMService.log", appDataA);
    UserLogger_Initialize(logPath, LOG_LEVEL_DEBUG, LOG_CATEGORY_ALL);
    KVM_LOG_INFO("Service", "Logger initialized. Log file: %s", logPath);
}

// ── SEH crash handler — flush log before the process dies ────────────────────
LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep)
{
    KVM_LOG_FATAL("Service",
        "UNHANDLED EXCEPTION: code=0x%08lX addr=%p — flushing log and exiting",
        ep->ExceptionRecord->ExceptionCode,
        ep->ExceptionRecord->ExceptionAddress);

    // Write a minidump alongside the log for post-mortem analysis
    wchar_t appDataW[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_COMMON_APPDATA, NULL, 0, appDataW))) {
        std::wstring dumpPath = std::wstring(appDataW) + L"\\KVM-Drivers\\KVMService_crash.dmp";
        HANDLE hFile = CreateFileW(dumpPath.c_str(), GENERIC_WRITE,
            0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mei;
            mei.ThreadId          = GetCurrentThreadId();
            mei.ExceptionPointers = ep;
            mei.ClientPointers    = FALSE;
            MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                hFile, MiniDumpWithDataSegs, &mei, NULL, NULL);
            CloseHandle(hFile);
        }
    }

    UserLogger_FlushSync();   // drain ring buffer before exit
    return EXCEPTION_EXECUTE_HANDLER;
}

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shell32.lib")

static void* g_wsServer  = nullptr;
static KVMDrivers::Remote::VNCServer* g_vncServer = nullptr;

// ── Minimal single-file HTTP server ─────────────────────────────────────────
// Serves the KVM web client (index.html) on HTTP so users can open
//   http://<host>:8080   in any browser to get the full remote desktop UI.
//
// Resolution order for index.html:
//   1. <ExeDir>\webclient\index.html  (release layout)
//   2. <ExeDir>\index.html            (flat layout)
//   3. src\webclient\index.html relative to the exe (dev tree layout)

static std::string g_indexHtml;       // cached file contents
static std::string g_indexHtmlPath;   // path that was loaded
static std::thread g_httpThread;
static std::atomic<bool> g_httpRunning{false};
static SOCKET g_httpListenSocket = INVALID_SOCKET;
static int    g_httpPort         = 8080;

static std::string ResolveWebClientPath()
{
    // Get the directory that contains KVMService.exe
    wchar_t exeW[MAX_PATH] = {};
    GetModuleFileNameW(NULL, exeW, MAX_PATH);
    std::wstring exeDir(exeW);
    auto slash = exeDir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) exeDir = exeDir.substr(0, slash);

    auto tryW = [](const std::wstring& p) -> std::string {
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) {
            // Convert to narrow string
            std::string n(p.begin(), p.end());
            return n;
        }
        return {};
    };

    std::string found;
    if (!(found = tryW(exeDir + L"\\webclient\\index.html")).empty()) return found;
    if (!(found = tryW(exeDir + L"\\index.html")).empty())             return found;
    // Dev tree: walk up looking for src\webclient\index.html
    std::wstring dir = exeDir;
    for (int i = 0; i < 6 && !dir.empty(); i++) {
        if (!(found = tryW(dir + L"\\src\\webclient\\index.html")).empty()) return found;
        auto p = dir.find_last_of(L"\\/");
        if (p == std::wstring::npos) break;
        dir = dir.substr(0, p);
    }
    return {};
}

static std::string GetLocalIPv4()
{
    char hostname[256] = {};
    if (gethostname(hostname, sizeof(hostname)) != 0) return "localhost";
    addrinfo hints = {}, *info = nullptr;
    hints.ai_family = AF_INET;
    if (getaddrinfo(hostname, nullptr, &hints, &info) != 0) return "localhost";
    char ip[INET_ADDRSTRLEN] = "localhost";
    if (info) inet_ntop(AF_INET, &((sockaddr_in*)info->ai_addr)->sin_addr, ip, sizeof(ip));
    freeaddrinfo(info);
    return ip;
}

static void HttpWorker(SOCKET client)
{
    char buf[4096] = {};
    recv(client, buf, sizeof(buf) - 1, 0);

    // We only serve GET / and GET /index.html — ignore everything else
    bool ok = (strncmp(buf, "GET / ", 6) == 0 ||
               strncmp(buf, "GET /index.html ", 16) == 0 ||
               strncmp(buf, "GET /favicon.ico", 16) == 0);

    if (ok && strncmp(buf, "GET /favicon.ico", 16) == 0) {
        // Return minimal 204 No Content for favicon
        const char* r = "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n";
        send(client, r, (int)strlen(r), 0);
    } else if (ok && !g_indexHtml.empty()) {
        std::string response =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html; charset=utf-8\r\n"
            "Content-Length: " + std::to_string(g_indexHtml.size()) + "\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: close\r\n\r\n" + g_indexHtml;
        send(client, response.data(), (int)response.size(), 0);
    } else {
        const char* body = "<h1>KVM Web Client</h1><p>index.html not found beside KVMService.exe.</p>";
        std::string response =
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Type: text/html\r\n"
            "Content-Length: " + std::to_string(strlen(body)) + "\r\n"
            "Connection: close\r\n\r\n" + body;
        send(client, response.data(), (int)response.size(), 0);
    }
    closesocket(client);
}

static void HttpLoop()
{
    while (g_httpRunning) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(g_httpListenSocket, &rd);
        timeval tv = {0, 100000};
        if (select(0, &rd, nullptr, nullptr, &tv) <= 0) continue;

        SOCKET client = accept(g_httpListenSocket, nullptr, nullptr);
        if (client == INVALID_SOCKET) continue;

        // Handle inline — requests are tiny; no need for per-client thread
        DWORD timeout = 3000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout));
        HttpWorker(client);
    }
}

static bool StartHttpServer(int port)
{
    std::string path = ResolveWebClientPath();
    if (!path.empty()) {
        std::ifstream f(path, std::ios::binary);
        g_indexHtml = std::string(
            std::istreambuf_iterator<char>(f),
            std::istreambuf_iterator<char>());
        g_indexHtmlPath = path;
        KVM_LOG_INFO("HTTP", "Serving web client from: %s (%zu bytes)",
            path.c_str(), g_indexHtml.size());
    } else {
        KVM_LOG_WARN("HTTP", "index.html not found beside KVMService.exe — HTTP will return 404");
    }

    g_httpListenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_httpListenSocket == INVALID_SOCKET) {
        KVM_LOG_ERROR("HTTP", "socket() failed: %d", WSAGetLastError());
        return false;
    }

    int opt = 1;
    setsockopt(g_httpListenSocket, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

    sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((u_short)port);

    if (bind(g_httpListenSocket, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        KVM_LOG_ERROR("HTTP", "bind port %d failed: %d", port, WSAGetLastError());
        closesocket(g_httpListenSocket);
        g_httpListenSocket = INVALID_SOCKET;
        return false;
    }
    if (listen(g_httpListenSocket, 10) == SOCKET_ERROR) {
        KVM_LOG_ERROR("HTTP", "listen() failed: %d", WSAGetLastError());
        closesocket(g_httpListenSocket);
        g_httpListenSocket = INVALID_SOCKET;
        return false;
    }

    g_httpPort    = port;
    g_httpRunning = true;
    g_httpThread  = std::thread(HttpLoop);

    std::string ip = GetLocalIPv4();
    KVM_LOG_INFO("HTTP", "Web client listening on http://%s:%d/ (also http://localhost:%d/)",
        ip.c_str(), port, port);
    return true;
}

static void StopHttpServer()
{
    g_httpRunning = false;
    if (g_httpListenSocket != INVALID_SOCKET) {
        closesocket(g_httpListenSocket);
        g_httpListenSocket = INVALID_SOCKET;
    }
    if (g_httpThread.joinable()) g_httpThread.join();
}
// ────────────────────────────────────────────────────────────────────────────

SERVICE_STATUS g_ServiceStatus = { 0 };
SERVICE_STATUS_HANDLE g_StatusHandle = NULL;
HANDLE g_ServiceStopEvent = INVALID_HANDLE_VALUE;

VOID WINAPI ServiceMain(DWORD argc, LPWSTR* argv) {
    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    // ── Logger must be first — everything else logs to it ────────────────────
    InitServiceLogger();
    SetUnhandledExceptionFilter(CrashHandler);

    KVM_LOG_INFO("Service", "ServiceMain entered — registering SCM control handler");

    g_StatusHandle = RegisterServiceCtrlHandler(L"KVMService", ServiceCtrlHandler);
    if (g_StatusHandle == NULL) {
        KVM_LOG_FATAL("Service", "RegisterServiceCtrlHandler failed: %lu", GetLastError());
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
        DWORD err = GetLastError();
        KVM_LOG_FATAL("Service", "CreateEvent(stop) failed: %lu", err);
        g_ServiceStatus.dwCurrentState  = SERVICE_STOPPED;
        g_ServiceStatus.dwWin32ExitCode = err;
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
        UserLogger_FlushSync();
        return;
    }

    KVM_LOG_INFO("Service", "Initializing driver interface");
    if (!InitializeDriverInterface()) {
        KVM_LOG_FATAL("Service", "InitializeDriverInterface() failed — aborting start");
        CloseHandle(g_ServiceStopEvent);
        g_ServiceStopEvent = INVALID_HANDLE_VALUE;
        g_ServiceStatus.dwCurrentState  = SERVICE_STOPPED;
        g_ServiceStatus.dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR;
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
        UserLogger_FlushSync();
        return;
    }

    KVM_LOG_INFO("Service", "Starting protocol servers");
    if (!StartProtocolServers()) {
        KVM_LOG_FATAL("Service", "StartProtocolServers() failed — aborting start");
        CloseHandle(g_ServiceStopEvent);
        g_ServiceStopEvent = INVALID_HANDLE_VALUE;
        g_ServiceStatus.dwCurrentState  = SERVICE_STOPPED;
        g_ServiceStatus.dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR;
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
        UserLogger_FlushSync();
        return;
    }

    g_ServiceStatus.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
    KVM_LOG_INFO("Service", "KVMService is RUNNING");

    while (WaitForSingleObject(g_ServiceStopEvent, 100) != WAIT_OBJECT_0) {
        ProcessServiceTasks();
    }

    KVM_LOG_INFO("Service", "Stop event signalled — shutting down");
    StopProtocolServers();
    CleanupDriverInterface();
    CloseHandle(g_ServiceStopEvent);
    g_ServiceStopEvent = INVALID_HANDLE_VALUE;

    g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
    KVM_LOG_INFO("Service", "KVMService stopped cleanly");
    UserLogger_FlushSync();
    UserLogger_Shutdown();
}

VOID WINAPI ServiceCtrlHandler(DWORD CtrlCode) {
    switch (CtrlCode) {
    case SERVICE_CONTROL_STOP:
        KVM_LOG_INFO("Service", "SCM STOP control received");
        g_ServiceStatus.dwCurrentState = SERVICE_STOP_PENDING;
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
        SetEvent(g_ServiceStopEvent);
        break;
    case SERVICE_CONTROL_SHUTDOWN:
        KVM_LOG_INFO("Service", "SCM SHUTDOWN control received");
        g_ServiceStatus.dwCurrentState = SERVICE_STOP_PENDING;
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
        SetEvent(g_ServiceStopEvent);
        break;
    default:
        KVM_LOG_DEBUG("Service", "SCM control code %lu ignored", CtrlCode);
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

// Read a single integer value from the tray's JSON settings file.
// Uses CSIDL_COMMON_APPDATA (%PROGRAMDATA%) so the file is accessible
// from both the tray (user account) and the service (LocalService account).
static int ReadSettingInt(const char* key, int defaultVal)
{
    wchar_t appData[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_COMMON_APPDATA, NULL, 0, appData)))
        return defaultVal;

    std::wstring path = std::wstring(appData) + L"\\KVM-Drivers\\settings.json";
    std::ifstream f(path);
    if (!f.is_open()) return defaultVal;

    std::string json((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());

    // Search for  "key": <digits>
    std::string needle = std::string("\"") + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return defaultVal;

    pos = json.find_first_of("0123456789", pos + needle.size());
    if (pos == std::string::npos) return defaultVal;

    try { return std::stoi(json.substr(pos)); }
    catch (...) { return defaultVal; }
}

BOOL StartProtocolServers() {
    int wsMaxClients  = ReadSettingInt("WsMaxClients",  10);
    int vncMaxClients = ReadSettingInt("VncMaxClients", 10);
    KVM_LOG_INFO("Service", "Settings: WsMaxClients=%d  VncMaxClients=%d",
        wsMaxClients, vncMaxClients);

    // --- Async WebSocket server (JSON-RPC input injection, port 8443) ---
    KVM_LOG_INFO("Service", "Starting WebSocket server on port 8443 (max %d clients)", wsMaxClients);
    g_wsServer = WsAsync_Create(8443, wsMaxClients);
    if (!g_wsServer) {
        KVM_LOG_ERROR("Service", "WsAsync_Create() returned null");
        return FALSE;
    }
    if (!WsAsync_Start(g_wsServer)) {
        KVM_LOG_ERROR("Service", "WsAsync_Start() failed — port 8443 may already be in use");
        WsAsync_Destroy(g_wsServer);
        g_wsServer = nullptr;
        return FALSE;
    }
    KVM_LOG_INFO("Service", "WebSocket server STARTED on port 8443");

    // --- VNC server (RFB 3.8, port 5900) ---
    KVM_LOG_INFO("Service", "Starting VNC server on port 5900 (max %d clients)", vncMaxClients);
    g_vncServer = new KVMDrivers::Remote::VNCServer(vncMaxClients);
    if (!g_vncServer->Start()) {
        KVM_LOG_WARN("Service",
            "VNC server failed to start on port 5900 (non-fatal — WS still active)");
        delete g_vncServer;
        g_vncServer = nullptr;
    } else {
        KVM_LOG_INFO("Service", "VNC server STARTED on port 5900");
    }

    // --- HTTP server (serves web client index.html, port 8080) ---
    KVM_LOG_INFO("Service", "Starting HTTP web client server on port 8080");
    if (!StartHttpServer(8080)) {
        KVM_LOG_WARN("Service",
            "HTTP server failed to start on port 8080 (non-fatal — open index.html directly)");
    }

    return TRUE;
}

VOID StopProtocolServers() {
    KVM_LOG_INFO("Service", "Stopping HTTP server");
    StopHttpServer();
    if (g_wsServer) {
        KVM_LOG_INFO("Service", "Stopping WebSocket server");
        WsAsync_Stop(g_wsServer);
        WsAsync_Destroy(g_wsServer);
        g_wsServer = nullptr;
        KVM_LOG_INFO("Service", "WebSocket server stopped");
    }
    if (g_vncServer) {
        KVM_LOG_INFO("Service", "Stopping VNC server");
        g_vncServer->Stop();
        delete g_vncServer;
        g_vncServer = nullptr;
        KVM_LOG_INFO("Service", "VNC server stopped");
    }
}

VOID ProcessServiceTasks() {
    // Lightweight 100ms tick: currently no pending background tasks.
    // Connection approval polling is handled inside ConnectionAuthGate::Evaluate()
    // which blocks its own accept thread; no action needed here.
}
