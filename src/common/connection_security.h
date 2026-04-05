// connection_security.h - M7 security features:
//   1. ETW per-connection audit logging
//   2. Certificate pinning (reject connections not matching pinned hash)
//   3. Mutual authentication config (require client TLS certificates)
//   4. IP allowlist enforcement

#pragma once
#define SECURITY_WIN32
#include <windows.h>
#include <winsock2.h>
#include <wincrypt.h>
#include <evntprov.h>
#include <evntrace.h>
#include <winmeta.h>
#include <sspi.h>
#include <schannel.h>
#include <shlobj.h>
#include <ole2.h>
#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <functional>
#include <mutex>
#include <atomic>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

#pragma comment(lib, "crypt32.lib")

// ─────────────────────────────────────────────────────────────────────────────
// ETW Audit Logger
// Provider GUID: {B3D5A9B0-4C2E-4E6F-8D3A-1F2C3D4E5F60}
// ─────────────────────────────────────────────────────────────────────────────

// {B3D5A9B0-4C2E-4E6F-8D3A-1F2C3D4E5F60}
static const GUID KVM_ETW_PROVIDER_GUID = {
    0xB3D5A9B0, 0x4C2E, 0x4E6F,
    { 0x8D, 0x3A, 0x1F, 0x2C, 0x3D, 0x4E, 0x5F, 0x60 }
};

// ETW event IDs
constexpr USHORT ETW_EVENT_CONNECT         = 100;
constexpr USHORT ETW_EVENT_DISCONNECT      = 101;
constexpr USHORT ETW_EVENT_AUTH_SUCCESS    = 102;
constexpr USHORT ETW_EVENT_AUTH_FAIL       = 103;
constexpr USHORT ETW_EVENT_INPUT_INJECTED  = 104;
constexpr USHORT ETW_EVENT_CERT_REJECTED   = 105;
constexpr USHORT ETW_EVENT_IP_BLOCKED      = 106;
constexpr USHORT ETW_EVENT_RATE_LIMITED    = 107;

class EtwAuditLogger {
public:
    EtwAuditLogger() : providerHandle_(0), enabled_(false) {}

    ~EtwAuditLogger() {
        if (enabled_) {
            EventUnregister(providerHandle_);
        }
    }

    // Register ETW provider. Call once at startup.
    bool Initialize() {
        ULONG status = EventRegister(
            &KVM_ETW_PROVIDER_GUID,
            EnableCallback, this,
            &providerHandle_);
        if (status != ERROR_SUCCESS) {
            std::cerr << "[ETW] EventRegister failed: " << status << std::endl;
            return false;
        }
        enabled_ = true;
        std::cout << "[ETW] Audit provider registered" << std::endl;
        return true;
    }

    // Log a connection event
    void LogConnect(const std::string& clientIP, const std::string& protocol) {
        WriteAuditEvent(ETW_EVENT_CONNECT, "CONNECT", clientIP, protocol, "");
    }

    void LogDisconnect(const std::string& clientIP, const std::string& protocol,
                       const std::string& reason = "") {
        WriteAuditEvent(ETW_EVENT_DISCONNECT, "DISCONNECT", clientIP, protocol, reason);
    }

    void LogAuthSuccess(const std::string& clientIP, const std::string& protocol,
                        const std::string& method) {
        WriteAuditEvent(ETW_EVENT_AUTH_SUCCESS, "AUTH_SUCCESS", clientIP, protocol, method);
    }

    void LogAuthFail(const std::string& clientIP, const std::string& protocol,
                     const std::string& reason) {
        WriteAuditEvent(ETW_EVENT_AUTH_FAIL, "AUTH_FAIL", clientIP, protocol, reason);
    }

    void LogInputInjected(const std::string& clientIP, const std::string& inputType) {
        WriteAuditEvent(ETW_EVENT_INPUT_INJECTED, "INPUT", clientIP, "", inputType);
    }

    void LogCertRejected(const std::string& clientIP, const std::string& thumbprint) {
        WriteAuditEvent(ETW_EVENT_CERT_REJECTED, "CERT_REJECTED", clientIP, "TLS", thumbprint);
    }

    void LogIpBlocked(const std::string& clientIP) {
        WriteAuditEvent(ETW_EVENT_IP_BLOCKED, "IP_BLOCKED", clientIP, "", "");
    }

    void LogRateLimited(const std::string& clientIP, int limitPerSec) {
        std::string details = "limit=" + std::to_string(limitPerSec);
        WriteAuditEvent(ETW_EVENT_RATE_LIMITED, "RATE_LIMITED", clientIP, "", details);
    }

    bool IsEnabled() const { return enabled_; }

private:
    REGHANDLE        providerHandle_;
    std::atomic<bool> enabled_;

    static VOID NTAPI EnableCallback(LPCGUID, ULONG, UCHAR, ULONGLONG,
                                     ULONGLONG, PEVENT_FILTER_DESCRIPTOR, PVOID) {}

    void WriteAuditEvent(USHORT eventId, const char* eventType,
                         const std::string& clientIP, const std::string& protocol,
                         const std::string& details) {
        if (!enabled_) return;

        // Build the event descriptor
        EVENT_DESCRIPTOR desc = {};
        desc.Id      = eventId;
        desc.Version = 0;
        desc.Channel = 0;
        desc.Level   = WINEVENT_LEVEL_INFO;
        desc.Opcode  = 0;

        // Build string payload: "EventType|ClientIP|Protocol|Details"
        std::string payload = std::string(eventType) + "|" +
                              clientIP + "|" + protocol + "|" + details;
        std::wstring wPayload(payload.begin(), payload.end());

        EVENT_DATA_DESCRIPTOR dataDesc;
        EventDataDescCreate(&dataDesc, wPayload.c_str(),
            (ULONG)((wPayload.size() + 1) * sizeof(wchar_t)));

        EventWrite(providerHandle_, &desc, 1, &dataDesc);

        // Also output to stderr for immediate visibility
        std::cerr << "[AUDIT] " << payload << std::endl;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Certificate Pinning
// ─────────────────────────────────────────────────────────────────────────────

class CertificatePinner {
public:
    // Add a pinned SHA-1 thumbprint (hex string, no spaces)
    void AddPinnedThumbprint(const std::string& thumbprintHex) {
        std::string normalized = thumbprintHex;
        // Remove spaces and convert to uppercase
        normalized.erase(std::remove(normalized.begin(), normalized.end(), ' '),
                         normalized.end());
        std::transform(normalized.begin(), normalized.end(),
                       normalized.begin(), ::toupper);
        std::lock_guard<std::mutex> lk(mutex_);
        pinnedThumbprints_.push_back(normalized);
        std::cout << "[CertPin] Pinned: " << normalized << std::endl;
    }

    bool IsEmpty() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return pinnedThumbprints_.empty();
    }

    // Validate a certificate against pinned thumbprints
    // Returns true if cert matches any pin (or if no pins configured = allow all)
    bool Validate(PCCERT_CONTEXT cert, std::string& outThumbprint) const {
        if (!cert) return false;

        // Compute SHA-1 thumbprint of the certificate
        BYTE thumb[20];
        DWORD thumbLen = sizeof(thumb);
        if (!CertGetCertificateContextProperty(cert, CERT_HASH_PROP_ID,
                thumb, &thumbLen)) {
            return false;
        }

        std::ostringstream ss;
        for (DWORD i = 0; i < thumbLen; i++) {
            ss << std::uppercase << std::hex << std::setw(2)
               << std::setfill('0') << (int)thumb[i];
        }
        outThumbprint = ss.str();

        std::lock_guard<std::mutex> lk(mutex_);
        if (pinnedThumbprints_.empty()) return true;  // no pins = accept all

        return std::find(pinnedThumbprints_.begin(), pinnedThumbprints_.end(),
                         outThumbprint) != pinnedThumbprints_.end();
    }

    void Clear() {
        std::lock_guard<std::mutex> lk(mutex_);
        pinnedThumbprints_.clear();
    }

private:
    mutable std::mutex      mutex_;
    std::vector<std::string> pinnedThumbprints_;
};

// ─────────────────────────────────────────────────────────────────────────────
// IP Allowlist Enforcement
// ─────────────────────────────────────────────────────────────────────────────

class IpAllowlist {
public:
    // Add an allowed IP or CIDR (e.g. "192.168.1.0/24" or "10.0.0.5")
    void Add(const std::string& entry) {
        std::lock_guard<std::mutex> lk(mutex_);
        entries_.push_back(entry);
    }

    void SetEntries(const std::vector<std::string>& entries) {
        std::lock_guard<std::mutex> lk(mutex_);
        entries_ = entries;
    }

    bool IsEmpty() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return entries_.empty();
    }

    // Returns true if IP is allowed (or list is empty = allow all)
    bool IsAllowed(const std::string& ip) const {
        std::lock_guard<std::mutex> lk(mutex_);
        if (entries_.empty()) return true;

        for (const auto& entry : entries_) {
            if (entry.find('/') != std::string::npos) {
                if (MatchCidr(ip, entry)) return true;
            } else {
                if (ip == entry) return true;
            }
        }
        return false;
    }

private:
    mutable std::mutex      mutex_;
    std::vector<std::string> entries_;

    // Simple CIDR matching (IPv4 only)
    static bool MatchCidr(const std::string& ip, const std::string& cidr) {
        size_t slash = cidr.find('/');
        if (slash == std::string::npos) return ip == cidr;

        std::string netStr  = cidr.substr(0, slash);
        int         prefix  = std::stoi(cidr.substr(slash + 1));

        DWORD ipAddr  = ParseIPv4(ip);
        DWORD netAddr = ParseIPv4(netStr);
        if (ipAddr == INADDR_NONE || netAddr == INADDR_NONE) return false;

        DWORD mask = prefix > 0 ? (~0u << (32 - prefix)) : 0u;
        return (ntohl(ipAddr) & mask) == (ntohl(netAddr) & mask);
    }

    static DWORD ParseIPv4(const std::string& ip) {
        return inet_addr(ip.c_str());
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Mutual Authentication Configuration
// ─────────────────────────────────────────────────────────────────────────────

struct MutualAuthConfig {
    bool requireClientCert = false;          // If true, reject connections without cert
    bool validateClientCertChain = true;     // Validate cert chain against trusted CAs
    CertificatePinner* pinner = nullptr;     // If set, pin specific client certs

    // Schannel flags for ASC_REQ_MUTUAL_AUTH
    ULONG GetSchannelFlags() const {
        ULONG flags = ASC_REQ_SEQUENCE_DETECT | ASC_REQ_REPLAY_DETECT |
                      ASC_REQ_CONFIDENTIALITY | ASC_REQ_STREAM;
        if (requireClientCert) {
            flags |= ASC_REQ_MUTUAL_AUTH;
        }
        return flags;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Trusted Clients Store
// Persists IP → trust-expiry pairs to disk so approved clients auto-reconnect.
// Written by the C# tray app; read by C++ servers.
// File: %PROGRAMDATA%\KVM-Drivers\trusted_clients.txt  (simple key-value text)
// ─────────────────────────────────────────────────────────────────────────────

class TrustedClientsStore {
public:
    struct TrustedEntry {
        std::string ip;
        LONGLONG    expiryEpoch;  // 0 = permanent
    };

    void Load() {
        std::lock_guard<std::mutex> lk(mutex_);
        entries_.clear();
        std::string path = GetFilePath();
        FILE* f = nullptr;
        if (fopen_s(&f, path.c_str(), "r") != 0 || !f) return;

        char line[256];
        while (fgets(line, sizeof(line), f)) {
            // Format: "IP EXPIRY_EPOCH\n"  (EXPIRY_EPOCH = 0 means permanent)
            char ip[64] = {};
            LONGLONG expiry = 0;
            if (sscanf_s(line, "%63s %lld", ip, (unsigned)sizeof(ip), &expiry) >= 1) {
                entries_.push_back({ ip, expiry });
            }
        }
        fclose(f);
    }

    void Save() const {
        std::lock_guard<std::mutex> lk(mutex_);
        std::string path = GetFilePath();
        FILE* f = nullptr;
        if (fopen_s(&f, path.c_str(), "w") != 0 || !f) return;
        for (const auto& e : entries_) {
            fprintf(f, "%s %lld\n", e.ip.c_str(), e.expiryEpoch);
        }
        fclose(f);
    }

    // Add or refresh a trusted entry
    void Trust(const std::string& ip, int durationMinutes = 0) {
        std::lock_guard<std::mutex> lk(mutex_);
        LONGLONG expiry = 0;
        if (durationMinutes > 0) {
            FILETIME ft;
            GetSystemTimeAsFileTime(&ft);
            LONGLONG now = (((LONGLONG)ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
            expiry = now + (LONGLONG)durationMinutes * 60 * 10000000LL;
        }
        for (auto& e : entries_) {
            if (e.ip == ip) { e.expiryEpoch = expiry; return; }
        }
        entries_.push_back({ ip, expiry });
    }

    void Revoke(const std::string& ip) {
        std::lock_guard<std::mutex> lk(mutex_);
        entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
            [&](const TrustedEntry& e) { return e.ip == ip; }), entries_.end());
    }

    bool IsTrusted(const std::string& ip) const {
        std::lock_guard<std::mutex> lk(mutex_);
        for (const auto& e : entries_) {
            if (e.ip != ip) continue;
            if (e.expiryEpoch == 0) return true;  // permanent
            FILETIME ft;
            GetSystemTimeAsFileTime(&ft);
            LONGLONG now = (((LONGLONG)ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
            if (now < e.expiryEpoch) return true;
        }
        return false;
    }

    // Returns a snapshot (copy) so callers don't need to hold the mutex.
    std::vector<TrustedEntry> GetEntries() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return entries_;
    }

private:
    mutable std::mutex        mutex_;
    std::vector<TrustedEntry> entries_;

    static std::string GetFilePath() {
        char path[MAX_PATH] = {};
        // Use CSIDL_COMMON_APPDATA (%PROGRAMDATA%) so the file is accessible
        // from both the service (LocalService) and the tray (interactive user).
        if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_COMMON_APPDATA, NULL, 0, path))) {
            return std::string(path) + "\\KVM-Drivers\\trusted_clients.txt";
        }
        return "trusted_clients.txt";
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Approval Request Store (file-based IPC between C++ servers and C# tray)
// C++ writes pending requests; C# polls, shows dialog, writes results back.
// ─────────────────────────────────────────────────────────────────────────────

struct ApprovalRequest {
    std::string requestId;   // UUID-style unique ID
    std::string clientIP;
    std::string protocol;
    std::string timestamp;
    bool        isAuthenticated;
};

class ApprovalRequestStore {
public:
    // C++ server: enqueue a pending approval request. Returns requestId.
    std::string EnqueueRequest(const std::string& clientIP,
                               const std::string& protocol,
                               bool isAuthenticated) {
        ApprovalRequest req;
        req.requestId     = GenerateId();
        req.clientIP      = clientIP;
        req.protocol      = protocol;
        req.isAuthenticated = isAuthenticated;
        req.timestamp     = CurrentIso8601();

        WriteRequestFile(req);
        return req.requestId;
    }

    // C++ server: poll until the result file appears or timeout (ms)
    // Returns "approved", "rejected", "timedout", or "blocked"
    std::string WaitForResult(const std::string& requestId, int timeoutMs = 35000) {
        std::string resultPath = GetResultPath(requestId);
        auto deadline = GetTickCount64() + (ULONGLONG)timeoutMs;

        while (GetTickCount64() < deadline) {
            FILE* f = nullptr;
            if (fopen_s(&f, resultPath.c_str(), "r") == 0 && f) {
                char buf[32] = {};
                fgets(buf, sizeof(buf), f);
                fclose(f);
                DeleteFileA(resultPath.c_str());
                DeleteFileA(GetRequestPath(requestId).c_str());
                std::string result(buf);
                // trim newline
                if (!result.empty() && result.back() == '\n') result.pop_back();
                return result;
            }
            Sleep(250);
        }
        // Timeout: clean up
        DeleteFileA(GetRequestPath(requestId).c_str());
        return "timedout";
    }

    // Get the pending-approvals directory. Uses CSIDL_COMMON_APPDATA (%PROGRAMDATA%)
    // so service and tray processes share the same IPC directory.
    static std::string GetPendingDir() {
        char path[MAX_PATH] = {};
        if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_COMMON_APPDATA, NULL, 0, path))) {
            return std::string(path) + "\\KVM-Drivers\\pending_approvals\\";
        }
        return ".\\pending_approvals\\";
    }

private:
    static std::string GetRequestPath(const std::string& id) {
        return GetPendingDir() + id + ".request";
    }
    static std::string GetResultPath(const std::string& id) {
        return GetPendingDir() + id + ".result";
    }

    void WriteRequestFile(const ApprovalRequest& req) {
        std::string dir = GetPendingDir();
        CreateDirectoryA(dir.c_str(), NULL);

        FILE* f = nullptr;
        std::string path = GetRequestPath(req.requestId);
        if (fopen_s(&f, path.c_str(), "w") != 0 || !f) return;
        fprintf(f, "id=%s\nip=%s\nprotocol=%s\ntimestamp=%s\nauthenticated=%d\n",
            req.requestId.c_str(), req.clientIP.c_str(), req.protocol.c_str(),
            req.timestamp.c_str(), req.isAuthenticated ? 1 : 0);
        fclose(f);
    }

    static std::string GenerateId() {
        GUID guid;
        CoCreateGuid(&guid);
        char buf[40];
        sprintf_s(buf, "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
            guid.Data1, guid.Data2, guid.Data3,
            guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
            guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
        return buf;
    }

    static std::string CurrentIso8601() {
        SYSTEMTIME st;
        GetSystemTime(&st);
        char buf[32];
        sprintf_s(buf, "%04d-%02d-%02dT%02d:%02d:%02dZ",
            st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond);
        return buf;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Connection Auth Gate
// Single entry point that enforces all auth policy:
//   1. Localhost (127.0.0.1 / ::1) → always allowed, no prompt
//   2. Token auth → if valid token supplied, allow immediately
//   3. Trusted client (previously approved) → allow immediately
//   4. TrustOnFirstUse mode → queue approval dialog, block until decision
//   5. RequireCredentials mode → reject if no valid token
// ─────────────────────────────────────────────────────────────────────────────

struct AuthGateConfig {
    bool requireRemoteAuth  = true;   // Require auth for non-localhost connections
    bool trustOnFirstUse    = true;   // Show approval dialog for unknown clients
    bool localhostBypass    = true;   // Always allow 127.0.0.1 / ::1 without auth
    std::string authToken   = "";     // Pre-shared token (empty = token auth disabled)
};

enum class AuthDecision {
    Allowed,           // connection permitted
    AllowedLocalhost,  // permitted because source is localhost
    AllowedToken,      // permitted because valid token supplied
    AllowedTrusted,    // permitted because in trusted clients list
    AllowedApproved,   // permitted because user approved via dialog
    Rejected,          // rejected (no valid auth, no approval)
    RejectedBlocked,   // permanently blocked by user
    RejectedTimeout,   // approval dialog timed out (auto-reject)
};

class ConnectionAuthGate {
public:
    AuthGateConfig        config;
    TrustedClientsStore   trustedClients;
    ApprovalRequestStore  approvalStore;

    ConnectionAuthGate() {
        trustedClients.Load();
    }

    // Evaluate whether a connection should be allowed.
    // suppliedToken: token provided by client (empty if none)
    // isAuthenticated: true if VNC password auth already passed
    AuthDecision Evaluate(const std::string& clientIP,
                          const std::string& protocol,
                          const std::string& suppliedToken = "",
                          bool isAuthenticated = false) {
        // 1. Localhost bypass
        if (config.localhostBypass && IsLocalhost(clientIP)) {
            return AuthDecision::AllowedLocalhost;
        }

        // 2. Token auth (pre-shared bearer token)
        if (!config.authToken.empty() && suppliedToken == config.authToken) {
            trustedClients.Trust(clientIP, 0);  // permanent once token-authed
            trustedClients.Save();
            return AuthDecision::AllowedToken;
        }

        // 3. Already VNC-authenticated + in trusted list = allow
        if (isAuthenticated && trustedClients.IsTrusted(clientIP)) {
            return AuthDecision::AllowedTrusted;
        }

        // 4. Trusted client (previously approved by user)
        if (trustedClients.IsTrusted(clientIP)) {
            return AuthDecision::AllowedTrusted;
        }

        // 5. Not trusted — check policy
        if (!config.requireRemoteAuth) {
            return AuthDecision::Allowed;  // auth disabled, allow all
        }

        if (config.trustOnFirstUse) {
            // Queue approval request and wait for tray decision
            std::string reqId = approvalStore.EnqueueRequest(
                clientIP, protocol, isAuthenticated);
            std::cout << "[Auth] Waiting for approval of " << clientIP
                      << " (id=" << reqId << ")" << std::endl;

            std::string result = approvalStore.WaitForResult(reqId);

            if (result == "approved") {
                trustedClients.Trust(clientIP, 60);  // trust for 1 hour default
                trustedClients.Save();
                return AuthDecision::AllowedApproved;
            } else if (result == "blocked") {
                return AuthDecision::RejectedBlocked;
            } else if (result == "timedout") {
                return AuthDecision::RejectedTimeout;
            }
            return AuthDecision::Rejected;
        }

        // RequireAuth with no TOFU = reject unknown clients
        return AuthDecision::Rejected;
    }

    bool IsAllowed(AuthDecision d) const {
        return d == AuthDecision::Allowed
            || d == AuthDecision::AllowedLocalhost
            || d == AuthDecision::AllowedToken
            || d == AuthDecision::AllowedTrusted
            || d == AuthDecision::AllowedApproved;
    }

    const char* DecisionName(AuthDecision d) const {
        switch (d) {
        case AuthDecision::Allowed:          return "allowed";
        case AuthDecision::AllowedLocalhost: return "allowed-localhost";
        case AuthDecision::AllowedToken:     return "allowed-token";
        case AuthDecision::AllowedTrusted:   return "allowed-trusted";
        case AuthDecision::AllowedApproved:  return "allowed-approved";
        case AuthDecision::Rejected:         return "rejected";
        case AuthDecision::RejectedBlocked:  return "rejected-blocked";
        case AuthDecision::RejectedTimeout:  return "rejected-timeout";
        default:                             return "unknown";
        }
    }

private:
    static bool IsLocalhost(const std::string& ip) {
        return ip == "127.0.0.1" || ip == "::1" || ip == "localhost";
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Global security context (singleton-like for easy access)
// ─────────────────────────────────────────────────────────────────────────────

class ConnectionSecurityContext {
public:
    EtwAuditLogger      auditLog;
    CertificatePinner   certPinner;
    IpAllowlist         ipAllowlist;
    MutualAuthConfig    mutualAuth;
    ConnectionAuthGate  authGate;   // TOFU + localhost bypass + trusted client store

    bool Initialize() {
        return auditLog.Initialize();
    }

    // Validate an incoming connection. Returns true if allowed, false if rejected.
    // Logs the decision via ETW.
    bool ValidateConnection(const std::string& clientIP, const std::string& protocol) {
        if (!ipAllowlist.IsAllowed(clientIP)) {
            auditLog.LogIpBlocked(clientIP);
            std::cerr << "[Security] Blocked: " << clientIP
                      << " not in allowlist" << std::endl;
            return false;
        }
        auditLog.LogConnect(clientIP, protocol);
        return true;
    }

    // Validate a client TLS certificate (cert pinning check)
    bool ValidateClientCert(PCCERT_CONTEXT cert, const std::string& clientIP) {
        if (!cert) {
            if (mutualAuth.requireClientCert) {
                auditLog.LogAuthFail(clientIP, "TLS", "No client cert provided");
                return false;
            }
            return true;
        }

        std::string thumbprint;
        if (!certPinner.Validate(cert, thumbprint)) {
            auditLog.LogCertRejected(clientIP, thumbprint);
            std::cerr << "[Security] Cert rejected: " << thumbprint
                      << " from " << clientIP << std::endl;
            return false;
        }

        auditLog.LogAuthSuccess(clientIP, "TLS", "cert:" + thumbprint);
        return true;
    }

    static ConnectionSecurityContext& Global() {
        static ConnectionSecurityContext instance;
        return instance;
    }

private:
    ConnectionSecurityContext() = default;
};
