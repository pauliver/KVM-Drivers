// DEPRECATED — This file is NOT compiled or linked into any build target.
// The active implementation is websocket_server_async.cpp (AsyncWebSocketServer),
// which is the server that service.cpp creates via WsAsync_Create().
// This file is kept for historical reference only and should be deleted
// once the async implementation has been in production long enough.

// WebSocket Server - Native protocol handler with JSON-RPC and driver integration
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iostream>
#include <thread>
#include <vector>
#include <string>
#include <sstream>
#include <functional>
#include <atomic>
#include <mutex>
#include <chrono>

// Include driver interface
#include "../../core/driver_interface.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "crypt32.lib")

// Simple JSON parser for RPC (minimal implementation)
class SimpleJsonParser {
public:
    static std::string GetString(const std::string& json, const std::string& key) {
        std::string search = "\"" + key + "\":\"";
        size_t pos = json.find(search);
        if (pos == std::string::npos) return "";
        pos += search.length();
        size_t end = json.find("\"", pos);
        if (end == std::string::npos) return "";
        return json.substr(pos, end - pos);
    }
    
    static int GetInt(const std::string& json, const std::string& key, int defaultVal = 0) {
        std::string search = "\"" + key + "\":";
        size_t pos = json.find(search);
        if (pos == std::string::npos) return defaultVal;
        pos += search.length();
        // Skip whitespace
        while (pos < json.length() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
        size_t end = pos;
        while (end < json.length() && (isdigit(json[end]) || json[end] == '-')) end++;
        try {
            return std::stoi(json.substr(pos, end - pos));
        } catch (...) {
            return defaultVal;
        }
    }
    
    static bool GetBool(const std::string& json, const std::string& key, bool defaultVal = false) {
        std::string search = "\"" + key + "\":";
        size_t pos = json.find(search);
        if (pos == std::string::npos) return defaultVal;
        pos += search.length();
        while (pos < json.length() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
        if (json.substr(pos, 4) == "true") return true;
        if (json.substr(pos, 5) == "false") return false;
        return defaultVal;
    }
};

// SHA1 hash for WebSocket handshake
std::string Base64Encode(const BYTE* data, DWORD len) {
    DWORD base64Len = 0;
    CryptBinaryToStringA(data, len, CRYPT_STRING_BASE64, NULL, &base64Len);
    std::string result(base64Len, 0);
    CryptBinaryToStringA(data, len, CRYPT_STRING_BASE64, &result[0], &base64Len);
    result.resize(base64Len - 1); // Remove null terminator
    return result;
}

std::string ComputeWebSocketAccept(const std::string& key) {
    const std::string magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string combined = key + magic;
    
    HCRYPTPROV hProv;
    HCRYPTHASH hHash;
    BYTE hash[20];
    DWORD hashLen = 20;
    
    CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT);
    CryptCreateHash(hProv, CALG_SHA1, 0, 0, &hHash);
    CryptHashData(hHash, (BYTE*)combined.c_str(), combined.length(), 0);
    CryptGetHashParam(hHash, HP_HASHVAL, hash, &hashLen, 0);
    CryptDestroyHash(hHash);
    CryptReleaseContext(hProv, 0);
    
    return Base64Encode(hash, 20);
}

class WebSocketServer {
public:
    WebSocketServer(int port = 8443) 
        : port(port), running(false), listenSocket(INVALID_SOCKET), driverInterface(nullptr) {
        // Initialize driver interface
        driverInterface = new DriverInterface();
        if (driverInterface) {
            driverInterface->Initialize();
        }
    }
    
    ~WebSocketServer() {
        Stop();
        if (driverInterface) {
            driverInterface->Disconnect();
            delete driverInterface;
        }
    }

    bool Start() {
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            std::cerr << "WSAStartup failed" << std::endl;
            return false;
        }

        listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSocket == INVALID_SOCKET) {
            std::cerr << "socket failed: " << WSAGetLastError() << std::endl;
            WSACleanup();
            return false;
        }

        // Enable address reuse
        int opt = 1;
        setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

        sockaddr_in service;
        service.sin_family = AF_INET;
        service.sin_addr.s_addr = INADDR_ANY;
        service.sin_port = htons(port);

        if (bind(listenSocket, (SOCKADDR*)&service, sizeof(service)) == SOCKET_ERROR) {
            std::cerr << "bind failed: " << WSAGetLastError() << std::endl;
            closesocket(listenSocket);
            WSACleanup();
            return false;
        }

        if (listen(listenSocket, SOMAXCONN) == SOCKET_ERROR) {
            std::cerr << "listen failed: " << WSAGetLastError() << std::endl;
            closesocket(listenSocket);
            WSACleanup();
            return false;
        }

        running = true;
        std::cout << "WebSocket server listening on port " << port << std::endl;

        acceptThread = std::thread(&WebSocketServer::AcceptConnections, this);
        return true;
    }

    void Stop() {
        running = false;
        
        if (listenSocket != INVALID_SOCKET) {
            closesocket(listenSocket);
            listenSocket = INVALID_SOCKET;
        }
        
        if (acceptThread.joinable()) {
            acceptThread.join();
        }
        
        // Wait for all client threads
        {
            std::lock_guard<std::mutex> lock(threadsMutex_);
            for (auto& t : clientThreads_) {
                if (t.joinable()) t.join();
            }
            clientThreads_.clear();
        }
        
        WSACleanup();
    }

private:
    std::vector<std::thread> clientThreads_;
    std::mutex threadsMutex_;
    std::atomic<int> connectionCount_{0};
    static constexpr int MAX_CONNECTIONS = 20;
    static constexpr int SOCKET_TIMEOUT_MS = 30000;

    void AcceptConnections() {
        while (running) {
            // Use select() for non-blocking accept with timeout
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(listenSocket, &readSet);
            timeval tv = {0, 100000}; // 100ms
            
            if (select(0, &readSet, NULL, NULL, &tv) <= 0) {
                continue;
            }
            
            sockaddr_in clientAddr;
            int clientAddrSize = sizeof(clientAddr);
            SOCKET clientSocket = accept(listenSocket, (sockaddr*)&clientAddr, &clientAddrSize);

            if (clientSocket == INVALID_SOCKET) {
                continue;
            }
            
            // Check connection limit
            if (connectionCount_ >= MAX_CONNECTIONS) {
                std::cerr << "Max connections reached, rejecting" << std::endl;
                closesocket(clientSocket);
                continue;
            }
            
            // Set socket timeouts
            DWORD timeout = SOCKET_TIMEOUT_MS;
            setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
            setsockopt(clientSocket, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout));

            char clientIP[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &clientAddr.sin_addr, clientIP, INET_ADDRSTRLEN);
            int clientPort = ntohs(clientAddr.sin_port);
            std::cout << "[WS] Client connected: " << clientIP << ":" << clientPort
                      << " (" << connectionCount_.load() + 1 << "/" << MAX_CONNECTIONS << ")" << std::endl;

            connectionCount_++;

            std::lock_guard<std::mutex> lock(threadsMutex_);
            clientThreads_.emplace_back(&WebSocketServer::HandleClient, this, clientSocket, clientAddr);
        }
    }

    void HandleClient(SOCKET clientSocket, sockaddr_in clientAddr) {
        char clientIP[INET_ADDRSTRLEN] = "unknown";
        inet_ntop(AF_INET, &clientAddr.sin_addr, clientIP, INET_ADDRSTRLEN);
        int clientPort = ntohs(clientAddr.sin_port);

        // Receive HTTP upgrade request
        char buffer[4096];
        int received = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
        if (received <= 0) {
            std::cerr << "[WS] " << clientIP << ":" << clientPort
                      << " disconnected before handshake (recv=" << received << ")" << std::endl;
            connectionCount_--;
            closesocket(clientSocket);
            return;
        }
        buffer[received] = '\0';

        // Parse WebSocket key
        std::string request(buffer);
        std::string wsKey = ParseHeader(request, "Sec-WebSocket-Key");
        
        if (wsKey.empty()) {
            std::cerr << "[WS] " << clientIP << ":" << clientPort
                      << " invalid WebSocket upgrade - no Sec-WebSocket-Key" << std::endl;
            connectionCount_--;
            closesocket(clientSocket);
            return;
        }

        // Send handshake response
        std::string accept = ComputeWebSocketAccept(wsKey);
        std::string response = 
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " + accept + "\r\n"
            "Sec-WebSocket-Protocol: json-rpc\r\n"
            "\r\n";

        send(clientSocket, response.c_str(), (int)response.length(), 0);
        std::cout << "[WS] Handshake complete: " << clientIP << ":" << clientPort << std::endl;

        // Handle WebSocket frames
        HandleWebSocket(clientSocket, clientIP, clientPort);

        std::cout << "[WS] Client disconnected: " << clientIP << ":" << clientPort
                  << " (" << connectionCount_.load() - 1 << "/" << MAX_CONNECTIONS << " remaining)" << std::endl;
        connectionCount_--;
        closesocket(clientSocket);
    }

    // Receive exactly 'len' bytes; returns false on disconnect or error
    static bool RecvExact(SOCKET s, void* buf, int len) {
        int received = 0;
        char* ptr = static_cast<char*>(buf);
        while (received < len) {
            int n = recv(s, ptr + received, len - received, 0);
            if (n <= 0) return false;
            received += n;
        }
        return true;
    }

    void HandleWebSocket(SOCKET clientSocket, const char* clientIP, int clientPort) {
        while (running) {
            // Read WebSocket frame header
            BYTE header[2];
            int ret = recv(clientSocket, (char*)header, 2, 0);
            if (ret != 2) {
                if (ret == 0) {
                    std::cout << "[WS] " << clientIP << ":" << clientPort << " closed connection" << std::endl;
                } else if (ret < 0) {
                    int err = WSAGetLastError();
                    if (err != WSAETIMEDOUT) {
                        std::cerr << "[WS] " << clientIP << ":" << clientPort
                                  << " recv error: " << err << std::endl;
                    } else {
                        std::cerr << "[WS] " << clientIP << ":" << clientPort
                                  << " recv timeout (" << SOCKET_TIMEOUT_MS << "ms)" << std::endl;
                    }
                }
                break;
            }

            bool fin = (header[0] & 0x80) != 0;
            BYTE opcode = header[0] & 0x0F;
            bool masked = (header[1] & 0x80) != 0;
            UINT64 payloadLen = header[1] & 0x7F;

            // Extended payload length
            if (payloadLen == 126) {
                BYTE len16[2] = {};
                if (!RecvExact(clientSocket, len16, 2)) return;
                payloadLen = ((UINT64)len16[0] << 8) | len16[1];
            } else if (payloadLen == 127) {
                BYTE len64[8] = {};
                if (!RecvExact(clientSocket, len64, 8)) return;
                payloadLen = 0;
                for (int i = 0; i < 8; i++)
                    payloadLen = (payloadLen << 8) | len64[i];
            }

            // Reject absurdly large frames before allocating
            if (payloadLen > 16 * 1024 * 1024) {
                std::cerr << "[WS] Frame too large (" << payloadLen << " bytes), closing" << std::endl;
                return;
            }

            // Read masking key
            BYTE maskKey[4] = {0};
            if (masked) {
                if (!RecvExact(clientSocket, maskKey, 4)) return;
            }

            // Read and unmask payload
            std::vector<BYTE> payload(payloadLen);
            UINT64 received = 0;
            while (received < payloadLen) {
                int chunk = recv(clientSocket, (char*)payload.data() + received, 
                    (int)(payloadLen - received), 0);
                if (chunk <= 0) {
                    return;
                }
                received += chunk;
            }

            // Unmask
            if (masked) {
                for (UINT64 i = 0; i < payloadLen; i++) {
                    payload[i] ^= maskKey[i % 4];
                }
            }

            // Handle frame
            switch (opcode) {
            case 0x1: // Text frame
            case 0x2: // Binary frame
                ProcessMessage(clientSocket, std::string(payload.begin(), payload.end()));
                break;
            case 0x8: // Close
                std::cout << "[WS] " << clientIP << ":" << clientPort << " sent close frame" << std::endl;
                SendClose(clientSocket);
                return;
            case 0x9: // Ping
                SendPong(clientSocket, payload);
                break;
            case 0xA: // Pong
                break;
            default:
                std::cerr << "[WS] " << clientIP << ":" << clientPort
                          << " unknown opcode: 0x" << std::hex << (int)opcode << std::dec << std::endl;
                break;
            }
        }
    }

    void ProcessMessage(SOCKET clientSocket, const std::string& message) {
        // Parse JSON-RPC request
        std::cout << "Received: " << message << std::endl;
        
        // Extract method
        std::string method = SimpleJsonParser::GetString(message, "method");
        if (method.empty()) {
            SendErrorResponse(clientSocket, 1, -32600, "Invalid Request");
            return;
        }
        
        // Extract id
        int id = SimpleJsonParser::GetInt(message, "id", 0);
        
        bool success = false;
        std::string result = "{}";
        
        // Dispatch to appropriate handler
        if (method == "input.keyboard.keydown") {
            int keyCode = SimpleJsonParser::GetInt(message, "keyCode", 0);
            int modifiers = SimpleJsonParser::GetInt(message, "modifiers", 0);
            if (driverInterface && keyCode > 0) {
                success = driverInterface->InjectKeyDown((UCHAR)keyCode, (UCHAR)modifiers);
            }
        }
        else if (method == "input.keyboard.keyup") {
            int keyCode = SimpleJsonParser::GetInt(message, "keyCode", 0);
            int modifiers = SimpleJsonParser::GetInt(message, "modifiers", 0);
            if (driverInterface && keyCode > 0) {
                success = driverInterface->InjectKeyUp((UCHAR)keyCode, (UCHAR)modifiers);
            }
        }
        else if (method == "input.mouse.move") {
            int x = SimpleJsonParser::GetInt(message, "x", 0);
            int y = SimpleJsonParser::GetInt(message, "y", 0);
            bool absolute = SimpleJsonParser::GetBool(message, "absolute", false);
            if (driverInterface) {
                success = driverInterface->InjectMouseMove(x, y, absolute);
            }
        }
        else if (method == "input.mouse.button") {
            int button = SimpleJsonParser::GetInt(message, "button", 0);
            bool pressed = SimpleJsonParser::GetBool(message, "pressed", true);
            if (driverInterface) {
                success = driverInterface->InjectMouseButton((UCHAR)button, pressed);
            }
        }
        else if (method == "input.mouse.scroll") {
            int vertical = SimpleJsonParser::GetInt(message, "vertical", 0);
            int horizontal = SimpleJsonParser::GetInt(message, "horizontal", 0);
            if (driverInterface) {
                success = driverInterface->InjectMouseScroll(vertical, horizontal);
            }
        }
        else if (method == "system.ping") {
            success = true;
            result = "\"pong\"";
        }
        else if (method == "system.get_version") {
            success = true;
            result = "{\"version\":\"1.0.0\",\"protocol\":\"2.0\"}";
        }
        else {
            SendErrorResponse(clientSocket, id, -32601, "Method not found: " + method);
            return;
        }
        
        // Send response
        if (success) {
            std::string response = "{\"jsonrpc\":\"2.0\",\"result\":" + result + ",\"id\":" + std::to_string(id) + "}";
            SendTextFrame(clientSocket, response);
        } else {
            SendErrorResponse(clientSocket, id, -32603, "Injection failed");
        }
    }
    
    void SendErrorResponse(SOCKET clientSocket, int id, int code, const std::string& message) {
        std::string response = "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":" + 
                              std::to_string(code) + ",\"message\":\"" + message + "\"},\"id\":" + 
                              std::to_string(id) + "}";
        SendTextFrame(clientSocket, response);
    }

    void SendTextFrame(SOCKET clientSocket, const std::string& message) {
        std::vector<BYTE> frame;
        frame.push_back(0x81); // FIN + text opcode
        
        size_t len = message.length();
        if (len < 126) {
            frame.push_back((BYTE)len);
        } else if (len < 65536) {
            frame.push_back(126);
            frame.push_back((len >> 8) & 0xFF);
            frame.push_back(len & 0xFF);
        } else {
            frame.push_back(127);
            for (int i = 7; i >= 0; i--) {
                frame.push_back((len >> (i * 8)) & 0xFF);
            }
        }

        frame.insert(frame.end(), message.begin(), message.end());
        send(clientSocket, (char*)frame.data(), frame.size(), 0);
    }

    void SendClose(SOCKET clientSocket) {
        BYTE closeFrame[] = { 0x88, 0x00 };
        send(clientSocket, (char*)closeFrame, 2, 0);
    }

    void SendPong(SOCKET clientSocket, const std::vector<BYTE>& payload) {
        std::vector<BYTE> frame;
        frame.push_back(0x8A); // FIN + pong
        frame.push_back((BYTE)payload.size());
        frame.insert(frame.end(), payload.begin(), payload.end());
        send(clientSocket, (char*)frame.data(), frame.size(), 0);
    }

    std::string ParseHeader(const std::string& request, const std::string& header) {
        size_t pos = request.find(header + ": ");
        if (pos == std::string::npos) return "";
        pos += header.length() + 2;
        size_t end = request.find("\r\n", pos);
        return request.substr(pos, end - pos);
    }

    int port;
    bool running;
    SOCKET listenSocket;
    std::thread acceptThread;
    DriverInterface* driverInterface;
};

// C interface for integration
extern "C" {
    __declspec(dllexport) void* WebSocketServer_Create(int port) {
        return new WebSocketServer(port);
    }

    __declspec(dllexport) bool WebSocketServer_Start(void* server) {
        return ((WebSocketServer*)server)->Start();
    }

    __declspec(dllexport) void WebSocketServer_Stop(void* server) {
        ((WebSocketServer*)server)->Stop();
    }

    __declspec(dllexport) void WebSocketServer_Destroy(void* server) {
        delete (WebSocketServer*)server;
    }
}
