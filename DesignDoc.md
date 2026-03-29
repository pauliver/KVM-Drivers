# KVM-Drivers Design Document

## Table of Contents
1. [Architecture Overview](#architecture-overview)
2. [Driver Architecture](#driver-architecture)
3. [System Tray Application](#system-tray-application)
4. [Remote Management Protocol](#remote-management-protocol)
5. [Automated Testing Framework](#automated-testing-framework)
6. [Communication Architecture](#communication-architecture)
7. [Security Model](#security-model)
8. [Error Handling & Monitoring](#error-handling--monitoring)

---

## Architecture Overview

### High-Level System Diagram

```
┌─────────────────────────────────────────────────────────────────────────┐
│                           REMOTE / AUTOMATION CLIENTS                    │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  │
│  │   Remote     │  │   Remote     │  │    CI/CD     │  │   Manual     │  │
│  │   Desktop    │  │   Web UI     │  │   Pipeline   │  │   Scripts    │  │
│  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘  │
└─────────┼────────────────┼──────────────────┼─────────────────┼─────────┘
          │                │                  │                 │
          └────────────────┴──────────┬───────┴─────────────────┘
                                      │ KVM Protocol (TLS/WebSocket)
                                      ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                    PILOTED SYSTEM (Windows Host)                         │
│  ┌─────────────────────────────────────────────────────────────────┐   │
│  │                    KVM Tray Application                          │   │
│  │  ┌────────────┐  ┌────────────┐  ┌────────────┐  ┌────────────┐  │   │
│  │  │  Driver    │  │ Connection │  │   Logger   │  │ Automation │  │   │
│  │  │  Manager   │  │  Manager   │  │  Service   │  │  Engine    │  │   │
│  │  └─────┬──────┘  └─────┬──────┘  └─────┬──────┘  └─────┬──────┘  │   │
│  │        └────────────────┴───────────────┴───────────────┘        │   │
│  └──────────────────────────────────────────────────────────────────┘   │
│                              │                                         │
│  ┌───────────────────────────┼──────────────────────────────────────┐ │
│  │         Core Service      │ User-Mode Service (KVMService.exe)    │ │
│  │  ┌──────────────────────┐ │  ┌────────────────────────────────┐   │ │
│  │  │  Device I/O Router  │◄┼──┤  Protocol Handler (JSON-RPC)  │   │ │
│  │  │  (Input Dispatch)   │ │  │  - Remote commands             │   │ │
│  │  └──────────────────────┘ │  │  - Automation sequences          │   │ │
│  │                         │  └────────────────────────────────┘   │ │
│  └─────────────────────────┴──────────────────────────────────────┘ │
│                              │                                         │
│  ┌───────────────────────────┼──────────────────────────────────────┐ │
│  │       DRIVER LAYER        │ Kernel-Mode Drivers                   │ │
│  │  ┌──────────┐ ┌──────────┐│┌──────────┐ ┌──────────────────────┐  │ │
│  │  │ V-HID    │ │ V-HID    │││ V-XInput │ │    V-Display         │  │ │
│  │  │ Keyboard│ │ Mouse    │││ Controller│ │  (IDD/Indirect)     │  │ │
│  │  │ (Filter)│ │ (Filter) │││ (Filter) │ │   Display Driver     │  │ │
│  │  └────┬─────┘ └────┬─────┘│└────┬─────┘ └──────────┬───────────┘  │ │
│  │       └────────────┴──────┘─────┴──────────────────┘              │ │
│  │                         │                                         │ │
│  │                    Windows HID Stack                               │ │
│  │  ┌──────────────────────┴──────────────────────────────────────┐  │ │
│  │  │              HID Class Driver (hidclass.sys)                │  │ │
│  │  │           USB HID Minidrivers (usbhid, kbdhid, mouhid)      │  │ │
│  │  │                      Win32k.sys                           │  │ │
│  │  └─────────────────────────────────────────────────────────────┘  │ │
│  └──────────────────────────────────────────────────────────────────┘ │
└───────────────────────────────────────────────────────────────────────┘
```

---

## Driver Architecture

### 1. Virtual HID Keyboard Driver (vhidkb.sys)

**Purpose**: Emulate physical keyboard input at the driver level

**Implementation Approach**:
- **Type**: Upper filter driver on the HID keyboard stack
- **Technology**: WDF (Windows Driver Framework) Kernel-Mode Driver
- **HID Report Descriptor**: Standard boot keyboard (8-byte reports)

**Key Components**:
```c
// Driver IOCTL interface for user-mode communication
#define IOCTL_VKB_INJECT_KEYDOWN    CTL_CODE(FILE_DEVICE_KEYBOARD, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VKB_INJECT_KEYUP      CTL_CODE(FILE_DEVICE_KEYBOARD, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VKB_INJECT_COMBO      CTL_CODE(FILE_DEVICE_KEYBOARD, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VKB_RESET             CTL_CODE(FILE_DEVICE_KEYBOARD, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)

// Input report structure (compatible with boot keyboard)
typedef struct _VKB_INPUT_REPORT {
    UCHAR  ReportID;
    UCHAR  ModifierKeys;    // Ctrl, Alt, Shift, Win, etc.
    UCHAR  Reserved;
    UCHAR  KeyCodes[6];     // Up to 6 simultaneous keys
} VKB_INPUT_REPORT, *PVKB_INPUT_REPORT;
```

**Hardware Indistinguishability**:
- Uses standard HID report format matching physical keyboards
- Proper PnP device IDs (VID/PID matching generic HID keyboards)
- Supports hot-plug events like physical devices
- Integrates with Windows biometric/secure attention key (SAS) handling

**Special Features**:
- NKRO (N-Key Rollover) mode for unlimited simultaneous keys
- Scancode mapping for international layouts
- Secure Attention Sequence (Ctrl+Alt+Del) handling

### 2. Virtual HID Mouse Driver (vhidmouse.sys)

**Purpose**: Emulate physical mouse input at the driver level

**Implementation Approach**:
- **Type**: Upper filter driver on the HID mouse stack
- **Technology**: WDF Kernel-Mode Driver
- **HID Report Descriptor**: Standard boot mouse (3-4 byte reports)

**Key Components**:
```c
#define IOCTL_VMOUSE_INJECT_MOVE     CTL_CODE(FILE_DEVICE_MOUSE, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VMOUSE_INJECT_BUTTON   CTL_CODE(FILE_DEVICE_MOUSE, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VMOUSE_INJECT_SCROLL   CTL_CODE(FILE_DEVICE_MOUSE, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)

// Mouse input report
typedef struct _VMOUSE_INPUT_REPORT {
    UCHAR  ReportID;
    UCHAR  Buttons;         // Bitmask for buttons 1-5
    SHORT  X;               // Relative X movement (-32768 to 32767)
    SHORT  Y;               // Relative Y movement (-32768 to 32767)
    SHORT  Wheel;           // Vertical scroll
    SHORT  HWheel;          // Horizontal scroll (optional)
} VMOUSE_INPUT_REPORT, *PVMOUSE_INPUT_REPORT;
```

**Hardware Indistinguishability**:
- Generates identical mouse packets to physical HID mice
- Supports high-resolution (HID 2.0) mouse reports
- Proper cursor acceleration integration (delegates to Windows)

**Special Features**:
- Absolute positioning mode (for remote desktop scenarios)
- High-DPI support (configurable DPI scaling)
- Precision scroll wheel support

### 3. Virtual Xbox Controller Driver (vxinput.sys)

**Purpose**: Emulate Xbox 360/One controller for game/application compatibility

**Implementation Approach**:
- **Type**: Virtual bus driver with child HID devices
- **Technology**: WDF Kernel-Mode Driver + User-mode component
- **Method**: Uses ViGEmBus-compatible approach or official Xbox driver protocol

**Architecture Options**:

**Option A: ViGEmBus Approach (Recommended)**
- Compatible with existing ViGEmBus client libraries
- Well-tested with games and applications
- Existing ecosystem support

**Option B: Custom HID Approach**
- Direct HID report generation matching Xbox 360 descriptor
- Custom bus enumerator for device creation

**Key Components**:
```c
#define IOCTL_VXINPUT_CREATE_DEVICE  CTL_CODE(FILE_DEVICE_BUS_EXTENDER, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VXINPUT_DESTROY_DEVICE CTL_CODE(FILE_DEVICE_BUS_EXTENDER, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VXINPUT_REPORT         CTL_CODE(FILE_DEVICE_BUS_EXTENDER, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)

// Xbox 360 compatible input report
typedef struct _XUSB_REPORT {
    USHORT wButtons;        // Digital buttons (ABXY, D-Pad, triggers, etc.)
    BYTE   bLeftTrigger;    // Left analog trigger (0-255)
    BYTE   bRightTrigger;   // Right analog trigger (0-255)
    SHORT  sThumbLX;        // Left stick X (-32768 to 32767)
    SHORT  sThumbLY;        // Left stick Y (-32768 to 32767)
    SHORT  sThumbRX;        // Right stick X (-32768 to 32767)
    SHORT  sThumbRY;        // Right stick Y (-32768 to 32767)
} XUSB_REPORT, *PXUSB_REPORT;
```

**Hardware Indistinguishability**:
- Exact USB VID/PID match (Microsoft Xbox 360 Controller)
- Identical HID report descriptor
- XInput API compatibility (games see no difference)
- Haptic feedback (rumble) support

### 4. Virtual Display Driver (vdisplay.sys)

**Purpose**: Provide virtual display output and capture for remote viewing

**Implementation Approach**:
- **Type**: Indirect Display Driver (IDD) - Windows 10 1607+
- **Technology**: User-mode driver framework (UMDF 2.0)
- **Alternative**: IddSampleDriver-based approach

**Key Components**:
```c
// IDD callback interface implementation
// Key callbacks:
// - ParseEdid(): Report monitor capabilities
// - SetSwapChain(): Receive GPU frames
// - ReportFrameStatistics(): Timing information

// Frame capture and encoding
#define IOCTL_VDISP_GET_FRAMEBUFFER  CTL_CODE(FILE_DEVICE_DISPLAY, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VDISP_SET_MODE         CTL_CODE(FILE_DEVICE_DISPLAY, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)

// Frame buffer access
typedef struct _VDISP_FRAMEBUFFER_INFO {
    UINT   Width;
    UINT   Height;
    UINT   Stride;
    DXGI_FORMAT Format;
    HANDLE SharedTextureHandle;  // GPU shared handle for zero-copy
} VDISP_FRAMEBUFFER_INFO, *PVDISP_FRAMEBUFFER_INFO;
```

**Hardware Indistinguishability**:
- Standard EDID with generic monitor identifier
- Multiple supported resolutions and refresh rates
- HDR support (if requested)
- G-Sync/FreeSync compatibility reports

**Frame Capture Strategy**:
- GPU texture sharing for minimal overhead
- Hardware-accelerated encoding (NVENC, QuickSync, AMF)
- Configurable quality/latency tradeoffs

**Hardware-Accelerated Video Encoding**:

The system must detect available CPU/GPU video encoding capabilities at runtime and select the optimal encoder for remote streaming scenarios. This requires custom implementation paths for each vendor:

```c
// Video encoder capability detection
typedef enum _VIDEO_ENCODER_TYPE {
    Encoder_None,           // Fallback to CPU encoding (libx264)
    Encoder_Intel_QSV,      // Intel QuickSync Video (2nd Gen Core+)
    Encoder_AMD_AMF,        // AMD Advanced Media Framework (Radeon 200+)
    Encoder_Nvidia_NVENC    // NVIDIA NVENC (Kepler/Maxwell/Pascal+)
} VIDEO_ENCODER_TYPE;

typedef struct _ENCODER_CAPS {
    VIDEO_ENCODER_TYPE Type;
    WCHAR AdapterName[256];
    UINT AdapterIndex;
    BOOL Supports_H264;
    BOOL Supports_H265;
    BOOL Supports_AV1;      // Intel Arc, AMD RX 7000, NVIDIA Ada
    UINT MaxResolution;     // e.g., 4096x4096
    UINT MaxBitrate;        // Mbps
    BOOL Supports_LowLatency;
    BOOL Supports_SpatialAQ; // Adaptive quantization
    BOOL Supports_TemporalAQ;
} ENCODER_CAPS, *PENCODER_CAPS;

// Runtime encoder selection
ENCODER_CAPS DetectBestEncoder(IDXGIAdapter* pTargetAdapter);
HRESULT InitializeEncoder(const ENCODER_CAPS* pCaps, const ENCODER_CONFIG* pConfig);
```

**Vendor-Specific Implementation Paths**:

| Vendor | API/SDK | Key Features | Detection Method |
|--------|---------|--------------|------------------|
| **Intel** | Intel Media SDK / oneVPL | QuickSync, Low-Power HEVC, AV1 (Arc) | Check `igd10umd64.dll` presence, Query MFXVersion |
| **AMD** | AMF (Advanced Media Framework) | H264, H265, AV1 (RX 7000), Pre-analysis | Load `amfrt64.dll`, Query AMFDeviceCaps |
| **NVIDIA** | NVENC via Video Codec SDK | H264, H265, AV1 (Ada), Multi-encode | Check `nvEncodeAPI64.dll`, Query NVENC caps via `NvEncodeAPIGetMaxSupportedVersion` |

**Intel QuickSync (oneVPL) Implementation**:
```c
// Intel QuickSync Video encoding via oneVPL
#include <mfxvideo.h>

typedef struct _QSV_ENCODER {
    mfxSession Session;
    mfxVideoParam Params;
    mfxFrameSurface1* pInputSurface;
    mfxBitstream Bitstream;
} QSV_ENCODER;

// Key capabilities to detect:
// - MFX_VERSION: API version support
// - mfxIMPL: Hardware vs Software fallback
// - mfxExtCodingOption3: Low latency mode, adaptive I/B placement
// - mfxExtCodingOption2: Lookahead depth for quality
// - mfxExtHEVCParam: Tile support for parallel decode
// - mfxExtAV1Param: AV1 encoding (Arc GPUs)
```

**AMD AMF Implementation**:
```c
// AMD Advanced Media Framework encoding
#include <AMF/core/Factory.h>
#include <AMF/components/VideoEncoderVCE.h>

typedef struct _AMF_ENCODER {
    amf::AMFFactory* pFactory;
    amf::AMFContext* pContext;
    amf::AMFComponent* pEncoder;
    amf::AMF_SURFACE_FORMAT Format;
} AMF_ENCODER;

// Key capabilities to detect:
// - AMFCapsVulkan/AMFCapsDX11: GPU compute support
// - AMF_VIDEO_ENCODER_CAP_QUERY_TIMEOUT: Driver responsiveness
// - AMF_VIDEO_ENCODER_CAP_QUERY_PRE_ANALYSIS: Pre-analysis support
// - AMF_VIDEO_ENCODER_CAP_QUERY_MAX_BITRATE: Bandwidth limits
// - AMFVideoEncoder_AV1: AV1 support (RX 7000 series)
// - AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR/VBR: Rate control modes
```

**NVIDIA NVENC Implementation**:
```c
// NVIDIA NVENC via Video Codec SDK
#include <nvEncodeAPI.h>

typedef struct _NVENC_ENCODER {
    NV_ENCODE_API_FUNCTION_LIST NvEncFunctions;
    void* hEncoder;
    NV_ENC_INITIALIZE_PARAMS InitParams;
    NV_ENC_CONFIG EncConfig;
} NVENC_ENCODER;

// Key capabilities to detect:
// - NV_ENC_CAPS_MAX_WIDTH/MAX_HEIGHT: Resolution limits
// - NV_ENC_CAPS_SUPPORT_MULTIPLE_REF_FRAMES: Quality features
// - NV_ENC_CAPS_SUPPORT_TEMPORAL_AQ: Adaptive quantization
// - NV_ENC_CAPS_SUPPORT_LOOKAHEAD: Lookahead for quality
// - NV_ENC_CODEC_AV1_GUID: AV1 support (RTX 40 series)
// - NV_ENC_MULTI_ENCODE: Simultaneous encoding (RTX 30+)

// NVIDIA GPU architecture detection (for feature availability)
typedef enum _NV_ARCH {
    NV_Arch_Unknown,
    NV_Arch_Kepler,     // GTX 600/700 - Basic NVENC
    NV_Arch_Maxwell,    // GTX 900 - Improved NVENC
    NV_Arch_Pascal,     // GTX 10 - High quality NVENC
    NV_Arch_Turing,     // RTX 20 - Turing NVENC
    NV_Arch_Ampere,     // RTX 30 - Multi-encode
    NV_Arch_Ada         // RTX 40 - AV1, Multi-encode
} NV_ARCH;
```

**Encoder Selection Logic**:
```c
VIDEO_ENCODER SelectOptimalEncoder(
    const ENCODER_CAPS* pAvailableEncoders,
    UINT EncoderCount,
    const STREAM_REQUIREMENTS* pRequirements)
{
    // Priority for remote streaming:
    // 1. Hardware encoder on same GPU as display (zero-copy)
    // 2. AV1 if available and client supports it (best compression)
    // 3. HEVC/H265 if available (good compression, hardware decode common)
    // 4. H264 baseline (universal compatibility)
    
    for each encoder in pAvailableEncoders:
        if (encoder.AdapterIndex == pRequirements->DisplayAdapterIndex) {
            // Same GPU as display - preferred for zero-copy
            if (pRequirements->Codec == AV1 && encoder.Supports_AV1)
                return encoder;
            if (pRequirements->Codec == H265 && encoder.Supports_H265)
                return encoder;
            if (encoder.Supports_H264)
                return encoder; // Fallback
        }
    
    // No same-GPU encoder, use any available
    // ... fallback logic
}
```

**Performance Targets by Encoder**:

| Encoder | Target Latency | Max Bitrate | Use Case |
|---------|---------------|-------------|----------|
| NVENC (Ada) | 4-8ms | 50 Mbps | High-quality remote gaming |
| NVENC (Turing+) | 8-12ms | 30 Mbps | General remote desktop |
| QuickSync | 8-16ms | 20 Mbps | Laptop/integrated graphics |
| AMF | 8-16ms | 30 Mbps | AMD GPU systems |
| CPU (x264) | 20-40ms | 10 Mbps | Fallback, low power systems |

**Integration with IDD**:
```c
// IDD swap chain callback with hardware encoding
HRESULT STDMETHODCALLTYPE SetSwapChain(
    IDDCX_SWAPCHAIN SwapChain,
    ID3D11Device* pDevice,
    HANDLE hSharedSurface)
{
    // 1. Detect GPU from D3D11 device
    IDXGIDevice* pDXGIDevice;
    pDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&pDXGIDevice);
    
    IDXGIAdapter* pAdapter;
    pDXGIDevice->GetAdapter(&pAdapter);
    
    // 2. Query encoder capabilities
    ENCODER_CAPS caps = DetectBestEncoder(pAdapter);
    
    // 3. Initialize appropriate encoder
    if (caps.Type == Encoder_Nvidia_NVENC) {
        InitializeNVENCEncoder(pDevice, hSharedSurface, &caps);
    } else if (caps.Type == Encoder_Intel_QSV) {
        InitializeQSVEncoder(pDevice, hSharedSurface, &caps);
    } else if (caps.Type == Encoder_AMD_AMF) {
        InitializeAMFEncoder(pDevice, hSharedSurface, &caps);
    }
    
    // 4. Begin frame processing loop
    // Each new frame -> Encode -> Stream to remote client
}
```

**Dynamic Quality Adjustment**:
- Monitor network bandwidth and latency
- Adjust encoder bitrate/CQP dynamically
- Switch codecs on-the-fly if client supports it
- Fallback to software encoding if hardware encoder fails

---

## System Tray Application

### Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                      KVM Tray Application                        │
│                     (C# WPF or Win32/C++)                        │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │                   System Tray Icon                       │   │
│  │  - Status indicators (driver state, connections)          │   │
│  │  - Context menu for quick actions                       │   │
│  │  - Balloon notifications for events                     │   │
│  └─────────────────────────────────────────────────────────┘   │
│                            │                                     │
│  ┌─────────────────────────┴──────────────────────────────────┐  │
│  │                      Main Window                            │  │
│  │  ┌────────────┐  ┌────────────┐  ┌────────────┐           │  │
│  │  │  Drivers   │  │   Logs     │  │ Automation │  ┌──────┐ │  │
│  │  │   Tab      │  │    Tab     │  │    Tab     │  │Remote│ │  │
│  │  │            │  │            │  │            │  │  Tab │ │  │
│  │  │ ┌────────┐ │  │ ┌────────┐ │  │ ┌────────┐ │  └──────┘ │  │
│  │  │ │Keyboard│ │  │ │ Real-  │ │  │ │Test    │ │           │  │
│  │  │ │ Mouse  │ │  │ │ time   │ │  │ │Suite   │ │           │  │
│  │  │ │Controller│ │  │ │ Filter │ │  │ │Manager │ │           │  │
│  │  │ │ Display│ │  │ │ Search │ │  │ │Results │ │           │  │
│  │  │ │[Toggle]│ │  │ │ Export │ │  │ │[Run]   │ │           │  │
│  │  │ └────────┘ │  │ └────────┘ │  │ └────────┘ │           │  │
│  │  └────────────┘  └────────────┘  └────────────┘           │  │
│  └───────────────────────────────────────────────────────────┘  │
│                                                                  │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │                      Background Services                    │ │
│  │  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────┐ │ │
│  │  │ Log Monitor │  │ Error Watcher │  │ Connection Manager  │ │ │
│  │  │             │  │             │  │                     │ │ │
│  │  │ - ETW trace │  │ - Driver health│  │ - TLS server       │ │ │
│  │  │ - Kernel log│  │ - Watchdog   │  │ - Endpoint auth    │ │ │
│  │  │ - App events│  │ - Alerting    │  │ - Session mgmt     │ │ │
│  │  └─────────────┘  └─────────────┘  └─────────────────────┘ │ │
│  └────────────────────────────────────────────────────────────┘ │
│                                                                  │
└──────────────────────────────────────────────────────────────────┘
```

### Feature Specifications

#### Driver Control Panel
- **Individual Toggles**: Enable/disable each driver independently
- **Status Indicators**: Green/Yellow/Red for each driver state
  - Green: Active and healthy
  - Yellow: Starting/stopping or degraded
  - Red: Error state or disabled
- **Auto-start Options**: Launch drivers on boot, on network activity, etc.

#### Log & Diagnostics System
**Log Sources**:
- ETW (Event Tracing for Windows) traces from drivers
- Windows Event Log (System/Application)
- Driver-specific debug logs
- Application-level logs

**Features**:
- Real-time log streaming (Live tail)
- Severity filtering (Error/Warning/Info/Debug)
- Keyword/regex search
- Log export (CSV, JSON, raw)
- Automatic error detection with pattern matching
- Email/notification alerts for critical errors

**Error Monitoring**:
```csharp
// Error detection patterns
public class ErrorMonitor
{
    // Watch for driver crashes
    private const string DRIVER_CRASH_PATTERN = @"BugCheck|DRIVER_IRQL_NOT_LESS_OR_EQUAL";
    
    // Watch for device failures
    private const string DEVICE_FAILURE_PATTERN = @"Device not started|This device cannot start";
    
    // Watch for connection issues
    private const string CONNECTION_ERROR_PATTERN = @"TLS handshake failed|Connection reset";
    
    // Alert threshold: 3 errors of same type in 5 minutes
    public int AlertThreshold { get; set; } = 3;
    public TimeSpan AlertWindow { get; set; } = TimeSpan.FromMinutes(5);
}
```

#### Connection Manager
**Remote Endpoint Management**:
- View active connections with client info
- Manual approve/deny for incoming connections
- Configurable auto-accept policies (IP allowlists, certificate fingerprints)
- Connection quality metrics (latency, bandwidth, frame rate)
- Force disconnect capability

**Endpoint Sources**:
```csharp
public enum EndpointSource
{
    LocalAutomation,      // Scripts running locally
    RemoteDesktopClient,  // Interactive remote user
    WebInterface,         // Browser-based control
    CiCdPipeline,         // Automated test runner
    MobileApp,            // iOS/Android companion app
    ApiClient             // Third-party integrations
}
```

#### Automated Testing Integration
- Test profile management (create, edit, delete)
- Schedule test runs (one-time, recurring)
- View test results and history
- Export test reports
- Integration with popular test frameworks (Selenium, Playwright, etc.)

---

## Remote Management Protocol

### Protocol Stack

```
┌─────────────────────────────────────────────────────────┐
│                    Application Layer                      │
│              JSON-RPC 2.0 / MessagePack-RPC             │
├─────────────────────────────────────────────────────────┤
│                    Session Layer                        │
│           WebSocket (primary) / TCP fallback            │
├─────────────────────────────────────────────────────────┤
│                    Security Layer                       │
│      TLS 1.3 (mandatory) + Certificate pinning          │
├─────────────────────────────────────────────────────────┤
│                    Transport Layer                      │
│                TCP / UDP (QUIC future)                  │
└─────────────────────────────────────────────────────────┘
```

### Message Protocol

```json
// Example: Inject keyboard input
{
  "jsonrpc": "2.0",
  "method": "input.keyboard.inject",
  "params": {
    "keys": [
      {"key": "ctrl", "action": "down"},
      {"key": "c", "action": "press", "duration_ms": 50},
      {"key": "ctrl", "action": "up"}
    ],
    "async": false
  },
  "id": 1
}

// Example: Mouse move
{
  "jsonrpc": "2.0",
  "method": "input.mouse.move",
  "params": {
    "mode": "relative",
    "x": 100,
    "y": -50,
    "duration_ms": 200
  },
  "id": 2
}

// Example: Get display frame
{
  "jsonrpc": "2.0",
  "method": "display.capture",
  "params": {
    "format": "jpeg",
    "quality": 85,
    "region": {"x": 0, "y": 0, "width": 1920, "height": 1080}
  },
  "id": 3
}
```

### Connection Flow

```
┌──────────┐                           ┌──────────┐
│  Client  │                           │  Server  │
│          │                           │  (KVM)   │
└────┬─────┘                           └────┬─────┘
     │                                      │
     │  1. TCP connect                      │
     ├─────────────────────────────────────>│
     │                                      │
     │  2. TLS handshake + cert validation  │
     │<====================================>│
     │                                      │
     │  3. WebSocket upgrade                │
     ├─────────────────────────────────────>│
     │                                      │
     │  4. Authentication (token/cert)     │
     ├─────────────────────────────────────>│
     │<─────────────────────────────────────│
     │      Auth result + session token      │
     │                                      │
     │  5. Request capabilities            │
     ├─────────────────────────────────────>│
     │<─────────────────────────────────────│
     │       Capabilities response          │
     │       (resolutions, features, etc.)  │
     │                                      │
     │  6. [Bidirectional data flow]       │
     │<────────────────────────────────────>│
     │     Input commands / Video frames    │
     │                                      │
```

### VNC Protocol Compatibility

**Goal**: Support standard VNC clients (RealVNC, TightVNC, TigerVNC, UltraVNC, etc.) for maximum compatibility with existing remote desktop tools.

**Implementation Approach**:
- **Protocol Handler**: Separate VNC server implementation alongside the native KVM protocol
- **Protocol Version**: Support RFB 3.8 with authentication extensions
- **Security**: VNC authentication + optional TLS wrapper (AnonTLS)
- **Default Port**: 5900 + display number (configurable)

**Supported Encodings**:
| Encoding | Priority | Notes |
|----------|----------|-------|
| Raw | Fallback | Always supported |
| RRE | Low | Basic compression |
| Hextile | Medium | Good for low-bandwidth |
| ZRLE | High | zlib compression |
| Tight | High | Best compression |
| H.264 | Experimental | Via FFmpeg integration |
| Cursor pseudo-encoding | Yes | Local cursor rendering |
| DesktopSize | Yes | Dynamic resolution changes |
| ExtendedDesktopSize | Yes | Multi-monitor support |

**VNC Server Architecture**:
```c
// VNC server integration with core service
typedef struct _VNC_SERVER_CONFIG {
    USHORT Port;                    // Default: 5900
    BOOL   RequirePassword;         // Classic VNC auth
    BOOL   EnableAnonTLS;           // TLS wrapper
    BOOL   EnableUltraVNCAuth;      // MS-Logon support
    WCHAR  Password[8];             // VNC auth (max 8 chars)
    UINT   FramebufferWidth;
    UINT   FramebufferHeight;
    VNC_COLOR_DEPTH Depth;
} VNC_SERVER_CONFIG;

// VNC protocol handler callbacks
interface IVNCProtocolHandler {
    // Client requests framebuffer update
    void OnFramebufferUpdateRequest(RECT region, BOOL incremental);
    
    // Client sends pointer event (mouse)
    void OnPointerEvent(BYTE buttonMask, UINT16 x, UINT16 y);
    
    // Client sends key event
    void OnKeyEvent(BOOL down, UINT32 key);  // X11 keysym
    
    // Client cut text (clipboard)
    void OnClientCutText(const char* text);
};
```

**VNC Authentication Options**:
1. **None**: No authentication (disabled by default, requires explicit opt-in)
2. **VNC Auth**: Classic 8-character password (DES-encrypted challenge-response)
3. **MS-Logon II**: UltraVNC/Windows domain authentication (AD integration)
4. **AnonTLS + VNC Auth**: TLS tunnel with VNC authentication

**Key Mapping (X11 to Windows)**:
```c
// X11 keysym to Windows virtual-key code mapping
UINT32 X11KeysymToWindowsVK(UINT32 keysym) {
    switch(keysym) {
        case XK_Shift_L:    return VK_LSHIFT;
        case XK_Shift_R:    return VK_RSHIFT;
        case XK_Control_L:  return VK_LCONTROL;
        case XK_Control_R:  return VK_RCONTROL;
        case XK_Alt_L:      return VK_LMENU;
        case XK_Alt_R:      return VK_RMENU;
        case XK_Meta_L:     return VK_LWIN;
        case XK_Meta_R:     return VK_RWIN;
        case XK_Return:     return VK_RETURN;
        case XK_Escape:     return VK_ESCAPE;
        case XK_Tab:        return VK_TAB;
        // ... full Unicode mapping table
    }
}
```

**Framebuffer Format Conversion**:
- VNC typically uses RGBA or BGRA formats
- System must convert from DXGI_FORMAT_B8G8R8A8_UNORM to VNC pixel format
- Handle endianness differences (VNC is network/big-endian)
- Support 8-bit, 16-bit, and 32-bit color depths

**Integration Points**:
- Reuse the same virtual display framebuffer from IDD driver
- Map VNC pointer events to vhidmouse.sys driver
- Map VNC key events to vhidkb.sys driver
- Share hardware encoder output with VNC Tight/H.264 encodings

**Testing Compatibility**:
- RealVNC Viewer (Windows, macOS, Linux, iOS, Android)
- TightVNC Viewer
- TigerVNC (Linux/Unix)
- UltraVNC (Windows-specific features)
- Chrome Remote Desktop (VNC-based)
- VNC Viewer for Android/iOS

---

## Automated Testing Framework

### Test Definition Format

```yaml
# test-suite.yaml
description: "Login flow regression test"
version: "1.0"

setup:
  - action: display.set_resolution
    params: { width: 1920, height: 1080 }
  - action: system.wait
    params: { duration_ms: 1000 }

steps:
  - id: 1
    action: input.mouse.click
    description: "Click username field"
    params: { x: 960, y: 540, button: "left" }
    
  - id: 2
    action: input.keyboard.type
    description: "Enter username"
    params: { text: "testuser", interval_ms: 10 }
    
  - id: 3
    action: input.keyboard.press
    description: "Tab to password field"
    params: { key: "tab" }
    
  - id: 4
    action: input.keyboard.type
    description: "Enter password"
    params: { text: "testpass123", interval_ms: 10 }
    
  - id: 5
    action: input.keyboard.press
    description: "Submit form"
    params: { key: "return" }
    
  - id: 6
    action: display.wait_for_change
    description: "Wait for dashboard to load"
    params: { timeout_ms: 5000, similarity_threshold: 0.95 }
    
  - id: 7
    action: display.capture
    description: "Take screenshot for verification"
    params: { name: "dashboard_loaded", region: "fullscreen" }

assertions:
  - type: display.compare
    reference: "refs/dashboard_expected.png"
    actual: "dashboard_loaded"
    tolerance: 0.02
    
  - type: display.find_element
    template: "refs/logout_button.png"
    required: true

teardown:
  - action: input.keyboard.combo
    params: { keys: ["alt", "f4"] }
```

### Execution Engine

```csharp
public interface ITestExecutor
{
    // Execute a single test step
    Task<StepResult> ExecuteStepAsync(TestStep step);
    
    // Execute full test suite
    Task<TestResult> ExecuteSuiteAsync(TestSuite suite);
    
    // Pause/resume execution
    void Pause();
    void Resume();
    
    // Event notifications
    event EventHandler<StepStartedEventArgs> StepStarted;
    event EventHandler<StepCompletedEventArgs> StepCompleted;
    event EventHandler<AssertionFailedEventArgs> AssertionFailed;
}
```

### CI/CD Integration

```yaml
# Example: GitHub Actions integration
jobs:
  windows-test:
    runs-on: self-hosted  # Or windows-latest with KVM-Drivers installed
    steps:
      - uses: actions/checkout@v3
      
      - name: Run KVM Test Suite
        uses: kvm-drivers/action@v1
        with:
          test-suite: 'tests/login-flow.yaml'
          endpoint: 'wss://test-runner.internal:8443'
          api-key: ${{ secrets.KVM_API_KEY }}
          
      - name: Upload Results
        uses: actions/upload-artifact@v3
        with:
          name: test-results
          path: results/
```

---

## Communication Architecture

### Inter-Process Communication

```
┌───────────────────────────────────────────────────────────┐
│                    Communication Flow                      │
├───────────────────────────────────────────────────────────┤
│                                                            │
│  ┌──────────────┐      Named Pipe /      ┌──────────────┐│
│  │  Tray App    │<-- Local RPC (gRPC)-->│ Core Service ││
│  │  (UI)        │                         │ (KVMService) ││
│  └──────────────┘                         └──────┬───────┘│
│                                                   │        │
│                              Device IoControl     │        │
│                          ┌────────────────────────┘        │
│                          ▼                                 │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐    │
│  │  vhidkb.sys  │  │vhidmouse.sys │  │ vdisplay.sys │    │
│  │              │  │              │  │              │    │
│  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘    │
│         │                 │                 │              │
│         └─────────────────┴─────────────────┘              │
│                           │                                │
│                    Windows Kernel                          │
└───────────────────────────────────────────────────────────┘
```

**Communication Methods**:
- **Tray ↔ Core Service**: Named pipes with gRPC or custom protocol
- **Core Service ↔ Drivers**: DeviceIoControl with custom IOCTLs
- **Remote Clients ↔ Core Service**: WebSocket over TLS
- **Log Collection**: ETW (Event Tracing for Windows) sessions

---

## Security Model

### Threat Model

| Threat | Mitigation |
|--------|-----------|
| Unauthorized remote access | TLS 1.3 + Mutual Auth + IP allowlists |
| Driver tampering | Code signing + Secure Boot (where possible) |
| Man-in-the-middle | Certificate pinning + mutual authentication |
| Privilege escalation | Driver runs least-privilege; admin only for install |
| Input injection attacks | Rate limiting + session authentication |
| Log tampering | Immutable log storage + digital signatures |

### Authentication Levels

```
Level 1: Anonymous (Local automation only)
Level 2: Pre-shared key (Basic remote)
Level 3: Certificate-based (Production remote)
Level 4: Hardware token + Certificate (High-security)
```

### Audit Logging

All security-relevant events are logged:
- Connection attempts (success/failure)
- Authentication events
- Driver enable/disable
- Input injection events (with session ID)
- Configuration changes
- Permission changes

---

## Error Handling & Monitoring

### Health Check System

```csharp
public class DriverHealthMonitor
{
    // Periodic health checks
    public async Task<HealthReport> CheckHealthAsync()
    {
        var report = new HealthReport();
        
        report.Keyboard = await CheckDriverHealthAsync("vhidkb");
        report.Mouse = await CheckDriverHealthAsync("vhidmouse");
        report.Controller = await CheckDriverHealthAsync("vxinput");
        report.Display = await CheckDriverHealthAsync("vdisplay");
        
        report.LastCheck = DateTime.UtcNow;
        return report;
    }
    
    private async Task<DriverHealth> CheckDriverHealthAsync(string driverName)
    {
        // Check driver is loaded
        // Check device is present in PnP
        // Check recent error counts
        // Check I/O statistics
    }
}
```

### Alerting Rules

| Condition | Severity | Action |
|-----------|----------|--------|
| Driver crash | Critical | Notify + Auto-restart attempt |
| 3+ connection failures | Warning | Notify + Log analysis |
| Unusual input rate | Warning | Log + Optional throttle |
| Disk space low (logs) | Warning | Notify + Auto-cleanup old logs |
| Certificate expiring soon | Info | Notify + Auto-renewal attempt |

### Log Retention

- **Debug logs**: 7 days (configurable)
- **Error logs**: 90 days
- **Audit logs**: 1 year (compliance)
- **Crash dumps**: 30 days (with auto-analysis)

---

## Appendix: Technology Stack

| Component | Technology | Notes |
|-----------|-----------|-------|
| Drivers | WDF (KMDF/UMDF 2.0) | Windows 10+ compatible |
| Core Service | C++17 / Rust | High-performance, low-latency |
| Tray App | C# WPF or Win32 | Native Windows look and feel |
| Protocol | JSON-RPC 2.0 over WebSocket | Simple, extensible |
| Encryption | TLS 1.3 | Mandatory, no fallback |
| Logging | ETW + spdlog | Structured, queryable |
| Testing | Custom + Playwright | Integration capability |
