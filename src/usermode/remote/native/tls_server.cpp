// tls_server.cpp - TLS 1.3 secure server implementation
// NOTE: This file is currently unused infrastructure.
// - The async WebSocket server (websocket_server_async.cpp) runs plain WS.
// - The VNC server uses its own SChannel wrapper in vnc_tls.h.
// To add TLS to the WebSocket path, instantiate TlsServer here and
// have service.cpp create it on port 8443 in front of AsyncWebSocketServer.
#define SECURITY_WIN32
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <schannel.h>
#include <security.h>
#include <sspi.h>

#ifndef SCH_CRED_MUTUAL_AUTH
#define SCH_CRED_MUTUAL_AUTH 0x00000002
#endif
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <map>
#include <memory>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "secur32.lib")
#pragma comment(lib, "crypt32.lib")

// Credentials structure for TLS
struct TlsCredentials {
    CredHandle hCred;
    BOOL valid;
    
    TlsCredentials() : valid(FALSE) {
        SecInvalidateHandle(&hCred);
    }
    
    ~TlsCredentials() {
        if (valid) {
            FreeCredentialsHandle(&hCred);
        }
    }
};

// TLS connection context
struct TlsConnection {
    SOCKET socket;
    CtxtHandle hContext;
    BOOL validContext;
    SecPkgContext_StreamSizes streamSizes;
    std::vector<BYTE> readBuffer;
    std::vector<BYTE> decryptBuffer;
    std::vector<BYTE> writeBuffer;
    
    TlsConnection(SOCKET s) : socket(s), validContext(FALSE) {
        SecInvalidateHandle(&hContext);
        readBuffer.resize(65536);
        decryptBuffer.resize(65536);
        writeBuffer.resize(65536);
    }
    
    ~TlsConnection() {
        if (validContext) {
            // Send close notify
            SecBufferDesc desc;
            SecBuffer buf;
            DWORD dwType = SCHANNEL_SHUTDOWN;
            
            buf.BufferType = SECBUFFER_TOKEN;
            buf.pvBuffer = &dwType;
            buf.cbBuffer = sizeof(dwType);
            
            desc.ulVersion = SECBUFFER_VERSION;
            desc.cBuffers = 1;
            desc.pBuffers = &buf;
            
            ApplyControlToken(&hContext, &desc);
            
            FreeContextBuffer(&hContext);
        }
        if (socket != INVALID_SOCKET) {
            closesocket(socket);
        }
    }
};

class TlsServer {
public:
    TlsServer(int port = 8443) 
        : port(port)
        , listenSocket(INVALID_SOCKET)
        , running(false)
        , requireClientCert(FALSE) {
        SecInvalidateHandle(&serverCreds.hCred);
    }

    ~TlsServer() {
        Shutdown();
    }

    // Initialize with certificate (self-signed for now, from store in production)
    bool Initialize(LPCWSTR certSubjectName = L"KVM-Server", BOOL clientCert = FALSE) {
        requireClientCert = clientCert;

        // Initialize Schannel credentials
        SCHANNEL_CRED schannelCred = {};
        schannelCred.dwVersion = SCHANNEL_CRED_VERSION;
        schannelCred.grbitEnabledProtocols = SP_PROT_TLS1_3_SERVER;  // TLS 1.3 only
        schannelCred.dwMinimumCipherStrength = 0;
        schannelCred.dwMaximumCipherStrength = 0;
        schannelCred.dwFlags = SCH_USE_STRONG_CRYPTO | SCH_CRED_NO_SYSTEM_MAPPER;

        if (requireClientCert) {
            schannelCred.dwFlags |= SCH_CRED_MUTUAL_AUTH;
        }

        // Set cipher suites for TLS 1.3
        // TLS_AES_256_GCM_SHA384, TLS_CHACHA20_POLY1305_SHA256, TLS_AES_128_GCM_SHA256
        ALG_ID algos[] = { CALG_AES_256, CALG_AES_128, CALG_SHA_384, CALG_SHA_256 };
        schannelCred.palgSupportedAlgs = algos;
        schannelCred.cSupportedAlgs = ARRAYSIZE(algos);

        // Acquire credentials
        TimeStamp expiry;
        SECURITY_STATUS status = AcquireCredentialsHandle(
            NULL,
            UNISP_NAME,
            SECPKG_CRED_INBOUND,
            NULL,
            &schannelCred,
            NULL,
            NULL,
            &serverCreds.hCred,
            &expiry
        );

        if (status != SEC_E_OK) {
            std::cerr << "[TlsServer] AcquireCredentialsHandle failed: 0x" << std::hex << status << std::endl;
            return false;
        }

        serverCreds.valid = TRUE;
        std::cout << "[TlsServer] TLS 1.3 credentials acquired" << std::endl;

        return true;
    }

    bool Start() {
        if (!serverCreds.valid) {
            std::cerr << "[TlsServer] Not initialized" << std::endl;
            return false;
        }

        // Initialize Winsock
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            std::cerr << "[TlsServer] WSAStartup failed" << std::endl;
            return false;
        }

        // Create socket
        listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSocket == INVALID_SOCKET) {
            std::cerr << "[TlsServer] socket failed: " << WSAGetLastError() << std::endl;
            WSACleanup();
            return false;
        }

        // Enable address reuse
        int opt = 1;
        setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

        // Bind
        sockaddr_in service = {};
        service.sin_family = AF_INET;
        service.sin_addr.s_addr = INADDR_ANY;
        service.sin_port = htons(port);

        if (bind(listenSocket, (sockaddr*)&service, sizeof(service)) == SOCKET_ERROR) {
            std::cerr << "[TlsServer] bind failed: " << WSAGetLastError() << std::endl;
            closesocket(listenSocket);
            listenSocket = INVALID_SOCKET;
            WSACleanup();
            return false;
        }

        // Listen
        if (listen(listenSocket, SOMAXCONN) == SOCKET_ERROR) {
            std::cerr << "[TlsServer] listen failed: " << WSAGetLastError() << std::endl;
            closesocket(listenSocket);
            listenSocket = INVALID_SOCKET;
            WSACleanup();
            return false;
        }

        running = true;
        acceptThread = std::thread(&TlsServer::AcceptLoop, this);

        std::cout << "[TlsServer] Listening on port " << port << " (TLS 1.3)" << std::endl;
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

        WSACleanup();
    }

    void Shutdown() {
        Stop();
    }

    // Send data over TLS
    bool Send(TlsConnection* conn, const void* data, size_t len) {
        if (!conn || !conn->validContext) return false;

        SecBuffer buffers[4];
        SecBufferDesc desc;
        SECURITY_STATUS status;
        DWORD cbData;

        // Encrypt message
        buffers[0].BufferType = SECBUFFER_STREAM_HEADER;
        buffers[0].pvBuffer = conn->writeBuffer.data();
        buffers[0].cbBuffer = conn->streamSizes.cbHeader;

        buffers[1].BufferType = SECBUFFER_DATA;
        buffers[1].pvBuffer = (PBYTE)data;
        buffers[1].cbBuffer = (ULONG)len;

        buffers[2].BufferType = SECBUFFER_STREAM_TRAILER;
        buffers[2].pvBuffer = conn->writeBuffer.data() + conn->streamSizes.cbHeader + len;
        buffers[2].cbBuffer = conn->streamSizes.cbTrailer;

        buffers[3].BufferType = SECBUFFER_EMPTY;

        desc.ulVersion = SECBUFFER_VERSION;
        desc.cBuffers = 4;
        desc.pBuffers = buffers;

        status = EncryptMessage(&conn->hContext, 0, &desc, 0);
        if (status != SEC_E_OK) {
            return false;
        }

        // Send encrypted data
        cbData = buffers[0].cbBuffer + buffers[1].cbBuffer + buffers[2].cbBuffer;
        int sent = send(conn->socket, (char*)conn->writeBuffer.data(), cbData, 0);
        
        return sent == (int)cbData;
    }

    // Receive data over TLS
    bool Receive(TlsConnection* conn, std::vector<BYTE>& outData) {
        if (!conn || !conn->validContext) return false;

        SecBuffer buffers[4];
        SecBufferDesc desc;
        SECURITY_STATUS status;
        ULONG cbData;

        // Receive data
        cbData = recv(conn->socket, (char*)conn->readBuffer.data(), conn->readBuffer.size(), 0);
        if (cbData <= 0) {
            return false;
        }

        // Decrypt
        buffers[0].BufferType = SECBUFFER_DATA;
        buffers[0].pvBuffer = conn->readBuffer.data();
        buffers[0].cbBuffer = cbData;

        buffers[1].BufferType = SECBUFFER_EMPTY;
        buffers[2].BufferType = SECBUFFER_EMPTY;
        buffers[3].BufferType = SECBUFFER_EMPTY;

        desc.ulVersion = SECBUFFER_VERSION;
        desc.cBuffers = 4;
        desc.pBuffers = buffers;

        status = DecryptMessage(&conn->hContext, &desc, 0, NULL);
        
        if (status == SEC_E_OK) {
            // Find data buffer
            for (int i = 1; i < 4; i++) {
                if (buffers[i].BufferType == SECBUFFER_DATA) {
                    outData.resize(buffers[i].cbBuffer);
                    memcpy(outData.data(), buffers[i].pvBuffer, buffers[i].cbBuffer);
                    return true;
                }
            }
        }

        return false;
    }

private:
    int port;
    SOCKET listenSocket;
    BOOL running;
    TlsCredentials serverCreds;
    BOOL requireClientCert;
    std::thread acceptThread;

    void AcceptLoop() {
        while (running) {
            sockaddr_in clientAddr;
            int addrLen = sizeof(clientAddr);
            
            SOCKET clientSocket = accept(listenSocket, (sockaddr*)&clientAddr, &addrLen);
            if (clientSocket == INVALID_SOCKET) {
                if (running) {
                    std::cerr << "[TlsServer] accept failed: " << WSAGetLastError() << std::endl;
                }
                continue;
            }

            // Get client IP
            char clientIP[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &clientAddr.sin_addr, clientIP, INET_ADDRSTRLEN);
            std::cout << "[TlsServer] Client connected from " << clientIP << std::endl;

            // Handle TLS handshake in new thread
            std::thread clientThread(&TlsServer::HandleClient, this, clientSocket);
            clientThread.detach();
        }
    }

    void HandleClient(SOCKET clientSocket) {
        auto conn = std::make_unique<TlsConnection>(clientSocket);

        // Perform TLS handshake
        if (!DoHandshake(conn.get())) {
            std::cerr << "[TlsServer] TLS handshake failed" << std::endl;
            return;
        }

        std::cout << "[TlsServer] TLS handshake complete, connection established" << std::endl;

        // Query stream sizes for encryption
        QueryContextAttributes(&conn->hContext, SECPKG_ATTR_STREAM_SIZES, &conn->streamSizes);

        // Connection is now ready for encrypted communication
        // The caller would use Send() and Receive() for application data
        
        // For now, close the connection
        // In production, this would pass conn to a protocol handler
    }

    bool DoHandshake(TlsConnection* conn) {
        SecBuffer outBuffers[2];
        SecBufferDesc outDesc;
        DWORD dwSSPIOutFlags;
        DWORD dwSSPIFlags = ASC_REQ_ALLOCATE_MEMORY | ASC_REQ_STREAM | ASC_REQ_CONFIDENTIALITY;
        
        if (requireClientCert) {
            dwSSPIFlags |= ASC_REQ_MUTUAL_AUTH;
        }

        outBuffers[0].BufferType = SECBUFFER_TOKEN;
        outBuffers[0].pvBuffer = NULL;
        outBuffers[0].cbBuffer = 0;

        outBuffers[1].BufferType = SECBUFFER_EMPTY;

        outDesc.ulVersion = SECBUFFER_VERSION;
        outDesc.cBuffers = 2;
        outDesc.pBuffers = outBuffers;

        TimeStamp tsExpiry;
        SECURITY_STATUS status;
        BOOL first = TRUE;
        
        while (TRUE) {
            if (first) {
                // First call - no input data
                status = AcceptSecurityContext(
                    &serverCreds.hCred,
                    NULL,
                    NULL,
                    dwSSPIFlags,
                    SECURITY_NATIVE_DREP,
                    &conn->hContext,
                    &outDesc,
                    &dwSSPIOutFlags,
                    &tsExpiry
                );
                first = FALSE;
            } else {
                // Receive data from client
                int received = recv(conn->socket, (char*)conn->readBuffer.data(), conn->readBuffer.size(), 0);
                if (received <= 0) {
                    return FALSE;
                }

                SecBuffer inBuffers[2];
                SecBufferDesc inDesc;

                inBuffers[0].BufferType = SECBUFFER_TOKEN;
                inBuffers[0].pvBuffer = conn->readBuffer.data();
                inBuffers[0].cbBuffer = received;

                inBuffers[1].BufferType = SECBUFFER_EMPTY;

                inDesc.ulVersion = SECBUFFER_VERSION;
                inDesc.cBuffers = 2;
                inDesc.pBuffers = inBuffers;

                status = AcceptSecurityContext(
                    &serverCreds.hCred,
                    &conn->hContext,
                    &inDesc,
                    dwSSPIFlags,
                    SECURITY_NATIVE_DREP,
                    &conn->hContext,
                    &outDesc,
                    &dwSSPIOutFlags,
                    &tsExpiry
                );

                // Check for any extra data
                if (inBuffers[1].BufferType == SECBUFFER_EXTRA && inBuffers[1].cbBuffer > 0) {
                    // Copy extra data to beginning of buffer
                    memmove(conn->readBuffer.data(), 
                           (BYTE*)inBuffers[0].pvBuffer + (inBuffers[0].cbBuffer - inBuffers[1].cbBuffer),
                           inBuffers[1].cbBuffer);
                }
            }

            // Send output token if any
            if (outBuffers[0].cbBuffer > 0 && outBuffers[0].pvBuffer) {
                send(conn->socket, (char*)outBuffers[0].pvBuffer, outBuffers[0].cbBuffer, 0);
                FreeContextBuffer(outBuffers[0].pvBuffer);
                outBuffers[0].pvBuffer = NULL;
                outBuffers[0].cbBuffer = 0;
            }

            // Check status
            if (status == SEC_E_OK) {
                conn->validContext = TRUE;
                return TRUE;
            } else if (status == SEC_I_CONTINUE_NEEDED) {
                continue;
            } else {
                std::cerr << "[TlsServer] AcceptSecurityContext failed: 0x" << std::hex << status << std::endl;
                return FALSE;
            }
        }
    }
};

// C interface for integration
extern "C" {
    __declspec(dllexport) void* TlsServer_Create(int port) {
        return new TlsServer(port);
    }

    __declspec(dllexport) bool TlsServer_Init(void* server, bool requireClientCert) {
        return ((TlsServer*)server)->Initialize(L"KVM-Server", requireClientCert ? TRUE : FALSE);
    }

    __declspec(dllexport) bool TlsServer_Start(void* server) {
        return ((TlsServer*)server)->Start();
    }

    __declspec(dllexport) void TlsServer_Stop(void* server) {
        ((TlsServer*)server)->Stop();
    }

    __declspec(dllexport) void TlsServer_Destroy(void* server) {
        delete (TlsServer*)server;
    }
}
