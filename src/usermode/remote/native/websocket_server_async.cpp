// websocket_server_async.cpp - Async WebSocket server with non-blocking I/O and threading
// Uses select() for multiplexing with worker thread pool for driver injection

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincrypt.h>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>
#include <map>

#include "../../core/driver_interface.h"
#include "../../common/logging/unified_logger.h"
#include "../../common/performance/performance_monitor.h"
#include "../../common/adaptive_quality.h"
#include "../../common/connection_security.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "crypt32.lib")

// Configuration
#define WS_MAX_CLIENTS 10
#define WS_BUFFER_SIZE 65536
#define WS_SELECT_TIMEOUT_MS 100
#define WS_INJECTION_QUEUE_SIZE 100

// Client state
struct WSClient {
    SOCKET socket;
    sockaddr_in address;
    bool connected;
    bool handshaked;
    std::string recvBuffer;
    std::string sendBuffer;
    
    // Rate limiting
    ULONGLONG lastInputTime;
    int inputCountLastSecond;
    
    // Statistics
    ULONGLONG messagesReceived;
    ULONGLONG messagesSent;
};

// Message for async injection
struct InjectionMessage {
    std::string method;
    std::vector<int> intParams;
    std::vector<bool> boolParams;
    SOCKET responseSocket;
    int requestId;
};

class AsyncWebSocketServer {
public:
    AsyncWebSocketServer(int port = 8443) 
        : port_(port)
        , running_(false)
        , listenSocket_(INVALID_SOCKET)
        , driverInterface_(nullptr)
        , logger_(nullptr)
        , perfMonitor_(nullptr) {
        
        // Initialize logging (use new/delete for RAII safety, not malloc/free)
        logger_ = new LOGGER_CONTEXT();
        LoggerInitialize(logger_, LOG_LEVEL_INFO, LOG_CATEGORY_ALL);

        // Initialize performance monitor
        perfMonitor_ = new PERF_MONITOR_CONTEXT();
        PerfMonitorInitialize(perfMonitor_, PERF_CATEGORY_ALL, 1000, 5000);
        PerfMonitorStartHitchDetection(perfMonitor_, 1000);

        // Initialize driver interface
        driverInterface_ = new DriverInterface();
        driverInterface_->Initialize();
        
        // Clear clients
        for (int i = 0; i < WS_MAX_CLIENTS; i++) {
            clients_[i].socket = INVALID_SOCKET;
            clients_[i].connected = false;
            clients_[i].handshaked = false;
        }
        
        LOG_INFO(logger_, LOG_CATEGORY_NETWORK, "AsyncWebSocket", 
            "Server initialized on port %d", port);
    }
    
    ~AsyncWebSocketServer() {
        Stop();
        
        if (driverInterface_) {
            delete driverInterface_;
        }
        
        if (perfMonitor_) {
            PerfMonitorStopHitchDetection(perfMonitor_);
            PerfMonitorShutdown(perfMonitor_);
            delete perfMonitor_;
            perfMonitor_ = nullptr;
        }
        
        if (logger_) {
            LoggerShutdown(logger_);
            delete logger_;
            logger_ = nullptr;
        }
    }
    
    bool Start() {
        if (running_) return false;
        
        // Initialize Winsock
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            LOG_ERROR(logger_, LOG_CATEGORY_NETWORK, "AsyncWebSocket", 
                "WSAStartup failed: %d", WSAGetLastError());
            return false;
        }
        
        // Create listen socket
        listenSocket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSocket_ == INVALID_SOCKET) {
            LOG_ERROR(logger_, LOG_CATEGORY_NETWORK, "AsyncWebSocket", 
                "socket() failed: %d", WSAGetLastError());
            WSACleanup();
            return false;
        }
        
        // Enable non-blocking mode
        u_long nonBlocking = 1;
        if (ioctlsocket(listenSocket_, FIONBIO, &nonBlocking) != 0) {
            LOG_ERROR(logger_, LOG_CATEGORY_NETWORK, "AsyncWebSocket", 
                "ioctlsocket() failed: %d", WSAGetLastError());
            closesocket(listenSocket_);
            WSACleanup();
            return false;
        }
        
        // Enable address reuse
        int opt = 1;
        setsockopt(listenSocket_, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
        
        // Bind
        sockaddr_in service = {};
        service.sin_family = AF_INET;
        service.sin_addr.s_addr = INADDR_ANY;
        service.sin_port = htons(port_);
        
        if (bind(listenSocket_, (sockaddr*)&service, sizeof(service)) == SOCKET_ERROR) {
            LOG_ERROR(logger_, LOG_CATEGORY_NETWORK, "AsyncWebSocket", 
                "bind() failed: %d", WSAGetLastError());
            closesocket(listenSocket_);
            WSACleanup();
            return false;
        }
        
        // Listen
        if (listen(listenSocket_, SOMAXCONN) == SOCKET_ERROR) {
            LOG_ERROR(logger_, LOG_CATEGORY_NETWORK, "AsyncWebSocket", 
                "listen() failed: %d", WSAGetLastError());
            closesocket(listenSocket_);
            WSACleanup();
            return false;
        }
        
        running_ = true;
        
        // Start worker threads
        networkThread_ = std::thread(&AsyncWebSocketServer::NetworkLoop, this);
        injectionThread_ = std::thread(&AsyncWebSocketServer::InjectionWorker, this);
        
        LOG_INFO(logger_, LOG_CATEGORY_NETWORK, "AsyncWebSocket", 
            "Server started on port %d (non-blocking mode)", port_);
        
        return true;
    }
    
    void Stop() {
        if (!running_) return;
        
        running_ = false;
        
        // Signal injection queue
        injectionCV_.notify_all();
        
        // Wait for threads
        if (networkThread_.joinable()) {
            networkThread_.join();
        }
        if (injectionThread_.joinable()) {
            injectionThread_.join();
        }
        
        // Close all client sockets
        for (int i = 0; i < WS_MAX_CLIENTS; i++) {
            if (clients_[i].socket != INVALID_SOCKET) {
                closesocket(clients_[i].socket);
                clients_[i].socket = INVALID_SOCKET;
                clients_[i].connected = false;
            }
        }
        
        if (listenSocket_ != INVALID_SOCKET) {
            closesocket(listenSocket_);
            listenSocket_ = INVALID_SOCKET;
        }
        
        WSACleanup();
        
        LOG_INFO(logger_, LOG_CATEGORY_NETWORK, "AsyncWebSocket", "Server stopped");
    }

private:
    // Main network loop using select()
    void NetworkLoop() {
        LOG_INFO(logger_, LOG_CATEGORY_NETWORK, "AsyncWebSocket", "Network thread started");
        
        fd_set readSet, writeSet, exceptSet;
        timeval timeout;
        
        while (running_) {
            // Clear sets
            FD_ZERO(&readSet);
            FD_ZERO(&writeSet);
            FD_ZERO(&exceptSet);
            
            // Add listen socket
            FD_SET(listenSocket_, &readSet);
            int maxFd = (int)listenSocket_;
            
            // Add client sockets
            for (int i = 0; i < WS_MAX_CLIENTS; i++) {
                if (clients_[i].socket != INVALID_SOCKET) {
                    FD_SET(clients_[i].socket, &readSet);
                    
                    // Add to write set if we have data to send
                    if (!clients_[i].sendBuffer.empty()) {
                        FD_SET(clients_[i].socket, &writeSet);
                    }
                    
                    FD_SET(clients_[i].socket, &exceptSet);
                    
                    if ((int)clients_[i].socket > maxFd) {
                        maxFd = (int)clients_[i].socket;
                    }
                }
            }
            
            // Set timeout (100ms)
            timeout.tv_sec = 0;
            timeout.tv_usec = WS_SELECT_TIMEOUT_MS * 1000;
            
            // Wait for activity
            int ready = select(maxFd + 1, &readSet, &writeSet, &exceptSet, &timeout);
            
            if (ready == SOCKET_ERROR) {
                LOG_ERROR(logger_, LOG_CATEGORY_NETWORK, "AsyncWebSocket", 
                    "select() failed: %d", WSAGetLastError());
                Sleep(10);
                continue;
            }
            
            // Check for new connections
            if (FD_ISSET(listenSocket_, &readSet)) {
                AcceptNewConnection();
            }
            
            // Handle client I/O
            for (int i = 0; i < WS_MAX_CLIENTS; i++) {
                if (clients_[i].socket == INVALID_SOCKET) continue;
                
                // Check for errors
                if (FD_ISSET(clients_[i].socket, &exceptSet)) {
                    LOG_WARNING(logger_, LOG_CATEGORY_NETWORK, "AsyncWebSocket", 
                        "Socket exception on client %d", i);
                    DisconnectClient(i);
                    continue;
                }
                
                // Check for incoming data
                if (FD_ISSET(clients_[i].socket, &readSet)) {
                    if (!HandleClientRead(i)) {
                        DisconnectClient(i);
                        continue;
                    }
                }
                
                // Check for write readiness
                if (FD_ISSET(clients_[i].socket, &writeSet)) {
                    HandleClientWrite(i);
                }
            }
        }
        
        LOG_INFO(logger_, LOG_CATEGORY_NETWORK, "AsyncWebSocket", "Network thread stopped");
    }
    
    // Accept new connection (non-blocking)
    void AcceptNewConnection() {
        sockaddr_in clientAddr;
        int addrLen = sizeof(clientAddr);
        
        SOCKET clientSocket = accept(listenSocket_, (sockaddr*)&clientAddr, &addrLen);
        
        if (clientSocket == INVALID_SOCKET) {
            int error = WSAGetLastError();
            if (error != WSAEWOULDBLOCK) {
                LOG_ERROR(logger_, LOG_CATEGORY_NETWORK, "AsyncWebSocket", 
                    "accept() failed: %d", error);
            }
            return;
        }
        
        // Set non-blocking
        u_long nonBlocking = 1;
        ioctlsocket(clientSocket, FIONBIO, &nonBlocking);
        
        // Resolve client IP for security checks
        char clientIP[INET_ADDRSTRLEN] = "unknown";
        inet_ntop(AF_INET, &clientAddr.sin_addr, clientIP, INET_ADDRSTRLEN);

        // Full auth gate: localhost bypass → trusted client → TOFU approval → reject
        auto& sec = ConnectionSecurityContext::Global();

        // First check IP allowlist (hard block before any auth logic)
        if (!sec.ipAllowlist.IsAllowed(clientIP)) {
            sec.auditLog.LogIpBlocked(clientIP);
            LOG_WARNING(logger_, LOG_CATEGORY_SECURITY, "AsyncWebSocket",
                "Connection blocked by IP allowlist: %s", clientIP);
            closesocket(clientSocket);
            return;
        }

        // Run connection auth gate (localhost bypass, trusted client, TOFU)
        auto decision = sec.authGate.Evaluate(clientIP, "WebSocket");
        if (!sec.authGate.IsAllowed(decision)) {
            sec.auditLog.LogAuthFail(clientIP, "WebSocket",
                sec.authGate.DecisionName(decision));
            LOG_WARNING(logger_, LOG_CATEGORY_SECURITY, "AsyncWebSocket",
                "Connection rejected (%s): %s",
                sec.authGate.DecisionName(decision), clientIP);
            closesocket(clientSocket);
            return;
        }

        LOG_INFO(logger_, LOG_CATEGORY_SECURITY, "AsyncWebSocket",
            "Connection permitted (%s): %s",
            sec.authGate.DecisionName(decision), clientIP);
        sec.auditLog.LogConnect(clientIP, "WebSocket");

        // Find free slot
        int slot = -1;
        for (int i = 0; i < WS_MAX_CLIENTS; i++) {
            if (!clients_[i].connected) {
                slot = i;
                break;
            }
        }

        if (slot == -1) {
            LOG_WARNING(logger_, LOG_CATEGORY_NETWORK, "AsyncWebSocket",
                "Too many clients, rejecting connection from %s", clientIP);
            sec.auditLog.LogDisconnect(clientIP, "WebSocket", "max clients");
            closesocket(clientSocket);
            return;
        }

        // Initialize client
        clients_[slot].socket = clientSocket;
        clients_[slot].address = clientAddr;
        clients_[slot].connected = true;
        clients_[slot].handshaked = false;
        clients_[slot].recvBuffer.clear();
        clients_[slot].sendBuffer.clear();
        clients_[slot].lastInputTime = GetTickCount64();
        clients_[slot].inputCountLastSecond = 0;
        clients_[slot].messagesReceived = 0;
        clients_[slot].messagesSent = 0;

        LOG_INFO(logger_, LOG_CATEGORY_NETWORK, "AsyncWebSocket",
            "Client %d connected from %s", slot, clientIP);
    }
    
    // Handle client read (non-blocking)
    bool HandleClientRead(int clientIndex) {
        WSClient& client = clients_[clientIndex];
        
        char buffer[1024];
        int received = recv(client.socket, buffer, sizeof(buffer), 0);
        
        if (received == SOCKET_ERROR) {
            int error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK) {
                return true;  // No data available, try again later
            }
            LOG_WARNING(logger_, LOG_CATEGORY_NETWORK, "AsyncWebSocket", 
                "recv() error on client %d: %d", clientIndex, error);
            return false;
        }
        
        if (received == 0) {
            // Connection closed
            LOG_INFO(logger_, LOG_CATEGORY_NETWORK, "AsyncWebSocket", 
                "Client %d disconnected", clientIndex);
            return false;
        }
        
        // Append to buffer
        client.recvBuffer.append(buffer, received);
        
        // Process messages
        if (!client.handshaked) {
            // Look for complete HTTP request
            if (client.recvBuffer.find("\r\n\r\n") != std::string::npos) {
                if (!PerformHandshake(clientIndex)) {
                    return false;
                }
                client.handshaked = true;
                client.recvBuffer.clear();
            }
        } else {
            // Process WebSocket frames
            ProcessWebSocketFrames(clientIndex);
        }
        
        return true;
    }
    
    // Handle client write (non-blocking)
    void HandleClientWrite(int clientIndex) {
        WSClient& client = clients_[clientIndex];
        
        if (client.sendBuffer.empty()) return;
        
        int sent = send(client.socket, client.sendBuffer.data(), 
            (int)client.sendBuffer.length(), 0);
        
        if (sent == SOCKET_ERROR) {
            int error = WSAGetLastError();
            if (error != WSAEWOULDBLOCK) {
                LOG_WARNING(logger_, LOG_CATEGORY_NETWORK, "AsyncWebSocket", 
                    "send() error on client %d: %d", clientIndex, error);
            }
            return;
        }
        
        // Remove sent data from buffer
        if (sent > 0) {
            client.sendBuffer.erase(0, sent);
            clients_[clientIndex].messagesSent++;
        }
    }
    
    // Perform WebSocket handshake
    bool PerformHandshake(int clientIndex) {
        // Parse WebSocket key
        std::string& request = clients_[clientIndex].recvBuffer;
        
        size_t keyPos = request.find("Sec-WebSocket-Key: ");
        if (keyPos == std::string::npos) {
            LOG_ERROR(logger_, LOG_CATEGORY_NETWORK, "AsyncWebSocket", 
                "No WebSocket key in request");
            return false;
        }
        
        keyPos += 19;
        size_t keyEnd = request.find("\r\n", keyPos);
        std::string wsKey = request.substr(keyPos, keyEnd - keyPos);
        
        // Compute accept key
        std::string accept = ComputeWebSocketAccept(wsKey);
        
        // Send response
        std::string response = 
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " + accept + "\r\n"
            "\r\n";
        
        clients_[clientIndex].sendBuffer += response;
        
        LOG_INFO(logger_, LOG_CATEGORY_NETWORK, "AsyncWebSocket", 
            "Client %d handshaked", clientIndex);
        
        return true;
    }
    
    // Process WebSocket frames
    void ProcessWebSocketFrames(int clientIndex) {
        WSClient& client = clients_[clientIndex];
        
        while (client.recvBuffer.length() >= 2) {
            // Parse frame header
            BYTE* data = (BYTE*)client.recvBuffer.data();
            size_t len = client.recvBuffer.length();
            
            bool fin = (data[0] & 0x80) != 0;
            BYTE opcode = data[0] & 0x0F;
            bool masked = (data[1] & 0x80) != 0;
            UINT64 payloadLen = data[1] & 0x7F;
            
            size_t headerSize = 2;
            
            // Extended length
            if (payloadLen == 126) {
                if (len < 4) return;
                payloadLen = (data[2] << 8) | data[3];
                headerSize = 4;
            } else if (payloadLen == 127) {
                if (len < 10) return;
                payloadLen = 0;
                for (int i = 0; i < 8; i++) {
                    payloadLen = (payloadLen << 8) | data[2 + i];
                }
                headerSize = 10;
            }
            
            // Masking key
            BYTE maskKey[4] = {0};
            if (masked) {
                if (len < headerSize + 4) return;
                memcpy(maskKey, data + headerSize, 4);
                headerSize += 4;
            }
            
            // Full frame available?
            if (len < headerSize + payloadLen) return;
            
            // Extract and unmask payload
            std::vector<BYTE> payload(payloadLen);
            for (UINT64 i = 0; i < payloadLen; i++) {
                payload[i] = data[headerSize + i];
                if (masked) {
                    payload[i] ^= maskKey[i % 4];
                }
            }
            
            // Remove processed frame from buffer
            client.recvBuffer.erase(0, headerSize + payloadLen);
            client.messagesReceived++;
            
            // Handle frame
            switch (opcode) {
            case 0x1: // Text
            case 0x2: // Binary
                HandleMessage(clientIndex, std::string(payload.begin(), payload.end()));
                break;
                
            case 0x8: // Close
                SendClose(clientIndex);
                DisconnectClient(clientIndex);
                return;
                
            case 0x9: // Ping
                SendPong(clientIndex, payload);
                break;
                
            case 0xA: // Pong
                break;  // Ignore
            }
        }
    }
    
    // Queue message for async injection
    void HandleMessage(int clientIndex, const std::string& message) {
        // Tier-aware rate limiting: limit drops as quality degrades under load
        const QualitySettings& qs = adaptiveQuality_.GetSettings();
        int rateLimit = qs.targetFps * 2;  // Allow 2 inputs per frame at current tier

        ULONGLONG now = GetTickCount64();
        if (now - clients_[clientIndex].lastInputTime > 1000) {
            clients_[clientIndex].inputCountLastSecond = 0;
        }

        if (clients_[clientIndex].inputCountLastSecond > rateLimit) {
            char clientIP[INET_ADDRSTRLEN] = "unknown";
            inet_ntop(AF_INET, &clients_[clientIndex].address.sin_addr,
                clientIP, INET_ADDRSTRLEN);
            LOG_WARNING(logger_, LOG_CATEGORY_NETWORK, "AsyncWebSocket",
                "Rate limit exceeded for client %d (limit=%d, tier=%s)",
                clientIndex, rateLimit,
                AdaptiveQuality::TierName(adaptiveQuality_.GetTier()));
            ConnectionSecurityContext::Global().auditLog.LogRateLimited(
                clientIP, rateLimit);
            adaptiveQuality_.ReportDroppedFrame();
            return;
        }
        
        clients_[clientIndex].inputCountLastSecond++;
        clients_[clientIndex].lastInputTime = now;
        
        // Queue for injection worker
        InjectionMessage msg;
        msg.responseSocket = clients_[clientIndex].socket;
        ParseInjectionRequest(message, msg);
        
        {
            std::unique_lock<std::mutex> lock(injectionMutex_);
            if (injectionQueue_.size() < WS_INJECTION_QUEUE_SIZE) {
                injectionQueue_.push(msg);
                injectionCV_.notify_one();
            } else {
                LOG_WARNING(logger_, LOG_CATEGORY_NETWORK, "AsyncWebSocket", 
                    "Injection queue full, dropping message");
            }
        }
    }
    
    // Worker thread for driver injection (prevents network blocking)
    void InjectionWorker() {
        LOG_INFO(logger_, LOG_CATEGORY_NETWORK, "AsyncWebSocket", "Injection worker started");
        
        while (running_) {
            InjectionMessage msg;
            
            // Wait for work
            {
                std::unique_lock<std::mutex> lock(injectionMutex_);
                injectionCV_.wait_for(lock, std::chrono::milliseconds(100), 
                    [this] { return !injectionQueue_.empty() || !running_; });
                
                if (!running_) break;
                
                if (injectionQueue_.empty()) continue;
                
                msg = injectionQueue_.front();
                injectionQueue_.pop();
            }
            
            // Perform injection (may take time, but doesn't block network)
            bool success = false;
            std::string result = "{}";
            
            ULONGLONG perfStart;
            PerfMonitorStart(perfMonitor_, PERF_CATEGORY_INPUT_INJECT, 
                msg.method.c_str(), __FUNCTION__, __LINE__, &perfStart);
            
            if (msg.method == "input.keyboard.keydown") {
                if (driverInterface_ && msg.intParams.size() >= 1) {
                    success = driverInterface_->InjectKeyDown(
                        (UCHAR)msg.intParams[0], 
                        msg.intParams.size() > 1 ? (UCHAR)msg.intParams[1] : 0);
                }
            }
            else if (msg.method == "input.mouse.move") {
                if (driverInterface_ && msg.intParams.size() >= 2) {
                    success = driverInterface_->InjectMouseMove(
                        msg.intParams[0], msg.intParams[1],
                        msg.boolParams.size() > 0 ? msg.boolParams[0] : false);
                }
            }
            else if (msg.method == "input.keyboard.keyup") {
                if (driverInterface_ && msg.intParams.size() >= 1) {
                    success = driverInterface_->InjectKeyUp(
                        (UCHAR)msg.intParams[0],
                        msg.intParams.size() > 1 ? (UCHAR)msg.intParams[1] : 0);
                }
            }
            else if (msg.method == "input.mouse.button") {
                if (driverInterface_ && msg.intParams.size() >= 1) {
                    bool pressed = msg.boolParams.size() > 0 ? msg.boolParams[0] : true;
                    success = driverInterface_->InjectMouseButton(
                        (UCHAR)msg.intParams[0], pressed);
                }
            }
            else if (msg.method == "input.mouse.scroll") {
                if (driverInterface_ && msg.intParams.size() >= 2) {
                    success = driverInterface_->InjectMouseScroll(
                        msg.intParams[0], msg.intParams[1]);
                }
            }
            else if (msg.method == "input.controller.report") {
                if (driverInterface_ && msg.intParams.size() >= 7) {
                    XUSB_REPORT report = {};
                    report.wButtons       = (USHORT)msg.intParams[0];
                    report.bLeftTrigger   = (BYTE)msg.intParams[1];
                    report.bRightTrigger  = (BYTE)msg.intParams[2];
                    report.sThumbLX       = (SHORT)msg.intParams[3];
                    report.sThumbLY       = (SHORT)msg.intParams[4];
                    report.sThumbRX       = (SHORT)msg.intParams[5];
                    report.sThumbRY       = (SHORT)msg.intParams[6];
                    success = driverInterface_->InjectControllerReport(report);
                }
            }
            else if (msg.method == "system.ping") {
                success = true;
                result = "\"pong\"";
            }
            else if (msg.method == "system.get_version") {
                success = true;
                result = "{\"version\":\"1.0.0\",\"protocol\":\"2.0\"}";
            }
            
            PerfMonitorEnd(perfMonitor_, PERF_CATEGORY_INPUT_INJECT,
                msg.method.c_str(), __FUNCTION__, __LINE__, perfStart, nullptr);
            
            // Queue response (will be sent by network thread)
            std::string response = success 
                ? "{\"jsonrpc\":\"2.0\",\"result\":" + result + ",\"id\":" + std::to_string(msg.requestId) + "}"
                : "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32603,\"message\":\"Injection failed\"},\"id\":" + std::to_string(msg.requestId) + "}";
            
            // Find client and queue response
            for (int i = 0; i < WS_MAX_CLIENTS; i++) {
                if (clients_[i].socket == msg.responseSocket && clients_[i].connected) {
                    std::lock_guard<std::mutex> lock(clientsMutex_);
                    clients_[i].sendBuffer += EncodeWebSocketFrame(response);
                    break;
                }
            }
        }
        
        LOG_INFO(logger_, LOG_CATEGORY_NETWORK, "AsyncWebSocket", "Injection worker stopped");
    }
    
    // Helper functions
    std::string ComputeWebSocketAccept(const std::string& key);
    void ParseInjectionRequest(const std::string& json, InjectionMessage& msg);
    std::string EncodeWebSocketFrame(const std::string& message);
    void SendClose(int clientIndex);
    void SendPong(int clientIndex, const std::vector<BYTE>& payload);
    void DisconnectClient(int clientIndex);

    // Adaptive quality controller - shared across all clients
    AdaptiveQuality adaptiveQuality_;
};

// ============================================================
// Helper function definitions (out-of-line)
// ============================================================

std::string AsyncWebSocketServer::ComputeWebSocketAccept(const std::string& key) {
    const std::string magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string combined = key + magic;

    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    BYTE hash[20] = {};
    DWORD hashLen = sizeof(hash);

    if (CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        if (CryptCreateHash(hProv, CALG_SHA1, 0, 0, &hHash)) {
            CryptHashData(hHash, (BYTE*)combined.data(), (DWORD)combined.size(), 0);
            CryptGetHashParam(hHash, HP_HASHVAL, hash, &hashLen, 0);
            CryptDestroyHash(hHash);
        }
        CryptReleaseContext(hProv, 0);
    }

    DWORD b64Len = 0;
    CryptBinaryToStringA(hash, hashLen,
        CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &b64Len);
    std::string b64(b64Len, '\0');
    CryptBinaryToStringA(hash, hashLen,
        CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, &b64[0], &b64Len);
    b64.resize(b64Len);
    return b64;
}

void AsyncWebSocketServer::ParseInjectionRequest(
        const std::string& json, InjectionMessage& msg) {
    // Minimal JSON field extractor (no external dependency)
    auto extractStr = [&](const char* key) -> std::string {
        std::string k = std::string("\"") + key + "\"";
        size_t p = json.find(k);
        if (p == std::string::npos) return "";
        p = json.find(':', p + k.size());
        if (p == std::string::npos) return "";
        p = json.find('"', p);
        if (p == std::string::npos) return "";
        size_t e = json.find('"', p + 1);
        if (e == std::string::npos) return "";
        return json.substr(p + 1, e - p - 1);
    };
    auto extractInt = [&](const char* key, int def = 0) -> int {
        std::string k = std::string("\"") + key + "\"";
        size_t p = json.find(k);
        if (p == std::string::npos) return def;
        p = json.find(':', p + k.size());
        if (p == std::string::npos) return def;
        p++;
        while (p < json.size() && json[p] == ' ') p++;
        if (p >= json.size()) return def;
        try { return std::stoi(json.substr(p)); } catch (...) { return def; }
    };
    auto extractBool = [&](const char* key, bool def = false) -> bool {
        std::string k = std::string("\"") + key + "\"";
        size_t p = json.find(k);
        if (p == std::string::npos) return def;
        p = json.find(':', p + k.size());
        if (p == std::string::npos) return def;
        p++;
        while (p < json.size() && json[p] == ' ') p++;
        if (p < json.size()) {
            if (json[p] == 't') return true;
            if (json[p] == 'f') return false;
        }
        return def;
    };

    msg.method    = extractStr("method");
    msg.requestId = extractInt("id");

    if (msg.method == "input.keyboard.keydown" ||
        msg.method == "input.keyboard.keyup") {
        msg.intParams.push_back(extractInt("keyCode"));
        msg.intParams.push_back(extractInt("modifiers"));
    } else if (msg.method == "input.mouse.move") {
        msg.intParams.push_back(extractInt("x"));
        msg.intParams.push_back(extractInt("y"));
        msg.boolParams.push_back(extractBool("absolute"));
    } else if (msg.method == "input.mouse.button") {
        msg.intParams.push_back(extractInt("button"));
        msg.boolParams.push_back(extractBool("pressed", true));
    } else if (msg.method == "input.mouse.scroll") {
        msg.intParams.push_back(extractInt("vertical"));
        msg.intParams.push_back(extractInt("horizontal"));
    } else if (msg.method == "input.controller.report") {
        msg.intParams.push_back(extractInt("buttons"));
        msg.intParams.push_back(extractInt("leftTrigger"));
        msg.intParams.push_back(extractInt("rightTrigger"));
        msg.intParams.push_back(extractInt("thumbLX"));
        msg.intParams.push_back(extractInt("thumbLY"));
        msg.intParams.push_back(extractInt("thumbRX"));
        msg.intParams.push_back(extractInt("thumbRY"));
    }
}

std::string AsyncWebSocketServer::EncodeWebSocketFrame(const std::string& message) {
    std::string frame;
    frame.push_back((char)0x81);  // FIN + text opcode
    size_t len = message.size();
    if (len < 126) {
        frame.push_back((char)len);
    } else if (len < 65536) {
        frame.push_back((char)126);
        frame.push_back((char)((len >> 8) & 0xFF));
        frame.push_back((char)(len & 0xFF));
    } else {
        frame.push_back((char)127);
        for (int i = 7; i >= 0; i--)
            frame.push_back((char)((len >> (i * 8)) & 0xFF));
    }
    frame += message;
    return frame;
}

void AsyncWebSocketServer::SendClose(int clientIndex) {
    char frame[2] = { (char)0x88, 0x00 };  // FIN + close opcode
    send(clients_[clientIndex].socket, frame, 2, 0);
}

void AsyncWebSocketServer::SendPong(int clientIndex, const std::vector<BYTE>& payload) {
    std::string frame;
    frame.push_back((char)0x8A);  // FIN + pong opcode
    size_t len = payload.size();
    frame.push_back((char)(len & 0x7F));
    frame.append(payload.begin(), payload.end());
    send(clients_[clientIndex].socket, frame.data(), (int)frame.size(), 0);
}

void AsyncWebSocketServer::DisconnectClient(int clientIndex) {
    if (clients_[clientIndex].socket == INVALID_SOCKET) return;

    char clientIP[INET_ADDRSTRLEN] = "unknown";
    inet_ntop(AF_INET, &clients_[clientIndex].address.sin_addr,
        clientIP, INET_ADDRSTRLEN);
    ConnectionSecurityContext::Global().auditLog.LogDisconnect(
        clientIP, "WebSocket", "client disconnected");
    LOG_INFO(logger_, LOG_CATEGORY_NETWORK, "AsyncWebSocket",
        "Disconnecting client %d (%s)", clientIndex, clientIP);

    closesocket(clients_[clientIndex].socket);
    clients_[clientIndex].socket     = INVALID_SOCKET;
    clients_[clientIndex].connected  = false;
    clients_[clientIndex].handshaked = false;
    clients_[clientIndex].recvBuffer.clear();
    clients_[clientIndex].sendBuffer.clear();
}

// ============================================================
// Factory API (see websocket_server_async.h)
// ============================================================
#include "websocket_server_async.h"

void* WsAsync_Create(int port)   { return new AsyncWebSocketServer(port); }
bool  WsAsync_Start(void* srv)   { return static_cast<AsyncWebSocketServer*>(srv)->Start(); }
void  WsAsync_Stop(void* srv)    { static_cast<AsyncWebSocketServer*>(srv)->Stop(); }
void  WsAsync_Destroy(void* srv) { delete static_cast<AsyncWebSocketServer*>(srv); }
