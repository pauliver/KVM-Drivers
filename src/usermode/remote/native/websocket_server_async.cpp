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

        // IP allowlist enforcement via ConnectionSecurityContext
        auto& sec = ConnectionSecurityContext::Global();
        if (!sec.ValidateConnection(clientIP, "WebSocket")) {
            LOG_WARNING(logger_, LOG_CATEGORY_SECURITY, "AsyncWebSocket",
                "Connection rejected by IP allowlist: %s", clientIP);
            closesocket(clientSocket);
            return;
        }

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
            // ... other methods
            
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
