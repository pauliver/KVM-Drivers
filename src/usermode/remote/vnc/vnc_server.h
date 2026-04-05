// vnc_server.h - VNC Server implementation (RFB 3.8 protocol)
// Supports RealVNC, TightVNC, TigerVNC, UltraVNC clients

#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winsock2.h>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <functional>

namespace KVMDrivers {
namespace Remote {

// RFB Protocol constants
namespace RFB {
    constexpr int ProtocolVersionMajor = 3;
    constexpr int ProtocolVersionMinor = 8;
    
    // Security types
    enum SecurityType {
        SecTypeInvalid = 0,
        SecTypeNone = 1,
        SecTypeVNCAuth = 2,
        SecTypeMSLogon = 113,  // UltraVNC MS-Logon
        SecTypeAnonTLS = 18,   // Anonymous TLS
        SecTypeVencrypt = 19,  // VeNCrypt (TLS + auth)
    };
    
    // Client to server messages
    enum ClientMsgType {
        ClientSetPixelFormat = 0,
        ClientSetEncodings = 2,
        ClientFramebufferUpdateRequest = 3,
        ClientKeyEvent = 4,
        ClientPointerEvent = 5,
        ClientCutText = 6,
    };
    
    // Server to client messages
    enum ServerMsgType {
        ServerFramebufferUpdate = 0,
        ServerSetColorMapEntries = 1,
        ServerBell = 2,
        ServerCutText = 3,
    };
    
    // Encodings
    enum Encoding {
        EncodingRaw = 0,
        EncodingCopyRect = 1,
        EncodingRRE = 2,
        EncodingCoRRE = 4,
        EncodingHextile = 5,
        EncodingZlib = 6,
        EncodingTight = 7,
        EncodingZRLE = 16,
        EncodingXCursor = -239,
        EncodingRichCursor = -240,
        EncodingDesktopSize = -223,
        EncodingExtendedDesktopSize = -308,
    };
    
    // Pixel format
    struct PixelFormat {
        UINT8 bitsPerPixel;
        UINT8 depth;
        UINT8 bigEndian;
        UINT8 trueColor;
        UINT16 redMax;
        UINT16 greenMax;
        UINT16 blueMax;
        UINT8 redShift;
        UINT8 greenShift;
        UINT8 blueShift;
        UINT8 padding[3];
    };
}

// VNC Server configuration
struct VNCServerConfig {
    int port = 5900;                    // Default VNC port
    int maxClients = 4;                 // Max concurrent connections
    bool requirePassword = true;          // Require VNC auth
    std::string password;                 // VNC password (max 8 chars)
    bool enableAnonTLS = false;           // Anonymous TLS wrapper
    bool enableMSLogon = false;           // UltraVNC MS-Logon
    std::string msLogonUsername;
    std::string msLogonDomain;
    
    // Framebuffer settings
    int framebufferWidth = 1920;
    int framebufferHeight = 1080;
    int bitsPerPixel = 32;
    
    // Performance
    int preferredEncoding = RFB::EncodingTight;
    int compressionLevel = 6;           // 0-9 for zlib/tight
    int jpegQuality = 8;                // 0-9 for tight encoding
    bool useCopyRect = true;
    bool sharedDesktop = true;            // Allow shared connections
};

// Client connection info
struct VNCClientInfo {
    SOCKET socket;
    std::string address;
    int port;
    bool authenticated;
    RFB::PixelFormat pixelFormat;
    std::vector<int> encodings;
    bool supportsCursorEncoding;
    bool supportsDesktopResize;
    
    // Viewport
    int x = 0, y = 0;
    int width = 0, height = 0;
};

// Framebuffer update rectangle
struct FBUUpdateRect {
    UINT16 x, y, width, height;
    int encodingType;
    std::vector<UINT8> data;
};

// Input event callbacks
using KeyEventCallback = std::function<void(UINT32 keysym, bool down, VNCClientInfo& client)>;
using PointerEventCallback = std::function<void(UINT16 x, UINT16 y, UINT8 buttonMask, VNCClientInfo& client)>;
using CutTextCallback = std::function<void(const std::string& text, VNCClientInfo& client)>;
using ClientConnectedCallback = std::function<void(VNCClientInfo& client)>;
using ClientDisconnectedCallback = std::function<void(VNCClientInfo& client)>;

class VncServerImpl;  // forward declaration — full definition in vnc_server.cpp

// VNC Server class
class VNCServer {
public:
    // maxClients: runtime connection cap (default 10). Pass AppSettings.VncMaxClients.
    explicit VNCServer(int maxClients = 10);
    ~VNCServer();
    
    // Lifecycle
    bool Initialize(const VNCServerConfig& config);
    void Shutdown();
    bool Start();
    void Stop();
    bool IsRunning() const;

    // Authentication (call before Start())
    void SetPassword(const std::string& password);
    void SetTlsEnabled(bool enabled);  // Enable AnonTLS (sec type 18, Schannel)
    
    // Framebuffer management
    void UpdateFramebuffer(const void* pixelData, int width, int height, int pitch);
    void RequestFramebufferUpdate(int x, int y, int width, int height);
    void SetFramebufferSize(int width, int height);
    
    // Cursor management
    void SetCursorImage(const void* imageData, int width, int height, 
        int hotspotX, int hotspotY);
    void SetCursorPosition(int x, int y);
    void SetCursorVisible(bool visible);
    
    // Desktop name
    void SetDesktopName(const std::string& name);
    
    // Event callbacks
    void SetKeyEventCallback(KeyEventCallback callback);
    void SetPointerEventCallback(PointerEventCallback callback);
    void SetCutTextCallback(CutTextCallback callback);
    void SetClientConnectedCallback(ClientConnectedCallback callback);
    void SetClientDisconnectedCallback(ClientDisconnectedCallback callback);
    
    // Client management
    int GetClientCount() const;
    void DisconnectClient(int clientIndex);
    void DisconnectAllClients();
    
    // Statistics
    int GetTotalBytesSent() const;
    int GetTotalBytesReceived() const;
    int GetFrameUpdatesSent() const;
    double GetAverageFrameTime() const;

private:
    VncServerImpl* impl_;
};

// X11 keysym to Windows VK code mapping
class KeySymMapper {
public:
    static UINT32 X11ToWindows(UINT32 keysym);
    static UINT32 WindowsToX11(UINT32 vk);
};

// Helper functions
namespace VNCUtils {
    // Start VNC server with default settings
    std::unique_ptr<VNCServer> StartDefaultServer(int port = 5900, 
        const std::string& password = "");
    
    // Check if a VNC client is compatible
    bool IsCompatibleClient(const std::string& clientName);
    
    // Get encoding name
    std::string GetEncodingName(int encoding);
    
    // Create VNC password file (for UltraVNC)
    bool CreatePasswordFile(const std::string& filepath, const std::string& password);
}

} // namespace Remote
} // namespace KVMDrivers
