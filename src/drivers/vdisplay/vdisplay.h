#pragma once

#include <windows.h>
#include <iddcx.h>

// IOCTL codes for display driver
#define IOCTL_VDISP_GET_FRAMEBUFFER  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x900, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VDISP_WAIT_FRAME       CTL_CODE(FILE_DEVICE_UNKNOWN, 0x901, METHOD_BUFFERED, FILE_ANY_ACCESS)

// GUID for container ID (randomly generated)
DEFINE_GUID(GUID_CONTAINER_ID, 0xA1B2C3D4, 0xE5F6, 0x7890, 0xAB, 0xCD, 0xEF, 0x12, 0x34, 0x56, 0x78, 0x90);

// IDD Plugin class for virtual display
typedef struct _VDISPLAY_CONTEXT {
    IDDCX_ADAPTER Adapter;
    IDDCX_MONITOR Monitor;
    HANDLE FrameCaptureEvent;   // Auto-reset: signalled each time a frame arrives
    UINT   Width;
    UINT   Height;
    UINT   RefreshRate;
    // Shared GPU texture handle from the most recent IDD frame.
    // Consumer (video pipeline / VNC server) calls OpenSharedResource with this.
    // Protected by FrameMutex: close the old handle before overwriting.
    CRITICAL_SECTION FrameMutex;
    HANDLE           SharedTextureHandle;  // HANDLE from IDXGIResource::GetSharedHandle
    UINT64           FrameCount;           // Monotonically increasing frame counter
} VDISPLAY_CONTEXT, *PVDISPLAY_CONTEXT;

// IDD Callback functions
class IddSamplePlugin {
public:
    static HRESULT WINAPI InitAdapter(
        IDDCX_ADAPTER AdapterObject,
        const IDD_CX_ADAPTER_INIT_PARAMS* pInArgs,
        IDD_CX_ADAPTER_INIT_PARAMS* pOutArgs
    );
    
    static HRESULT WINAPI ParseMonitorDescription(
        const IDD_CX_MONITOR_DESCRIPTION* pInDescription,
        IDD_CX_MONITOR_DESCRIPTION* pOutDescription
    );
    
    static HRESULT WINAPI FinishFrameProcessing(
        IDD_CX_FRAME_PROCESSING_CONTEXT Context,
        const IDD_CX_FINISH_FRAME_PROCESSING_PARAMS* pParams
    );
    
    static HRESULT WINAPI AssignSwapChain(
        IDDCX_MONITOR MonitorObject,
        const IDD_CX_ASSIGN_SWAP_CHAIN_SET* pParams,
        IDD_CX_ASSIGN_SWAP_CHAIN_SET* pOutParams
    );
    
    static HRESULT WINAPI UnassignSwapChain(
        IDDCX_MONITOR MonitorObject
    );
};

// IOCTL interface for user-mode
typedef struct _VDISP_FRAMEBUFFER_INFO {
    UINT Width;
    UINT Height;
    UINT Stride;
    DXGI_FORMAT Format;
    HANDLE SharedTextureHandle;
} VDISP_FRAMEBUFFER_INFO, *PVDISP_FRAMEBUFFER_INFO;

// IOCTL handler export
extern "C" __declspec(dllexport) BOOL WINAPI VDispIoctlHandler(
    DWORD dwIoControlCode,
    LPVOID lpInBuffer,
    DWORD nInBufferSize,
    LPVOID lpOutBuffer,
    DWORD nOutBufferSize,
    LPDWORD lpBytesReturned
);
