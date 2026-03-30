// connection_security.h - M7 security features:
//   1. ETW per-connection audit logging
//   2. Certificate pinning (reject connections not matching pinned hash)
//   3. Mutual authentication config (require client TLS certificates)
//   4. IP allowlist enforcement

#pragma once
#include <windows.h>
#include <wincrypt.h>
#include <evntprov.h>
#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <functional>
#include <mutex>
#include <atomic>

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
// Global security context (singleton-like for easy access)
// ─────────────────────────────────────────────────────────────────────────────

class ConnectionSecurityContext {
public:
    EtwAuditLogger  auditLog;
    CertificatePinner certPinner;
    IpAllowlist     ipAllowlist;
    MutualAuthConfig mutualAuth;

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
