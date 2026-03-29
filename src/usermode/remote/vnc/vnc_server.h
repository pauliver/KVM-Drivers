// vnc_server.h - VNC Server implementation (RFB 3.8 protocol)
// Supports RealVNC, TightVNC, TigerVNC, UltraVNC clients

#pragma once

#include <windows.h>
#include <winsock2.h>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <functional>
#include <openssl/des.h>

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

// VNC Server class
class VNCServer {
public:
    VNCServer();
    ~VNCServer();
    
    // Lifecycle
    bool Initialize(const VNCServerConfig& config);
    void Shutdown();
    bool Start();
    void Stop();
    bool IsRunning() const;
    
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
    VNCServerConfig config_;
    bool running_;
    bool initialized_;
    
    // Network
    SOCKET listenSocket_;
    std::thread acceptThread_;
    std::vector<std::thread> clientThreads_;
    std::vector<std::unique_ptr<VNCClientInfo>> clients_;
    std::mutex clientsMutex_;
    
    // Framebuffer
    std::vector<UINT8> framebuffer_;
    int fbWidth_;
    int fbHeight_;
    int fbPitch_;
    std::mutex fbMutex_;
    
    // Cursor
    std::vector<UINT8> cursorImage_;
    int cursorWidth_;
    int cursorHeight_;
    int cursorHotspotX_;
    int cursorHotspotY_;
    int cursorX_;
    int cursorY_;
    bool cursorVisible_;
    std::mutex cursorMutex_;
    
    // Desktop name
    std::string desktopName_;
    
    // Callbacks
    KeyEventCallback onKeyEvent_;
    PointerEventCallback onPointerEvent_;
    CutTextCallback onCutText_;
    ClientConnectedCallback onClientConnected_;
    ClientDisconnectedCallback onClientDisconnected_;
    
    // Statistics
    std::atomic<int> totalBytesSent_;
    std::atomic<int> totalBytesReceived_;
    std::atomic<int> frameUpdatesSent_;
    std::atomic<double> totalFrameTime_;
    
    // Methods
    bool CreateListenSocket();
    void AcceptLoop();
    void ClientLoop(VNCClientInfo* client);
    void RemoveClient(VNCClientInfo* client);
    
    // Protocol handlers
    bool HandleProtocolVersion(VNCClientInfo* client);
    bool HandleSecurity(VNCClientInfo* client);
    bool HandleVNCAuth(VNCClientInfo* client);
    bool HandleMSLogon(VNCClientInfo* client);
    bool HandleClientInit(VNCClientInfo* client);
    bool HandleServerInit(VNCClientInfo* client);
    
    // Message handlers
    bool HandleSetPixelFormat(VNCClientInfo* client);
    bool HandleSetEncodings(VNCClientInfo* client);
    bool HandleFramebufferUpdateRequest(VNCClientInfo* client);
    bool HandleKeyEvent(VNCClientInfo* client);
    bool HandlePointerEvent(VNCClientInfo* client);
    bool HandleCutText(VNCClientInfo* client);
    
    // Encoding
    void SendFramebufferUpdate(VNCClientInfo* client, int x, int y, int w, int h);
    void SendRawRect(VNCClientInfo* client, int x, int y, int w, int h);
    void SendRRERect(VNCClientInfo* client, int x, int y, int w, int h);
    void SendHextileRect(VNCClientInfo* client, int x, int y, int w, int h);
    void SendZlibRect(VNCClientInfo* client, int x, int y, int w, int h);
    void SendTightRect(VNCClientInfo* client, int x, int y, int w, int h);
    void SendZRLE(VNCClientInfo* client, int x, int y, int w, int h);
    void SendCursorUpdate(VNCClientInfo* client);
    void SendDesktopSizeUpdate(VNCClientInfo* client);
    
    // Helpers
    bool SendExact(SOCKET socket, const void* data, int len);
    bool RecvExact(SOCKET socket, void* data, int len);
    void WriteUInt8(std::vector<UINT8>& buf, UINT8 val);
    void WriteUInt16(std::vector<UINT8>& buf, UINT16 val);
    void WriteUInt32(std::vector<UINT8>& buf, UINT32 val);
    void WritePixelFormat(std::vector<UINT8>& buf, const RFB::PixelFormat& pf);
    
    // DES encryption for VNC auth
    void EncryptVNCChallenge(const UINT8 challenge[16], const char* password, 
        UINT8 response[16]);
    void DesKey(const char* password, DES_key_schedule* schedule);
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
