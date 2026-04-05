// vnc_server.cpp - VNC Server implementation (RFB 3.8 protocol)
// Supports RealVNC, TightVNC, TigerVNC, UltraVNC clients

#include "vnc_server.h"
#include <ws2tcpip.h>
#include <iostream>
#include <cstring>
#include <zlib.h>
#include <bcrypt.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include "vnc_tls.h"
#include "../../../common/adaptive_quality.h"
#include "../../core/driver_interface.h"

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace KVMDrivers {
namespace Remote {

// RFB Protocol constants
namespace RFB {
    constexpr int ProtocolVersionMajor = 3;
    constexpr int ProtocolVersionMinor = 8;
    
    enum SecurityType {
        SecTypeInvalid = 0,
        SecTypeNone = 1,
        SecTypeVNCAuth = 2,
    };
    
    enum ClientMsgType {
        ClientSetPixelFormat = 0,
        ClientSetEncodings = 2,
        ClientFramebufferUpdateRequest = 3,
        ClientKeyEvent = 4,
        ClientPointerEvent = 5,
        ClientCutText = 6,
    };
    
    enum ServerMsgType {
        ServerFramebufferUpdate = 0,
    };
    
    enum Encoding {
        EncodingRaw = 0,
        EncodingCopyRect = 1,
        EncodingRRE = 2,
        EncodingHextile = 5,
        EncodingZlib = 6,
        EncodingTight = 7,
        EncodingZRLE = 16,
    };
}

// X11 keysym to Windows Virtual Key mapping
UINT32 KeySymMapper::X11ToWindows(UINT32 keysym) {
    // Latin-1 printable: 0x0020-0x007e map directly to VK codes for A-Z/0-9
    if (keysym >= 0x0020 && keysym <= 0x007e) {
        if (keysym >= 'a' && keysym <= 'z') return (UINT32)(keysym - 32);  // uppercase VK
        if (keysym >= 'A' && keysym <= 'Z') return (UINT32)keysym;
        if (keysym >= '0' && keysym <= '9') return (UINT32)keysym;
        switch (keysym) {
        case ' ':  return VK_SPACE;
        case '!':  return '1';      // Shift+1
        case '@':  return '2';
        case '#':  return '3';
        case '$':  return '4';
        case '%':  return '5';
        case '^':  return '6';
        case '&':  return '7';
        case '*':  return '8';
        case '(':  return '9';
        case ')':  return '0';
        case '-': case '_': return VK_OEM_MINUS;
        case '=': case '+': return VK_OEM_PLUS;
        case '[': case '{': return VK_OEM_4;
        case ']': case '}': return VK_OEM_6;
        case '\\': case '|': return VK_OEM_5;
        case ';': case ':': return VK_OEM_1;
        case '\'': case '"': return VK_OEM_7;
        case '`': case '~': return VK_OEM_3;
        case ',': case '<': return VK_OEM_COMMA;
        case '.': case '>': return VK_OEM_PERIOD;
        case '/': case '?': return VK_OEM_2;
        }
        return 0;
    }

    // Function and special keys
    switch (keysym) {
    case 0xff08: return VK_BACK;       // BackSpace
    case 0xff09: return VK_TAB;        // Tab
    case 0xff0d: return VK_RETURN;     // Return
    case 0xff13: return VK_PAUSE;      // Pause
    case 0xff14: return VK_SCROLL;     // Scroll_Lock
    case 0xff1b: return VK_ESCAPE;     // Escape
    case 0xff21: return VK_CAPITAL;    // Caps Lock
    case 0xff50: return VK_HOME;
    case 0xff51: return VK_LEFT;
    case 0xff52: return VK_UP;
    case 0xff53: return VK_RIGHT;
    case 0xff54: return VK_DOWN;
    case 0xff55: return VK_PRIOR;      // Page Up
    case 0xff56: return VK_NEXT;       // Page Down
    case 0xff57: return VK_END;
    case 0xff58: return VK_CLEAR;      // Begin
    case 0xff63: return VK_INSERT;
    case 0xff67: return VK_APPS;       // Menu
    case 0xff6a: return VK_HELP;
    case 0xff7f: return VK_NUMLOCK;
    case 0xff8d: return VK_RETURN;     // KP_Enter
    case 0xff95: return VK_HOME;       // KP_Home
    case 0xff96: return VK_LEFT;       // KP_Left
    case 0xff97: return VK_UP;         // KP_Up
    case 0xff98: return VK_RIGHT;      // KP_Right
    case 0xff99: return VK_DOWN;       // KP_Down
    case 0xff9a: return VK_PRIOR;      // KP_Page_Up
    case 0xff9b: return VK_NEXT;       // KP_Page_Down
    case 0xff9c: return VK_END;        // KP_End
    case 0xff9e: return VK_INSERT;     // KP_Insert
    case 0xff9f: return VK_DELETE;     // KP_Delete
    case 0xffaa: return VK_MULTIPLY;
    case 0xffab: return VK_ADD;
    case 0xffad: return VK_SUBTRACT;
    case 0xffae: return VK_DECIMAL;
    case 0xffaf: return VK_DIVIDE;
    case 0xffb0: return VK_NUMPAD0;
    case 0xffb1: return VK_NUMPAD1;
    case 0xffb2: return VK_NUMPAD2;
    case 0xffb3: return VK_NUMPAD3;
    case 0xffb4: return VK_NUMPAD4;
    case 0xffb5: return VK_NUMPAD5;
    case 0xffb6: return VK_NUMPAD6;
    case 0xffb7: return VK_NUMPAD7;
    case 0xffb8: return VK_NUMPAD8;
    case 0xffb9: return VK_NUMPAD9;
    case 0xffbe: return VK_F1;
    case 0xffbf: return VK_F2;
    case 0xffc0: return VK_F3;
    case 0xffc1: return VK_F4;
    case 0xffc2: return VK_F5;
    case 0xffc3: return VK_F6;
    case 0xffc4: return VK_F7;
    case 0xffc5: return VK_F8;
    case 0xffc6: return VK_F9;
    case 0xffc7: return VK_F10;
    case 0xffc8: return VK_F11;
    case 0xffc9: return VK_F12;
    case 0xffe1: return VK_LSHIFT;
    case 0xffe2: return VK_RSHIFT;
    case 0xffe3: return VK_LCONTROL;
    case 0xffe4: return VK_RCONTROL;
    case 0xffe5: return VK_CAPITAL;
    case 0xffe7: return VK_LWIN;       // Meta_L
    case 0xffe8: return VK_RWIN;       // Meta_R
    case 0xffe9: return VK_LMENU;      // Alt_L
    case 0xffea: return VK_RMENU;      // Alt_R
    case 0xffeb: return VK_LWIN;       // Super_L
    case 0xffec: return VK_RWIN;       // Super_R
    case 0xffed: return VK_LWIN;       // Hyper_L -> Win
    case 0xffee: return VK_RWIN;       // Hyper_R -> Win
    case 0xffff: return VK_DELETE;
    }
    return 0;
}

UINT32 KeySymMapper::WindowsToX11(UINT32 vk) {
    UNREFERENCED_PARAMETER(vk);
    return 0;  // Not needed for input injection direction
}

// Safe recv wrapper - returns false on timeout/disconnect
static bool RecvAll(SOCKET s, char* buf, int len) {
    int received = 0;
    while (received < len) {
        int ret = recv(s, buf + received, len - received, 0);
        if (ret <= 0) return false;
        received += ret;
    }
    return true;
}

// Per-thread TLS context (set for AnonTLS connections, null otherwise)
static thread_local TlsSocket* t_tls = nullptr;

// TLS-aware send: routes through TLS when active, otherwise plain send
static int VncSend(SOCKET s, const void* buf, int len, int /*flags*/ = 0) {
    if (t_tls) return t_tls->Send(buf, len) ? len : SOCKET_ERROR;
    return send(s, static_cast<const char*>(buf), len, 0);
}

// TLS-aware recv: routes through TLS when active, otherwise plain RecvAll
static bool VncRecvAll(SOCKET s, char* buf, int len) {
    if (t_tls) {
        int received = 0;
        while (received < len) {
            int r = t_tls->Recv(buf + received, len - received);
            if (r <= 0) return false;
            received += r;
        }
        return true;
    }
    return RecvAll(s, buf, len);
}

// Per-client input state (tracked per thread, no shared state needed)
struct ClientInputState {
    UCHAR lastButtonMask = 0;  // Tracks button transitions
    LONG  lastX = 0;
    LONG  lastY = 0;
};

// Reverse bits of a byte (required by VNC DES key setup)
static UINT8 VncReverseBits(UINT8 b) {
    b = (UINT8)(((b >> 1) & 0x55u) | ((b & 0x55u) << 1));
    b = (UINT8)(((b >> 2) & 0x33u) | ((b & 0x33u) << 2));
    b = (UINT8)(((b >> 4) & 0x0Fu) | ((b & 0x0Fu) << 4));
    return b;
}

// VNC DES: encrypt 16-byte challenge with bit-reversed 8-byte password key (ECB mode)
static bool VncDesEncrypt(const UINT8 challenge[16], const char* password, UINT8 response[16]) {
    // Build 8-byte DES key from password with bit-reversal
    UINT8 key[8] = {0};
    for (int i = 0; i < 8 && password[i]; i++) {
        key[i] = VncReverseBits((UINT8)password[i]);
    }

    BCRYPT_ALG_HANDLE hAlg = NULL;
    BCRYPT_KEY_HANDLE hKey = NULL;
    bool success = false;

    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_DES_ALGORITHM, NULL, 0)))
        return false;

    // ECB mode - no IV
    BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE,
        (PUCHAR)BCRYPT_CHAIN_MODE_ECB, sizeof(BCRYPT_CHAIN_MODE_ECB), 0);

    if (BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(hAlg, &hKey, NULL, 0, key, 8, 0))) {
        ULONG cbResult = 0;
        // Copy challenge so we can encrypt in-place (BCrypt requires mutable input)
        UINT8 plaintext[16];
        memcpy(plaintext, challenge, 16);
        if (BCRYPT_SUCCESS(BCryptEncrypt(hKey, plaintext, 16, NULL, NULL, 0,
                response, 16, &cbResult, 0))) {
            success = true;
        }
        BCryptDestroyKey(hKey);
    }
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return success;
}

class VncServerImpl {
public:
    VncServerImpl(int port = 5900) 
        : port_(port), running_(false), listenSocket_(INVALID_SOCKET)
        , framebufferWidth_(1920), framebufferHeight_(1080)
        , tlsEnabled_(false), tlsCert_(nullptr) {
        // Pre-allocate framebuffer to avoid per-request heap allocation
        framebuffer_.resize((size_t)framebufferWidth_ * framebufferHeight_ * 4, 0);
        // Initialize driver interface for input injection
        driverInterface_ = new DriverInterface();
        driverInterface_->Initialize();
    }

    void SetPassword(const std::string& pw) { password_ = pw; }

    // Enable AnonTLS: create or load self-signed cert and wrap connections
    void SetTlsEnabled(bool enabled) {
        tlsEnabled_ = enabled;
        if (enabled && !tlsCert_) {
            tlsCert_ = TlsSocket::CreateSelfSignedCert(L"CN=KVM-Drivers-VNC");
            if (tlsCert_) {
                std::cout << "[VNC] TLS self-signed cert created" << std::endl;
            } else {
                std::cerr << "[VNC] Failed to create TLS cert, AnonTLS disabled" << std::endl;
                tlsEnabled_ = false;
            }
        }
    }

    ~VncServerImpl() {
        if (tlsCert_) { CertFreeCertificateContext(tlsCert_); }
        delete driverInterface_;
    }

    bool Start() {
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            return false;
        }

        listenSocket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSocket_ == INVALID_SOCKET) {
            WSACleanup();
            return false;
        }

        sockaddr_in service = {};
        service.sin_family = AF_INET;
        service.sin_addr.s_addr = INADDR_ANY;
        service.sin_port = htons(port_);

        if (bind(listenSocket_, (sockaddr*)&service, sizeof(service)) == SOCKET_ERROR) {
            closesocket(listenSocket_);
            WSACleanup();
            return false;
        }

        if (listen(listenSocket_, SOMAXCONN) == SOCKET_ERROR) {
            closesocket(listenSocket_);
            WSACleanup();
            return false;
        }

        running_ = true;
        StartCapture();
        acceptThread_ = std::thread(&VncServerImpl::AcceptLoop, this);
        return true;
    }

    void Stop() {
        running_ = false;

        // Close listen socket to unblock accept
        if (listenSocket_ != INVALID_SOCKET) {
            closesocket(listenSocket_);
            listenSocket_ = INVALID_SOCKET;
        }

        // Wait for accept thread
        if (acceptThread_.joinable()) {
            acceptThread_.join();
        }

        // Client threads are detached (see AcceptLoop); wait for them to drain
        // by polling connectionCount_ with a timeout.
        constexpr int DRAIN_TIMEOUT_MS = 5000;
        for (int waited = 0; connectionCount_ > 0 && waited < DRAIN_TIMEOUT_MS; waited += 50)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

        StopCapture();
        WSACleanup();
    }

private:
    int port_;
    bool running_;
    SOCKET listenSocket_;
    std::thread acceptThread_;

    std::atomic<int> connectionCount_{0};
    static constexpr int MAX_CONNECTIONS = 10;
    static constexpr int SOCKET_TIMEOUT_MS = 30000;  // 30 seconds

    int framebufferWidth_;
    int framebufferHeight_;
    std::vector<char> framebuffer_;  // Pre-allocated, avoids per-frame heap alloc
    std::mutex framebufferMutex_;

    // Note: negotiatedEncodings_ removed - it was a shared class member written by all
    // client threads (data race). Each HandleClient thread now carries its own local
    // std::vector<INT32> passed by reference into HandleSetEncodings/HandleUpdateRequest.

    // Adaptive quality - shared across all clients, degrades globally under load
    AdaptiveQuality adaptiveQuality_;

    // Driver interface for input injection
    DriverInterface* driverInterface_;
    std::string password_;  // Empty = no auth required

    // AnonTLS
    bool             tlsEnabled_;
    PCCERT_CONTEXT   tlsCert_;

    // DXGI Desktop Duplication capture
    ID3D11Device*            d3dDevice_    = nullptr;
    ID3D11DeviceContext*     d3dContext_   = nullptr;
    IDXGIOutputDuplication*  deskDupl_     = nullptr;
    std::thread              captureThread_;
    std::atomic<bool>        captureRunning_{false};

    bool InitCapture() {
        D3D_FEATURE_LEVEL featureLevels[] = {
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0
        };
        D3D_FEATURE_LEVEL obtained;
        HRESULT hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            featureLevels, ARRAYSIZE(featureLevels),
            D3D11_SDK_VERSION,
            &d3dDevice_, &obtained, &d3dContext_);
        if (FAILED(hr)) {
            std::cerr << "[VNC] D3D11CreateDevice failed: 0x" << std::hex << hr << std::dec << std::endl;
            return false;
        }

        IDXGIDevice*  dxgiDev    = nullptr;
        IDXGIAdapter* dxgiAdapt  = nullptr;
        IDXGIOutput*  dxgiOut    = nullptr;
        IDXGIOutput1* dxgiOut1   = nullptr;

        d3dDevice_->QueryInterface(__uuidof(IDXGIDevice),  (void**)&dxgiDev);
        dxgiDev->GetAdapter(&dxgiAdapt);
        dxgiAdapt->EnumOutputs(0, &dxgiOut);
        hr = dxgiOut->QueryInterface(__uuidof(IDXGIOutput1), (void**)&dxgiOut1);

        if (SUCCEEDED(hr)) {
            hr = dxgiOut1->DuplicateOutput(d3dDevice_, &deskDupl_);
            dxgiOut1->Release();
        }

        if (dxgiOut)   dxgiOut->Release();
        if (dxgiAdapt) dxgiAdapt->Release();
        if (dxgiDev)   dxgiDev->Release();

        if (FAILED(hr)) {
            std::cerr << "[VNC] DuplicateOutput failed: 0x" << std::hex << hr << std::dec << std::endl;
            return false;
        }

        // Resize framebuffer to match the actual output dimensions
        DXGI_OUTDUPL_DESC dd;
        deskDupl_->GetDesc(&dd);
        {
            std::lock_guard<std::mutex> lk(framebufferMutex_);
            framebufferWidth_  = (int)dd.ModeDesc.Width;
            framebufferHeight_ = (int)dd.ModeDesc.Height;
            framebuffer_.assign((size_t)framebufferWidth_ * framebufferHeight_ * 4, 0);
        }

        std::cout << "[VNC] DXGI capture: "
                  << framebufferWidth_ << "x" << framebufferHeight_ << std::endl;
        return true;
    }

    void CaptureLoop() {
        // Staging texture for CPU readback
        ID3D11Texture2D* stagingTex = nullptr;

        while (captureRunning_) {
            // Re-initialise duplication after access-lost
            if (!deskDupl_) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                InitCapture();
                continue;
            }

            IDXGIResource*      res   = nullptr;
            DXGI_OUTDUPL_FRAME_INFO fi  = {};
            HRESULT hr = deskDupl_->AcquireNextFrame(100, &fi, &res);

            if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;

            if (hr == DXGI_ERROR_ACCESS_LOST) {
                deskDupl_->Release(); deskDupl_ = nullptr;
                if (stagingTex) { stagingTex->Release(); stagingTex = nullptr; }
                // Also release D3D device/context so InitCapture() recreates them
                // cleanly without leaking the old COM references.
                if (d3dContext_) { d3dContext_->Release(); d3dContext_ = nullptr; }
                if (d3dDevice_)  { d3dDevice_->Release();  d3dDevice_  = nullptr; }
                continue;
            }

            if (FAILED(hr)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            // Get GPU texture
            ID3D11Texture2D* gpuTex = nullptr;
            res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&gpuTex);

            if (gpuTex) {
                // Create or recreate staging texture
                D3D11_TEXTURE2D_DESC desc;
                gpuTex->GetDesc(&desc);

                // Recreate staging texture if it doesn't exist or dimensions changed
                D3D11_TEXTURE2D_DESC stagingDesc;
                bool needNew = !stagingTex;
                if (stagingTex) {
                    stagingTex->GetDesc(&stagingDesc);
                    needNew = (stagingDesc.Width != desc.Width ||
                               stagingDesc.Height != desc.Height);
                }
                if (needNew) {
                    if (stagingTex) { stagingTex->Release(); stagingTex = nullptr; }
                    stagingDesc            = desc;
                    stagingDesc.Usage      = D3D11_USAGE_STAGING;
                    stagingDesc.BindFlags  = 0;
                    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    stagingDesc.MiscFlags  = 0;
                    d3dDevice_->CreateTexture2D(&stagingDesc, nullptr, &stagingTex);
                    if (stagingTex) {
                        // Resize shared framebuffer to match new resolution
                        std::lock_guard<std::mutex> lk(framebufferMutex_);
                        framebufferWidth_  = (int)desc.Width;
                        framebufferHeight_ = (int)desc.Height;
                        framebuffer_.assign((size_t)framebufferWidth_ * framebufferHeight_ * 4, 0);
                    }
                }

                if (stagingTex) {
                    d3dContext_->CopyResource(stagingTex, gpuTex);

                    D3D11_MAPPED_SUBRESOURCE mapped;
                    if (SUCCEEDED(d3dContext_->Map(stagingTex, 0,
                            D3D11_MAP_READ, 0, &mapped))) {
                        std::lock_guard<std::mutex> lk(framebufferMutex_);
                        int w = framebufferWidth_;
                        int h = framebufferHeight_;
                        const BYTE* src = reinterpret_cast<const BYTE*>(mapped.pData);
                        BYTE*       dst = reinterpret_cast<BYTE*>(framebuffer_.data());
                        UINT rowBytes = (UINT)w * 4;
                        for (int row = 0; row < h; row++) {
                            memcpy(dst + (size_t)row * rowBytes,
                                   src + (size_t)row * mapped.RowPitch,
                                   rowBytes);
                        }
                        d3dContext_->Unmap(stagingTex, 0);
                    }
                }
                gpuTex->Release();
            }

            res->Release();
            deskDupl_->ReleaseFrame();

            // Target ~30 fps for capture (VNC clients request updates individually)
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
        }

        if (stagingTex) { stagingTex->Release(); stagingTex = nullptr; }
    }

    void StartCapture() {
        if (!InitCapture()) {
            std::cerr << "[VNC] DXGI capture unavailable; framebuffer will remain black" << std::endl;
            return;
        }
        captureRunning_ = true;
        captureThread_ = std::thread(&VncServerImpl::CaptureLoop, this);
    }

    void StopCapture() {
        captureRunning_ = false;
        if (captureThread_.joinable()) captureThread_.join();
        if (deskDupl_)  { deskDupl_->Release();  deskDupl_   = nullptr; }
        if (d3dContext_){ d3dContext_->Release(); d3dContext_ = nullptr; }
        if (d3dDevice_) { d3dDevice_->Release();  d3dDevice_  = nullptr; }
    }

    void AcceptLoop() {
        while (running_) {
            // Use select() instead of blocking accept
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(listenSocket_, &readSet);
            timeval tv = {0, 100000}; // 100ms timeout
            
            if (select(0, &readSet, NULL, NULL, &tv) <= 0) {
                continue;  // Timeout or error, check running_ flag
            }
            
            sockaddr_in clientAddr;
            int addrLen = sizeof(clientAddr);
            SOCKET clientSocket = accept(listenSocket_, (sockaddr*)&clientAddr, &addrLen);

            if (clientSocket == INVALID_SOCKET) {
                continue;
            }
            
            // Check connection limit
            if (connectionCount_ >= MAX_CONNECTIONS) {
                closesocket(clientSocket);
                continue;
            }
            
            // Set socket timeouts to prevent blocking
            DWORD timeout = SOCKET_TIMEOUT_MS;
            setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
            setsockopt(clientSocket, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout));
            
            connectionCount_++;

            // Detach client threads so the vector never accumulates dead entries.
            // connectionCount_ is decremented when the thread finishes, so
            // Stop() can drain them without holding any references.
            std::thread t(&VncServerImpl::HandleClient, this, clientSocket);
            t.detach();
        }
    }

    void HandleClient(SOCKET clientSocket) {
        // Log connection
        sockaddr_in clientAddr = {};
        int addrLen = sizeof(clientAddr);
        getpeername(clientSocket, (sockaddr*)&clientAddr, &addrLen);
        char clientIP[INET_ADDRSTRLEN] = "unknown";
        inet_ntop(AF_INET, &clientAddr.sin_addr, clientIP, sizeof(clientIP));
        int clientPort = ntohs(clientAddr.sin_port);
        std::cout << "[VNC] Client connected: " << clientIP << ":" << clientPort << std::endl;

        // RFB 3.8 Handshake
        const char version[] = "RFB 003.008\n";
        if (VncSend(clientSocket, version, 12) != 12) {
            std::cerr << "[VNC] Failed to send version to " << clientIP << std::endl;
            goto cleanup;
        }

        {
            char clientVersion[13] = {};
            if (!VncRecvAll(clientSocket, clientVersion, 12)) {
                std::cerr << "[VNC] Client " << clientIP << " disconnected during version exchange" << std::endl;
                goto cleanup;
            }
            clientVersion[12] = '\0';
            std::cout << "[VNC] Client version: " << clientVersion;
        }

        {
            // Build security type list: AnonTLS (18) upgrades to TLS then offers inner auth
            std::vector<char> secList;
            if (tlsEnabled_)                 secList.push_back((char)RFB::SecTypeAnonTLS);
            if (!password_.empty())          secList.push_back((char)RFB::SecTypeVNCAuth);
            if (secList.empty())             secList.push_back((char)RFB::SecTypeNone);

            char nTypes = (char)secList.size();
            VncSend(clientSocket, &nTypes, 1);
            VncSend(clientSocket, secList.data(), (int)secList.size());

            char clientChoice;
            if (!VncRecvAll(clientSocket, &clientChoice, 1)) goto cleanup;
            std::cout << "[VNC] " << clientIP << " chose security type " << (int)clientChoice << std::endl;

            if (clientChoice == (char)RFB::SecTypeAnonTLS) {
                // Upgrade socket to TLS; all subsequent I/O goes through t_tls
                auto* tls = new TlsSocket(clientSocket, tlsCert_);
                if (!tls->ServerHandshake()) {
                    std::cerr << "[VNC] " << clientIP << " TLS handshake failed" << std::endl;
                    delete tls;
                    goto cleanup;
                }
                std::cout << "[VNC] " << clientIP << " TLS established" << std::endl;
                t_tls = tls;  // VncSend/VncRecvAll now route through TLS

                // Re-send inner security type list over TLS channel
                std::vector<char> innerSec;
                if (!password_.empty()) innerSec.push_back((char)RFB::SecTypeVNCAuth);
                else                    innerSec.push_back((char)RFB::SecTypeNone);
                char nInner = (char)innerSec.size();
                VncSend(clientSocket, &nInner, 1);
                VncSend(clientSocket, innerSec.data(), (int)innerSec.size());

                char innerChoice;
                if (!VncRecvAll(clientSocket, &innerChoice, 1)) goto cleanup;

                if (innerChoice == (char)RFB::SecTypeVNCAuth) {
                    if (!DoVNCAuth(clientSocket)) goto cleanup;
                } else {
                    UINT32 secOk = 0;
                    VncSend(clientSocket, &secOk, 4);
                }
                // Fall through to Client Init / Server Init / message loop
                // (all subsequent calls use VncSend/VncRecvAll which route through t_tls)
            } else if (clientChoice == (char)RFB::SecTypeVNCAuth) {
                std::cout << "[VNC] " << clientIP << " attempting VNCAuth" << std::endl;
                if (!DoVNCAuth(clientSocket)) {
                    std::cerr << "[VNC] " << clientIP << " authentication failed" << std::endl;
                    goto cleanup;
                }
                std::cout << "[VNC] " << clientIP << " authenticated" << std::endl;
            } else {
                // None (or unrecognised — send OK)
                std::cout << "[VNC] " << clientIP << " no auth" << std::endl;
                UINT32 secStatus = 0;
                VncSend(clientSocket, (char*)&secStatus, 4);
            }

            // Client init
            char shared;
            if (!VncRecvAll(clientSocket, &shared, 1)) goto cleanup;
            std::cout << "[VNC] Client init, shared=" << (int)shared << std::endl;

            // Server init
            UINT16 width = htons((u_short)framebufferWidth_);
            UINT16 height = htons((u_short)framebufferHeight_);
            VncSend(clientSocket, (char*)&width, 2);
            VncSend(clientSocket, (char*)&height, 2);

            // Pixel format: 32bpp, depth 24, little-endian, true-colour
            // DXGI format is BGRX: B at bits 0-7, G at 8-15, R at 16-23
            // RFB CARD16 values are big-endian in the protocol message
            char pixelFormat[16] = {
                32,       // bits-per-pixel
                24,       // depth
                0,        // big-endian-flag (0 = little-endian pixel layout)
                1,        // true-colour-flag
                0, 255,   // red-max   = 255  (big-endian CARD16)
                0, 255,   // green-max = 255
                0, 255,   // blue-max  = 255
                16,       // red-shift   (R is at bits 16-23 in BGRA)
                8,        // green-shift (G is at bits  8-15)
                0,        // blue-shift  (B is at bits  0-7)
                0, 0, 0   // 3 padding bytes
            };
            VncSend(clientSocket, pixelFormat, 16);

            // Desktop name
            const char name[] = "KVM-Drivers VNC";
            UINT32 nameLen = htonl((u_long)strlen(name));
            VncSend(clientSocket, (char*)&nameLen, 4);
            VncSend(clientSocket, name, (int)strlen(name));
            std::cout << "[VNC] Handshake complete with " << clientIP << std::endl;

            // Per-client state (local to this thread - no locking needed)
            ClientInputState inputState;
            std::vector<INT32> clientEncodings;  // per-connection, replaces shared negotiatedEncodings_

            // Main message loop
            while (running_) {
                char msgType;
                if (!VncRecvAll(clientSocket, &msgType, 1)) break;

                switch ((RFB::ClientMsgType)msgType) {
                case RFB::ClientSetPixelFormat:
                    HandleSetPixelFormat(clientSocket);
                    break;
                case RFB::ClientSetEncodings:
                    HandleSetEncodings(clientSocket, clientEncodings);
                    break;
                case RFB::ClientFramebufferUpdateRequest:
                    HandleUpdateRequest(clientSocket, clientEncodings);
                    break;
                case RFB::ClientKeyEvent:
                    HandleKeyEvent(clientSocket);
                    break;
                case RFB::ClientPointerEvent:
                    HandlePointerEvent(clientSocket, inputState);
                    break;
                case RFB::ClientCutText:
                    HandleCutText(clientSocket);
                    break;
                default:
                    std::cerr << "[VNC] Unknown message type: " << (int)msgType
                              << " from " << clientIP << " - closing" << std::endl;
                    goto cleanup_inner;
                }
            }
        }

        cleanup_inner:;
        cleanup:
        // Clean up per-thread TLS context if this was an AnonTLS connection
        if (t_tls) { delete t_tls; t_tls = nullptr; }
        std::cout << "[VNC] Client disconnected: " << clientIP << ":" << clientPort << std::endl;
        connectionCount_--;
        closesocket(clientSocket);
    }

    // Hextile encoding constants
    static constexpr int HT_Raw             = 1;
    static constexpr int HT_BackgroundSpec  = 2;
    static constexpr int HT_ForegroundSpec  = 4;
    static constexpr int HT_AnySubrects     = 8;
    static constexpr int HT_SubrectsColored = 16;
    static constexpr int TILE_SIZE          = 16;

    // Send Hextile-encoded data for a rect. Must be called with framebufferMutex_ held.
    void SendHextileData(SOCKET sock, int rx, int ry, int rw, int rh) {
        // Each 16x16 tile: detect uniform color and emit compact representation,
        // or fall back to Raw for complex tiles.
        std::vector<UINT8> tileBuf;
        tileBuf.reserve(TILE_SIZE * TILE_SIZE * 4 + 16);

        UINT32 lastBG = 0;
        bool firstTile = true;

        for (int ty = 0; ty < rh; ty += TILE_SIZE) {
            int th = std::min(TILE_SIZE, rh - ty);
            for (int tx = 0; tx < rw; tx += TILE_SIZE) {
                int tw = std::min(TILE_SIZE, rw - tx);

                int fbX = rx + tx;
                int fbY = ry + ty;

                // Sample tile pixels from framebuffer (BGRX 32-bit)
                const UINT32* fb = reinterpret_cast<const UINT32*>(framebuffer_.data());

                // Check if tile is uniform (all same color)
                UINT32 firstPx = 0;
                bool uniform = true;

                for (int py = 0; py < th && uniform; py++) {
                    for (int px = 0; px < tw; px++) {
                        size_t idx = (size_t)(fbY + py) * framebufferWidth_ + (fbX + px);
                        if (idx >= framebuffer_.size() / 4) { uniform = false; break; }
                        UINT32 pxVal = fb[idx] & 0x00FFFFFF;  // ignore alpha
                        if (py == 0 && px == 0) { firstPx = pxVal; }
                        else if (pxVal != firstPx) { uniform = false; break; }
                    }
                }

                tileBuf.clear();

                if (uniform) {
                    // Uniform tile: send BackgroundSpecified with no subrects
                    bool bgChanged = firstTile || (firstPx != lastBG);
                    UINT8 subtype = bgChanged ? (UINT8)HT_BackgroundSpec : 0;
                    tileBuf.push_back(subtype);
                    if (bgChanged) {
                        // Send pixel bytes in BGRX memory order so the client
                        // reads them as a little-endian uint32 with
                        // R at bits 16-23, G at 8-15, B at 0-7 — matching our
                        // declared pixel format (red-shift=16, green-shift=8).
                        tileBuf.push_back((UINT8)( firstPx        & 0xFF));  // B (bits 0-7)
                        tileBuf.push_back((UINT8)((firstPx >>  8) & 0xFF));  // G (bits 8-15)
                        tileBuf.push_back((UINT8)((firstPx >> 16) & 0xFF));  // R (bits 16-23)
                        tileBuf.push_back(0);                                 // unused
                        lastBG = firstPx;
                    }
                    firstTile = false;
                } else {
                    // Complex tile: build subrects for foreground color runs,
                    // fall back to Raw if subrects would be larger
                    
                    // Collect unique non-background colors as candidate subrects
                    // Strategy: use Raw for complex tiles with > 4 colors
                    UINT32 colors[8];
                    int nColors = 0;
                    bool tooMany = false;

                    for (int py = 0; py < th && !tooMany; py++) {
                        for (int px = 0; px < tw; px++) {
                            size_t idx = (size_t)(fbY + py) * framebufferWidth_ + (fbX + px);
                            if (idx >= framebuffer_.size() / 4) continue;
                            UINT32 pxVal = fb[idx] & 0x00FFFFFF;
                            bool found = false;
                            for (int c = 0; c < nColors; c++) {
                                if (colors[c] == pxVal) { found = true; break; }
                            }
                            if (!found) {
                                if (nColors >= 8) { tooMany = true; break; }
                                colors[nColors++] = pxVal;
                            }
                        }
                    }

                    if (tooMany || nColors > 4) {
                        // Raw tile
                        UINT8 subtype = (UINT8)HT_Raw;
                        tileBuf.push_back(subtype);
                        for (int py = 0; py < th; py++) {
                            for (int px = 0; px < tw; px++) {
                                size_t idx = (size_t)(fbY + py) * framebufferWidth_ + (fbX + px);
                                UINT32 pxVal = (idx < framebuffer_.size() / 4) ? fb[idx] : 0;
                                tileBuf.push_back((UINT8)(pxVal        & 0xFF));
                                tileBuf.push_back((UINT8)((pxVal >> 8) & 0xFF));
                                tileBuf.push_back((UINT8)((pxVal >>16) & 0xFF));
                                tileBuf.push_back(0);
                            }
                        }
                    } else {
                        // Subrect encoding: 2-color tiles use FG + subrects
                        // Background = most frequent color, FG = the rest
                        UINT32 bgColor = colors[0];  // use first as background
                        UINT32 fgColor = colors[1 % nColors];

                        bool bgChanged = firstTile || (bgColor != lastBG);
                        UINT8 subtype = (UINT8)(HT_ForegroundSpec | HT_AnySubrects |
                            (bgChanged ? HT_BackgroundSpec : 0) |
                            (nColors > 2 ? HT_SubrectsColored : 0));
                        tileBuf.push_back(subtype);

                        if (bgChanged) {
                            // Background BGRX bytes
                            tileBuf.push_back((UINT8)(bgColor        & 0xFF));
                            tileBuf.push_back((UINT8)((bgColor >>  8) & 0xFF));
                            tileBuf.push_back((UINT8)((bgColor >> 16) & 0xFF));
                            tileBuf.push_back(0);
                            lastBG = bgColor;
                        }
                        // Foreground BGRX bytes
                        tileBuf.push_back((UINT8)(fgColor        & 0xFF));
                        tileBuf.push_back((UINT8)((fgColor >>  8) & 0xFF));
                        tileBuf.push_back((UINT8)((fgColor >> 16) & 0xFF));
                        tileBuf.push_back(0);

                        // Count and encode subrects (1x1 runs of non-BG pixels)
                        std::vector<UINT8> subrects;
                        for (int py = 0; py < th; py++) {
                            for (int px = 0; px < tw; px++) {
                                size_t idx = (size_t)(fbY + py) * framebufferWidth_ + (fbX + px);
                                if (idx >= framebuffer_.size() / 4) continue;
                                UINT32 pxVal = fb[idx] & 0x00FFFFFF;
                                if (pxVal != bgColor) {
                                    if (nColors > 2) {
                                        // SubrectsColoured: color + pos
                                        // Subrect color in BGRX byte order
                                        subrects.push_back((UINT8)( pxVal        & 0xFF));
                                        subrects.push_back((UINT8)((pxVal >>  8) & 0xFF));
                                        subrects.push_back((UINT8)((pxVal >> 16) & 0xFF));
                                        subrects.push_back(0);
                                    }
                                    // subrect: hi4=x, lo4=y, hi4=w-1, lo4=h-1
                                    subrects.push_back((UINT8)((px << 4) | (py & 0xF)));
                                    subrects.push_back((UINT8)(0x00));  // 1x1 subrect (w-1=0, h-1=0)
                                }
                            }
                        }
                        // Subrect count (1 byte)
                        tileBuf.push_back((UINT8)(subrects.size() / (nColors > 2 ? 6 : 2)));
                        tileBuf.insert(tileBuf.end(), subrects.begin(), subrects.end());
                        firstTile = false;
                    }
                }

                VncSend(sock, (char*)tileBuf.data(), (int)tileBuf.size());
            }
        }
    }

    // VNC authentication: generate challenge, verify DES response
    bool DoVNCAuth(SOCKET sock) {
        // Generate 16-byte random challenge
        UINT8 challenge[16];
        if (!BCRYPT_SUCCESS(BCryptGenRandom(NULL, challenge, 16,
                BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
            std::cerr << "[VNC] BCryptGenRandom failed" << std::endl;
            return false;
        }

        // Send challenge to client
        VncSend(sock, (char*)challenge, 16);

        // Receive client's 16-byte DES-encrypted response
        UINT8 response[16];
        if (!VncRecvAll(sock, (char*)response, 16)) {
            std::cerr << "[VNC] Failed to receive auth response" << std::endl;
            return false;
        }

        // Compute expected response using our password
        UINT8 expected[16] = {};
        bool encrypted = VncDesEncrypt(challenge, password_.c_str(), expected);

        bool authOk = encrypted && (memcmp(response, expected, 16) == 0);

        // Send security result: 0 = OK, 1 = failed
        UINT32 result = htonl(authOk ? 0u : 1u);
        VncSend(sock, (char*)&result, 4);

        if (!authOk) {
            // RFB 3.8: send failure reason string
            const char reason[] = "Authentication failed";
            UINT32 len = htonl((u_long)strlen(reason));
            VncSend(sock, (char*)&len, 4);
            VncSend(sock, reason, (int)strlen(reason));
        }

        return authOk;
    }

    void HandleSetPixelFormat(SOCKET sock) {
        char buf[19];  // 3 padding + 16 pixel format bytes
        if (!VncRecvAll(sock, buf, 19)) return;
        std::cout << "[VNC] SetPixelFormat received (keeping server format)" << std::endl;
    }

    void HandleCutText(SOCKET sock) {
        char padding[3];
        UINT32 textLenNet;
        if (!VncRecvAll(sock, padding, 3)) return;
        if (!VncRecvAll(sock, (char*)&textLenNet, 4)) return;
        UINT32 textLen = ntohl(textLenNet);
        // Consume the clipboard text (ignore for now to keep client connected)
        if (textLen > 0 && textLen < 1024 * 1024) {
            std::vector<char> text(textLen);
            VncRecvAll(sock, text.data(), (int)textLen);
        } else if (textLen >= 1024 * 1024) {
            // Oversized clipboard text — consume in chunks to stay in sync
            char chunk[4096];
            UINT32 remaining = textLen;
            while (remaining > 0) {
                int n = (int)std::min((UINT32)sizeof(chunk), remaining);
                if (!VncRecvAll(sock, chunk, n)) return;
                remaining -= n;
            }
        }
        std::cout << "[VNC] ClientCutText: " << textLen << " bytes (discarded)" << std::endl;
    }

    void HandleSetEncodings(SOCKET sock, std::vector<INT32>& outEncodings) {
        char padding;
        if (!VncRecvAll(sock, &padding, 1)) return;

        UINT16 numEncodingsNet;
        if (!VncRecvAll(sock, (char*)&numEncodingsNet, 2)) return;
        UINT16 numEncodings = ntohs(numEncodingsNet);

        // CRITICAL: consume ALL encoding entries to keep protocol in sync
        // Previously only consumed min(numEncodings, 10), causing protocol desync
        outEncodings.clear();
        outEncodings.reserve(numEncodings);
        for (int i = 0; i < (int)numEncodings; i++) {
            INT32 enc;
            if (!VncRecvAll(sock, (char*)&enc, 4)) return;
            outEncodings.push_back(ntohl(enc));
        }

        // Log negotiated encodings
        std::cout << "[VNC] Client offered " << numEncodings << " encodings:";
        for (auto e : outEncodings) {
            std::cout << " " << e;
        }
        std::cout << std::endl;
    }

    void HandleUpdateRequest(SOCKET sock, const std::vector<INT32>& encodings) {
        char incremental;
        UINT16 x, y, w, h;
        if (!VncRecvAll(sock, &incremental, 1)) return;
        if (!VncRecvAll(sock, (char*)&x, 2)) return;
        if (!VncRecvAll(sock, (char*)&y, 2)) return;
        if (!VncRecvAll(sock, (char*)&w, 2)) return;
        if (!VncRecvAll(sock, (char*)&h, 2)) return;

        x = ntohs(x); y = ntohs(y);
        w = ntohs(w); h = ntohs(h);

        // Clamp to framebuffer bounds to prevent overflow
        if (x >= (UINT16)framebufferWidth_) x = (UINT16)(framebufferWidth_ - 1);
        if (y >= (UINT16)framebufferHeight_) y = (UINT16)(framebufferHeight_ - 1);
        if ((int)(x + w) > framebufferWidth_) w = (UINT16)(framebufferWidth_ - x);
        if ((int)(y + h) > framebufferHeight_) h = (UINT16)(framebufferHeight_ - y);

        size_t dataSize = (size_t)w * h * 4;
        if (dataSize == 0) return;

        // Throttle frame rate based on adaptive quality tier
        adaptiveQuality_.CheckSystemLoad();
        const QualitySettings& qs = adaptiveQuality_.GetSettings();
        int intervalMs = adaptiveQuality_.GetFrameIntervalMs();
        (void)intervalMs;  // Caller is responsible for sleep between requests

        // Clamp frame size to quality limit
        if (dataSize > (size_t)qs.maxFrameSizeBytes) {
            std::cerr << "[VNC] Frame too large (" << dataSize << " > " << qs.maxFrameSizeBytes
                      << "), dropping (tier=" << AdaptiveQuality::TierName(adaptiveQuality_.GetTier())
                      << ")" << std::endl;
            adaptiveQuality_.ReportDroppedFrame();
            return;
        }

        // Pick best encoding: prefer Hextile (5) over Raw (0)
        bool useHextile = false;
        for (INT32 enc : encodings) {
            if (enc == RFB::EncodingHextile) { useHextile = true; break; }
        }

        auto frameStart = std::chrono::high_resolution_clock::now();

        // Send framebuffer update header
        char header[4] = { 0, 0, 0, 1 };  // type=0, padding=0, nRects=1
        VncSend(sock, header, 4);

        // Rectangle header
        UINT16 rectX = htons(x);
        UINT16 rectY = htons(y);
        UINT16 rectW = htons(w);
        UINT16 rectH = htons(h);
        INT32 encoding = htonl(useHextile ? RFB::EncodingHextile : RFB::EncodingRaw);

        VncSend(sock, (char*)&rectX, 2);
        VncSend(sock, (char*)&rectY, 2);
        VncSend(sock, (char*)&rectW, 2);
        VncSend(sock, (char*)&rectH, 2);
        VncSend(sock, (char*)&encoding, 4);

        {
            std::lock_guard<std::mutex> lock(framebufferMutex_);
            if (useHextile) {
                SendHextileData(sock, x, y, w, h);
            } else {
                // Raw encoding
                size_t srcOffset = ((size_t)y * framebufferWidth_ + x) * 4;
                if (srcOffset + dataSize <= framebuffer_.size()) {
                    VncSend(sock, framebuffer_.data() + srcOffset, (int)dataSize);
                } else {
                    std::vector<char> black(dataSize, 0);
                    VncSend(sock, black.data(), (int)dataSize);
                }
            }
        }

        // Report latency for adaptive quality decision
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::high_resolution_clock::now() - frameStart).count();
        adaptiveQuality_.ReportFrameLatency((int)elapsed);
        if (elapsed > 100) {
            std::cerr << "[VNC] Slow frame send: " << elapsed << "ms for "
                      << w << "x" << h << " rect (tier="
                      << AdaptiveQuality::TierName(adaptiveQuality_.GetTier()) << ")" << std::endl;
        }
    }

    void HandleKeyEvent(SOCKET sock) {
        char down;
        char padding[2];
        UINT32 keysym;
        if (!VncRecvAll(sock, &down, 1)) return;
        if (!VncRecvAll(sock, padding, 2)) return;
        if (!VncRecvAll(sock, (char*)&keysym, 4)) return;
        keysym = ntohl(keysym);

        UINT32 vk = KeySymMapper::X11ToWindows(keysym);
        if (vk == 0) {
            std::cout << "[VNC] KeyEvent: unmapped keysym=0x" << std::hex << keysym
                      << std::dec << " (ignoring)" << std::endl;
            return;
        }

        // Inject via DriverInterface
        bool ok = false;
        if (driverInterface_) {
            if (down) {
                ok = driverInterface_->InjectKeyDown((UCHAR)vk, 0);
            } else {
                ok = driverInterface_->InjectKeyUp((UCHAR)vk, 0);
            }
        }
        std::cout << "[VNC] KeyEvent: keysym=0x" << std::hex << keysym
                  << " vk=0x" << vk << std::dec
                  << (down ? " DOWN" : " UP")
                  << (ok ? " OK" : " FAIL") << std::endl;
    }

    void HandlePointerEvent(SOCKET sock, ClientInputState& state) {
        char buttonMaskByte;
        UINT16 x, y;
        if (!VncRecvAll(sock, &buttonMaskByte, 1)) return;
        if (!VncRecvAll(sock, (char*)&x, 2)) return;
        if (!VncRecvAll(sock, (char*)&y, 2)) return;
        x = ntohs(x); y = ntohs(y);
        UCHAR buttonMask = (UCHAR)buttonMaskByte;

        if (!driverInterface_) return;

        // Always send absolute mouse move
        driverInterface_->InjectMouseMove((LONG)x, (LONG)y, true);

        // Handle scroll wheel pseudo-buttons (bits 3 and 4)
        if (buttonMask & 0x08) {
            driverInterface_->InjectMouseScroll(WHEEL_DELTA, 0);   // scroll up
        }
        if (buttonMask & 0x10) {
            driverInterface_->InjectMouseScroll(-WHEEL_DELTA, 0);  // scroll down
        }
        if (buttonMask & 0x20) {
            driverInterface_->InjectMouseScroll(0, -WHEEL_DELTA);  // scroll left
        }
        if (buttonMask & 0x40) {
            driverInterface_->InjectMouseScroll(0, WHEEL_DELTA);   // scroll right
        }

        // Detect transitions on real buttons (bits 0-2)
        // RFB bit 0=Left, 1=Middle, 2=Right
        static const struct { UCHAR bit; UCHAR vkBtn; } BTNS[] = {
            { 0x01, VMOUSE_BUTTON_LEFT   },
            { 0x02, VMOUSE_BUTTON_MIDDLE },
            { 0x04, VMOUSE_BUTTON_RIGHT  },
        };
        for (const auto& b : BTNS) {
            bool wasDown = (state.lastButtonMask & b.bit) != 0;
            bool isDown  = (buttonMask & b.bit) != 0;
            if (isDown && !wasDown) {
                driverInterface_->InjectMouseButton(b.vkBtn, true);
            } else if (!isDown && wasDown) {
                driverInterface_->InjectMouseButton(b.vkBtn, false);
            }
        }

        state.lastButtonMask = buttonMask & 0x07;  // only real buttons
        state.lastX = x;
        state.lastY = y;
    }
};

// Public API
VNCServer::VNCServer() : impl_(new VncServerImpl()) {}
VNCServer::~VNCServer() { delete impl_; }
bool VNCServer::Start() { return impl_->Start(); }
void VNCServer::Stop() { impl_->Stop(); }

void VNCServer::SetPassword(const std::string& password) {
    impl_->SetPassword(password);
}

void VNCServer::SetTlsEnabled(bool enabled) {
    impl_->SetTlsEnabled(enabled);
}

} // namespace Remote
} // namespace KVMDrivers
