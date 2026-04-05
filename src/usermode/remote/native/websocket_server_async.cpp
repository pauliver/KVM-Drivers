// websocket_server_async.cpp - Async WebSocket server with non-blocking I/O and threading
// Uses select() for multiplexing with worker thread pool for driver injection

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincrypt.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <gdiplus.h>
#include <objidl.h>
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
#include <algorithm>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "gdiplus.lib")

#include "../../core/driver_interface.h"
#include "../../common/logging/unified_logger.h"
#include "../../common/performance/performance_monitor.h"
#include "../../common/adaptive_quality.h"
#include "../../common/connection_security.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "crypt32.lib")

// Configuration
// WS_MAX_CLIENTS is the compile-time array ceiling.
// The runtime limit is AsyncWebSocketServer::maxClients_ which is set from
// AppSettings.WsMaxClients and clamped to [1, WS_MAX_CLIENTS].
#define WS_MAX_CLIENTS 32
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

    // Controller slot (-1 = no controller claimed)
    int controllerSlot;
};

// Reconnect hold: keeps a slot reserved for a disconnecting client's IP
// for CONTROLLER_HOLD_MS milliseconds so an unstable connection gets the
// same controller on reconnect.
struct ControllerHold {
    char     ip[INET_ADDRSTRLEN];
    int      slot;
    ULONGLONG releasedAt;   // GetTickCount64() when the hold started
    bool     active;
};

constexpr ULONGLONG CONTROLLER_HOLD_MS = 30000;  // 30-second reconnect grace

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
    AsyncWebSocketServer(int port = 8443, int maxClients = 10) 
        : port_(port)
        , maxClients_(maxClients < 1 ? 1 : maxClients > WS_MAX_CLIENTS ? WS_MAX_CLIENTS : maxClients)
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

        // Initialize GDI+ for JPEG encoding
        Gdiplus::GdiplusStartupInput gdipInput;
        Gdiplus::GdiplusStartup(&gdiplusToken_, &gdipInput, nullptr);

        // Initialize driver interface
        driverInterface_ = new DriverInterface();
        driverInterface_->Initialize();
        
        // Clear clients (use WS_MAX_CLIENTS for array sizing; maxClients_ for runtime cap)
        for (int i = 0; i < WS_MAX_CLIENTS; i++) {
            clients_[i].socket = INVALID_SOCKET;
            clients_[i].connected = false;
            clients_[i].handshaked = false;
            streamingClients_[i] = false;
            clients_[i].controllerSlot = -1;
        }
        for (int i = 0; i < 4; i++) { controllerSlotOwner_[i] = -1; controllerHolds_[i] = {}; }
        
        LOG_INFO(logger_, LOG_CATEGORY_NETWORK, "AsyncWebSocket",
            "Server initialized on port %d (max %d clients)", port, maxClients_);
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

        // Shutdown GDI+
        if (gdiplusToken_) {
            Gdiplus::GdiplusShutdown(gdiplusToken_);
            gdiplusToken_ = 0;
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
        for (int i = 0; i < maxClients_; i++) {
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
            for (int i = 0; i < maxClients_; i++) {
                if (clients_[i].socket != INVALID_SOCKET) {
                    FD_SET(clients_[i].socket, &readSet);
                    
                    // Always include handshaked clients in the write set so that
                    // HandleClientWrite (which now checks the buffer under its own
                    // lock) gets called every iteration.  Avoids a lockless peek at
                    // sendBuffer.empty() that would race with producer threads.
                    if (clients_[i].handshaked) {
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
            for (int i = 0; i < maxClients_; i++) {
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

        // Find free slot (within runtime maxClients_ cap, not the array ceiling)
        int slot = -1;
        for (int i = 0; i < maxClients_; i++) {
            if (!clients_[i].connected) {
                slot = i;
                break;
            }
        }

        if (slot == -1) {
            LOG_WARNING(logger_, LOG_CATEGORY_NETWORK, "AsyncWebSocket",
                "Too many clients (%d/%d), rejecting connection from %s",
                maxClients_, maxClients_, clientIP);
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
        clients_[slot].controllerSlot = -1;

        // Reconnect: if this IP has an active hold on a controller slot, restore it
        ULONGLONG now = GetTickCount64();
        for (int cs = 0; cs < 4; cs++) {
            ControllerHold& hold = controllerHolds_[cs];
            if (hold.active && strcmp(hold.ip, clientIP) == 0) {
                if (now - hold.releasedAt < CONTROLLER_HOLD_MS) {
                    // Same IP reconnected within grace period: restore slot
                    clients_[slot].controllerSlot  = cs;
                    controllerSlotOwner_[cs]        = slot;
                    hold.active                     = false;
                    LOG_INFO(logger_, LOG_CATEGORY_NETWORK, "AsyncWebSocket",
                        "Restored controller slot %d to reconnecting client %s",
                        cs, clientIP);
                } else {
                    // Grace period expired: fully release the slot
                    if (driverInterface_) driverInterface_->ReleaseControllerSlot(cs);
                    hold.active = false;
                }
                break;
            }
        }

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
    // Uses swap-under-lock so I/O happens outside the mutex.
    void HandleClientWrite(int clientIndex) {
        // Grab current send buffer under lock, leave it empty for producers
        std::string toSend;
        {
            std::lock_guard<std::mutex> lock(clientsMutex_);
            if (clients_[clientIndex].sendBuffer.empty()) return;
            toSend = std::move(clients_[clientIndex].sendBuffer);
            // sendBuffer is now empty; producers can safely append new data
        }

        int sent = send(clients_[clientIndex].socket,
            toSend.data(), (int)toSend.length(), 0);

        if (sent == SOCKET_ERROR) {
            int error = WSAGetLastError();
            if (error != WSAEWOULDBLOCK) {
                LOG_WARNING(logger_, LOG_CATEGORY_NETWORK, "AsyncWebSocket",
                    "send() error on client %d: %d", clientIndex, error);
            }
            // Put data back so it isn't lost
            std::lock_guard<std::mutex> lock(clientsMutex_);
            clients_[clientIndex].sendBuffer =
                toSend + clients_[clientIndex].sendBuffer;
            return;
        }

        if (sent > 0) {
            clients_[clientIndex].messagesSent++;
            if ((size_t)sent < toSend.size()) {
                // Partial send: prepend remainder ahead of any newly queued data
                std::lock_guard<std::mutex> lock(clientsMutex_);
                clients_[clientIndex].sendBuffer =
                    toSend.substr(sent) + clients_[clientIndex].sendBuffer;
            }
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
            
            // Reject absurdly large frames before allocating
            if (payloadLen > 16 * 1024 * 1024) {
                LOG_WARNING(logger_, LOG_CATEGORY_NETWORK, "AsyncWebSocket",
                    "Oversized frame (%llu bytes) from client %d — closing",
                    (unsigned long long)payloadLen, clientIndex);
                DisconnectClient(clientIndex);
                return;
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
    
    // Handle display.start/stop and info methods synchronously (no driver I/O)
    void HandleControlMessage(int clientIndex, InjectionMessage& msg) {
        if (msg.method == "display.start_stream") {
            StartClientStream(clientIndex, msg.requestId);
        } else if (msg.method == "display.stop_stream") {
            StopClientStream(clientIndex, msg.requestId);
        } else if (msg.method == "display.set_quality") {
            if (!msg.intParams.empty()) {
                streamQuality_ = msg.intParams[0];
            }
            std::string ack = "{\"jsonrpc\":\"2.0\",\"result\":{\"quality\":" +
                std::to_string(streamQuality_) + "},\"id\":" +
                std::to_string(msg.requestId) + "}";
            std::lock_guard<std::mutex> lock(clientsMutex_);
            if (clients_[clientIndex].connected)
                clients_[clientIndex].sendBuffer += EncodeWebSocketFrame(ack);
        } else if (msg.method == "system.get_capabilities") {
            std::string resp = "{\"jsonrpc\":\"2.0\",\"result\":{\"capabilities\":"
                "{\"screen\":true,\"keyboard\":true,\"mouse\":true,\"controller\":true},"
                "\"version\":\"1.0.0\",\"protocol\":\"2.0\"},\"id\":" +
                std::to_string(msg.requestId) + "}";
            std::lock_guard<std::mutex> lock(clientsMutex_);
            if (clients_[clientIndex].connected)
                clients_[clientIndex].sendBuffer += EncodeWebSocketFrame(resp);
        } else if (msg.method == "auth.authenticate") {
            // Connection-level auth already enforced by TOFU gate; accept any token here
            std::string resp = "{\"jsonrpc\":\"2.0\",\"result\":{\"status\":\"authenticated\"},\"id\":" +
                std::to_string(msg.requestId) + "}";
            std::lock_guard<std::mutex> lock(clientsMutex_);
            if (clients_[clientIndex].connected)
                clients_[clientIndex].sendBuffer += EncodeWebSocketFrame(resp);
        }
    }

    // Queue message for async injection
    void HandleMessage(int clientIndex, const std::string& message) {
        // Parse first so control messages bypass rate limiting entirely
        InjectionMessage msg;
        msg.responseSocket = clients_[clientIndex].socket;
        ParseInjectionRequest(message, msg);

        // Control messages: handle directly, never rate-limited
        if (msg.method == "display.start_stream" ||
            msg.method == "display.stop_stream"  ||
            msg.method == "display.set_quality"  ||
            msg.method == "system.get_capabilities" ||
            msg.method == "auth.authenticate") {
            HandleControlMessage(clientIndex, msg);
            return;
        }

        // Tier-aware rate limiting for input injection only
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
                    // Find the client index that owns this response socket
                    int clientIdx = -1;
                    for (int i = 0; i < maxClients_; i++) {
                        if (clients_[i].socket == msg.responseSocket && clients_[i].connected) {
                            clientIdx = i;
                            break;
                        }
                    }

                    if (clientIdx >= 0) {
                        // Auto-claim a slot on first controller report from this client
                        if (clients_[clientIdx].controllerSlot < 0) {
                            // Check for expired holds and expire them first
                            ULONGLONG now = GetTickCount64();
                            for (int cs = 0; cs < 4; cs++) {
                                ControllerHold& h = controllerHolds_[cs];
                                if (h.active && now - h.releasedAt >= CONTROLLER_HOLD_MS) {
                                    driverInterface_->ReleaseControllerSlot(cs);
                                    h.active = false;
                                }
                            }

                            int newSlot = driverInterface_->ClaimControllerSlot();
                            if (newSlot >= 0) {
                                clients_[clientIdx].controllerSlot = newSlot;
                                controllerSlotOwner_[newSlot] = clientIdx;
                                // Tell the client which player number they are
                                std::string notif =
                                    "{\"jsonrpc\":\"2.0\",\"method\":\"input.controller.assigned\","
                                    "\"params\":{\"slot\":" + std::to_string(newSlot) + "}}";
                                std::lock_guard<std::mutex> lock(clientsMutex_);
                                clients_[clientIdx].sendBuffer += EncodeWebSocketFrame(notif);
                                LOG_INFO(logger_, LOG_CATEGORY_NETWORK, "AsyncWebSocket",
                                    "Client %d claimed controller slot %d (player %d)",
                                    clientIdx, newSlot, newSlot + 1);
                            } else {
                                // All 4 slots are busy — tell the client
                                std::string err =
                                    "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32001,"
                                    "\"message\":\"All controller slots are occupied (max 4 players)\"},"
                                    "\"id\":" + std::to_string(msg.requestId) + "}";
                                std::lock_guard<std::mutex> lock(clientsMutex_);
                                clients_[clientIdx].sendBuffer += EncodeWebSocketFrame(err);
                                success = false;
                                goto controller_done;
                            }
                        }

                        int slot = clients_[clientIdx].controllerSlot;
                        XUSB_REPORT report = {};
                        report.wButtons      = (USHORT)msg.intParams[0];
                        report.bLeftTrigger  = (BYTE)msg.intParams[1];
                        report.bRightTrigger = (BYTE)msg.intParams[2];
                        report.sThumbLX      = (SHORT)msg.intParams[3];
                        report.sThumbLY      = (SHORT)msg.intParams[4];
                        report.sThumbRX      = (SHORT)msg.intParams[5];
                        report.sThumbRY      = (SHORT)msg.intParams[6];
                        success = driverInterface_->InjectControllerReportSlot(slot, report);
                    }
                    controller_done:;
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
            for (int i = 0; i < maxClients_; i++) {
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

    // Network configuration (set at construction, immutable after Start())
    int port_;
    int maxClients_;  // runtime cap ≤ WS_MAX_CLIENTS; set from AppSettings.WsMaxClients
    bool running_;
    SOCKET listenSocket_;
    DriverInterface*    driverInterface_;
    LOGGER_CONTEXT*     logger_;
    PERF_MONITOR_CONTEXT* perfMonitor_;
    std::thread networkThread_;
    std::thread injectionThread_;
    WSClient clients_[WS_MAX_CLIENTS];
    std::mutex clientsMutex_;
    std::queue<InjectionMessage> injectionQueue_;
    std::mutex injectionMutex_;
    std::condition_variable injectionCV_;

    // Adaptive quality controller - shared across all clients
    AdaptiveQuality adaptiveQuality_;

    // Controller slot management
    // controllerSlotOwner_[s] = WS client index that currently holds slot s, or -1
    int controllerSlotOwner_[4] = {-1, -1, -1, -1};
    ControllerHold controllerHolds_[4] = {};  // per-slot reconnect hold

    // Screen streaming
    ID3D11Device*            streamD3D_     = nullptr;
    ID3D11DeviceContext*     streamCtx_     = nullptr;
    IDXGIOutputDuplication*  streamDupl_    = nullptr;
    ID3D11Texture2D*         streamStaging_ = nullptr;
    int                      streamW_       = 0;
    int                      streamH_       = 0;
    std::thread              streamThread_;
    std::atomic<bool>        streamRunning_{false};
    std::atomic<int>         streamClientCount_{0};
    bool                     streamingClients_[WS_MAX_CLIENTS]{};
    int                      streamQuality_ = 70;  // JPEG quality 0-100
    ULONG_PTR                gdiplusToken_  = 0;

    bool InitCapture();
    void StartCapture();
    void StopCapture();
    void StreamLoop();
    std::vector<BYTE> EncodeFrameJPEG(const BYTE* bgra, int w, int h, UINT rowPitch);
    std::string EncodeBinaryFrame(const void* data, size_t len);
    void StartClientStream(int clientIndex, int requestId);
    void StopClientStream(int clientIndex, int requestId);
    void PushResolutionChange(int w, int h);
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
    } else if (msg.method == "display.set_quality") {
        // Map quality string to JPEG quality int
        std::string qs = extractStr("quality");
        int jpegQ = 70;
        if      (qs == "high")     jpegQ = 90;
        else if (qs == "medium")   jpegQ = 70;
        else if (qs == "low")      jpegQ = 40;
        else if (qs == "adaptive") jpegQ = 60;
        msg.intParams.push_back(jpegQ);
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

    // Release controller slot (with hold for reconnect stability)
    int cslot = clients_[clientIndex].controllerSlot;
    if (cslot >= 0) {
        char clientIP[INET_ADDRSTRLEN] = "unknown";
        inet_ntop(AF_INET, &clients_[clientIndex].address.sin_addr, clientIP, INET_ADDRSTRLEN);

        // Zero out all buttons so the game sees a clean release
        XUSB_REPORT zero = {};
        zero.bReportId = (BYTE)cslot;
        zero.bSize     = sizeof(XUSB_REPORT);
        if (driverInterface_) driverInterface_->InjectControllerReportSlot(cslot, zero);

        // Hold the slot for CONTROLLER_HOLD_MS so a reconnecting client
        // gets the same player number.
        controllerSlotOwner_[cslot] = -1;
        ControllerHold& hold = controllerHolds_[cslot];
        strncpy_s(hold.ip, clientIP, INET_ADDRSTRLEN - 1);
        hold.slot       = cslot;
        hold.releasedAt = GetTickCount64();
        hold.active     = true;

        clients_[clientIndex].controllerSlot = -1;
        // Do NOT call driverInterface_->ReleaseControllerSlot() yet—
        // the VHF device stays active during the hold window.
        LOG_INFO(logger_, LOG_CATEGORY_NETWORK, "AsyncWebSocket",
            "Controller slot %d held for %s (%.0f s grace)",
            cslot, clientIP, CONTROLLER_HOLD_MS / 1000.0);
    }

    // Clear streaming flag before closing
    if (streamingClients_[clientIndex]) {
        streamingClients_[clientIndex] = false;
        int prev = streamClientCount_.fetch_sub(1);
        if (prev <= 1) {
            StopCapture();
        }
    }

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
// Screen streaming implementation
// ============================================================

bool AsyncWebSocketServer::InitCapture() {
    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL obtained;
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        featureLevels, ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION,
        &streamD3D_, &obtained, &streamCtx_);
    if (FAILED(hr)) {
        LOG_ERROR(logger_, LOG_CATEGORY_NETWORK, "WsStream",
            "D3D11CreateDevice failed: 0x%x", (unsigned)hr);
        return false;
    }

    IDXGIDevice*  dxgiDev   = nullptr;
    IDXGIAdapter* dxgiAdapt = nullptr;
    IDXGIOutput*  dxgiOut   = nullptr;
    IDXGIOutput1* dxgiOut1  = nullptr;

    streamD3D_->QueryInterface(__uuidof(IDXGIDevice),  (void**)&dxgiDev);
    dxgiDev->GetAdapter(&dxgiAdapt);
    dxgiAdapt->EnumOutputs(0, &dxgiOut);
    hr = dxgiOut->QueryInterface(__uuidof(IDXGIOutput1), (void**)&dxgiOut1);

    if (SUCCEEDED(hr)) {
        hr = dxgiOut1->DuplicateOutput(streamD3D_, &streamDupl_);
        dxgiOut1->Release();
    }
    if (dxgiOut)   dxgiOut->Release();
    if (dxgiAdapt) dxgiAdapt->Release();
    if (dxgiDev)   dxgiDev->Release();

    if (FAILED(hr)) {
        LOG_ERROR(logger_, LOG_CATEGORY_NETWORK, "WsStream",
            "DuplicateOutput failed: 0x%x", (unsigned)hr);
        streamD3D_->Release(); streamD3D_ = nullptr;
        streamCtx_->Release(); streamCtx_ = nullptr;
        return false;
    }

    DXGI_OUTDUPL_DESC dd;
    streamDupl_->GetDesc(&dd);
    streamW_ = (int)dd.ModeDesc.Width;
    streamH_ = (int)dd.ModeDesc.Height;

    LOG_INFO(logger_, LOG_CATEGORY_NETWORK, "WsStream",
        "DXGI capture initialized: %dx%d", streamW_, streamH_);
    return true;
}

// ── Encoder pipeline architecture note ────────────────────────────────────────
// The hardware encoders (NVENC / AMD AMF / Intel QSV) live in
// video_pipeline.cpp / encoder_manager.cpp and produce H.264 packets.
// Currently THIS server uses its own direct DXGI → GDI+ JPEG path instead.
//
// The two pipelines are NOT connected. To hook them up:
//   1. Replace EncodeFrameJPEG() with EncoderManager::EncodeFrame() calls.
//   2. Wrap H.264 NAL units in fragmented MP4 (fMP4) so that the browser's
//      Media Source Extensions (MSE) API can consume them:
//        - Initialization segment: send once (SPS/PPS wrapped in MOOV box).
//        - Media segments: send each MOOF+MDAT pair per keyframe/GOP.
//   3. Update the web client to use a <video> element with MediaSource instead
//      of the current createImageBitmap() / Canvas approach.
//
// Until that work is done, JPEG binary frames are the delivery path.
// JPEG is ~100-200 KB/frame @ 1080p vs. ~10 KB/frame with H.264 CBR 4 Mbps.
// ────────────────────────────────────────────────────────────────────────────────

void AsyncWebSocketServer::StartCapture() {
    if (streamRunning_) return;
    if (!InitCapture()) {
        LOG_ERROR(logger_, LOG_CATEGORY_NETWORK, "WsStream",
            "Capture unavailable — stream will be black");
        return;
    }
    streamRunning_ = true;
    streamThread_ = std::thread(&AsyncWebSocketServer::StreamLoop, this);
}

void AsyncWebSocketServer::StopCapture() {
    streamRunning_ = false;
    if (streamThread_.joinable()) streamThread_.join();
    if (streamStaging_) { streamStaging_->Release(); streamStaging_ = nullptr; }
    if (streamDupl_)    { streamDupl_->Release();    streamDupl_    = nullptr; }
    if (streamCtx_)     { streamCtx_->Release();     streamCtx_     = nullptr; }
    if (streamD3D_)     { streamD3D_->Release();     streamD3D_     = nullptr; }
}

void AsyncWebSocketServer::StreamLoop() {
    LOG_INFO(logger_, LOG_CATEGORY_NETWORK, "WsStream", "Stream thread started");

    while (streamRunning_ && running_) {
        if (streamClientCount_ <= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        if (!streamDupl_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            InitCapture();
            continue;
        }

        IDXGIResource*           res = nullptr;
        DXGI_OUTDUPL_FRAME_INFO  fi  = {};
        HRESULT hr = streamDupl_->AcquireNextFrame(100, &fi, &res);

        if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;

        if (hr == DXGI_ERROR_ACCESS_LOST) {
            streamDupl_->Release(); streamDupl_ = nullptr;
            if (streamStaging_) { streamStaging_->Release(); streamStaging_ = nullptr; }
            continue;
        }

        if (FAILED(hr)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        ID3D11Texture2D* gpuTex = nullptr;
        res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&gpuTex);

        if (gpuTex) {
            D3D11_TEXTURE2D_DESC desc;
            gpuTex->GetDesc(&desc);

            bool needNew = !streamStaging_;
            if (streamStaging_) {
                D3D11_TEXTURE2D_DESC sd;
                streamStaging_->GetDesc(&sd);
                needNew = (sd.Width != desc.Width || sd.Height != desc.Height);
            }
            if (needNew) {
                if (streamStaging_) { streamStaging_->Release(); streamStaging_ = nullptr; }
                D3D11_TEXTURE2D_DESC sd = desc;
                sd.Usage           = D3D11_USAGE_STAGING;
                sd.BindFlags       = 0;
                sd.CPUAccessFlags  = D3D11_CPU_ACCESS_READ;
                sd.MiscFlags       = 0;
                streamD3D_->CreateTexture2D(&sd, nullptr, &streamStaging_);
                streamW_ = (int)desc.Width;
                streamH_ = (int)desc.Height;
                PushResolutionChange(streamW_, streamH_);
            }

            if (streamStaging_) {
                streamCtx_->CopyResource(streamStaging_, gpuTex);
                D3D11_MAPPED_SUBRESOURCE mapped;
                if (SUCCEEDED(streamCtx_->Map(streamStaging_, 0,
                        D3D11_MAP_READ, 0, &mapped))) {
                    std::vector<BYTE> jpeg = EncodeFrameJPEG(
                        reinterpret_cast<const BYTE*>(mapped.pData),
                        streamW_, streamH_, (UINT)mapped.RowPitch);
                    streamCtx_->Unmap(streamStaging_, 0);

                    if (!jpeg.empty()) {
                        std::string frame = EncodeBinaryFrame(jpeg.data(), jpeg.size());
                        static constexpr size_t MAX_SEND_BUF = 1 * 1024 * 1024; // 1 MB
                        std::lock_guard<std::mutex> lock(clientsMutex_);
                        for (int i = 0; i < maxClients_; i++) {
                            if (streamingClients_[i] && clients_[i].connected) {
                                if (clients_[i].sendBuffer.size() < MAX_SEND_BUF) {
                                    clients_[i].sendBuffer += frame;
                                }
                                // else: client can't keep up - silently drop frame
                            }
                        }
                    }
                }
            }
            gpuTex->Release();
        }

        res->Release();
        streamDupl_->ReleaseFrame();

        // ~30 fps target
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }

    LOG_INFO(logger_, LOG_CATEGORY_NETWORK, "WsStream", "Stream thread stopped");
}

std::vector<BYTE> AsyncWebSocketServer::EncodeFrameJPEG(
        const BYTE* bgra, int w, int h, UINT rowPitch) {
    using namespace Gdiplus;

    // DXGI BGRA → GDI+ PixelFormat32bppARGB (identical byte layout on little-endian)
    Bitmap bmp(w, h, (INT)rowPitch, PixelFormat32bppARGB, const_cast<BYTE*>(bgra));

    // Find JPEG encoder
    UINT num = 0, size = 0;
    GetImageEncodersSize(&num, &size);
    if (!size) return {};

    std::vector<BYTE> codecBuf(size);
    ImageCodecInfo* pInfo = reinterpret_cast<ImageCodecInfo*>(codecBuf.data());
    GetImageEncoders(num, size, pInfo);

    CLSID jpegClsid = {};
    bool  found     = false;
    for (UINT j = 0; j < num; j++) {
        if (wcscmp(pInfo[j].MimeType, L"image/jpeg") == 0) {
            jpegClsid = pInfo[j].Clsid;
            found = true;
            break;
        }
    }
    if (!found) return {};

    // Encode to IStream
    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) || !stream) return {};

    EncoderParameters params;
    params.Count = 1;
    params.Parameter[0].Guid               = EncoderQuality;
    params.Parameter[0].Type               = EncoderParameterValueTypeLong;
    params.Parameter[0].NumberOfValues     = 1;
    ULONG quality = (ULONG)streamQuality_;
    params.Parameter[0].Value = &quality;

    bmp.Save(stream, &jpegClsid, &params);

    // Copy to vector
    STATSTG stat = {};
    stream->Stat(&stat, STATFLAG_NONAME);
    size_t jpegSize = (size_t)stat.cbSize.QuadPart;

    std::vector<BYTE> result(jpegSize);
    LARGE_INTEGER li = {};
    stream->Seek(li, STREAM_SEEK_SET, nullptr);
    ULONG read = 0;
    stream->Read(result.data(), (ULONG)jpegSize, &read);
    stream->Release();
    result.resize(read);
    return result;
}

std::string AsyncWebSocketServer::EncodeBinaryFrame(const void* data, size_t len) {
    std::string frame;
    frame.push_back((char)0x82);  // FIN + binary opcode
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
    const char* d = static_cast<const char*>(data);
    frame.append(d, len);
    return frame;
}

void AsyncWebSocketServer::StartClientStream(int clientIndex, int requestId) {
    if (clientIndex < 0 || clientIndex >= maxClients_) return;

    if (!streamingClients_[clientIndex]) {
        streamingClients_[clientIndex] = true;
        streamClientCount_++;
        if (streamClientCount_ == 1) {
            StartCapture();
        }
    }

    // ACK + send current resolution if known
    {
        std::lock_guard<std::mutex> lock(clientsMutex_);
        if (!clients_[clientIndex].connected) return;

        if (streamW_ > 0 && streamH_ > 0) {
            std::string resMsg =
                "{\"jsonrpc\":\"2.0\",\"method\":\"display.resolution_change\","
                "\"params\":{\"width\":" + std::to_string(streamW_) +
                ",\"height\":" + std::to_string(streamH_) + "}}";
            clients_[clientIndex].sendBuffer += EncodeWebSocketFrame(resMsg);
        }

        std::string ack =
            "{\"jsonrpc\":\"2.0\",\"result\":{\"streaming\":true},\"id\":" +
            std::to_string(requestId) + "}";
        clients_[clientIndex].sendBuffer += EncodeWebSocketFrame(ack);
    }

    LOG_INFO(logger_, LOG_CATEGORY_NETWORK, "WsStream",
        "Client %d started streaming (active streams=%d)",
        clientIndex, (int)streamClientCount_);
}

void AsyncWebSocketServer::StopClientStream(int clientIndex, int requestId) {
    if (clientIndex < 0 || clientIndex >= maxClients_) return;

    if (streamingClients_[clientIndex]) {
        streamingClients_[clientIndex] = false;
        int prev = streamClientCount_.fetch_sub(1);
        if (prev <= 1) StopCapture();
    }

    std::lock_guard<std::mutex> lock(clientsMutex_);
    if (clients_[clientIndex].connected) {
        std::string ack =
            "{\"jsonrpc\":\"2.0\",\"result\":{\"streaming\":false},\"id\":" +
            std::to_string(requestId) + "}";
        clients_[clientIndex].sendBuffer += EncodeWebSocketFrame(ack);
    }
}

void AsyncWebSocketServer::PushResolutionChange(int w, int h) {
    std::string msg =
        "{\"jsonrpc\":\"2.0\",\"method\":\"display.resolution_change\","
        "\"params\":{\"width\":" + std::to_string(w) +
        ",\"height\":" + std::to_string(h) + "}}";
    std::lock_guard<std::mutex> lock(clientsMutex_);
    for (int i = 0; i < maxClients_; i++) {
        if (streamingClients_[i] && clients_[i].connected) {
            clients_[i].sendBuffer += EncodeWebSocketFrame(msg);
        }
    }
}

// ============================================================
// Factory API (see websocket_server_async.h)
// ============================================================
#include "websocket_server_async.h"

void* WsAsync_Create(int port, int maxClients) { return new AsyncWebSocketServer(port, maxClients); }
bool  WsAsync_Start(void* srv)   { return static_cast<AsyncWebSocketServer*>(srv)->Start(); }
void  WsAsync_Stop(void* srv)    { static_cast<AsyncWebSocketServer*>(srv)->Stop(); }
void  WsAsync_Destroy(void* srv) { delete static_cast<AsyncWebSocketServer*>(srv); }
