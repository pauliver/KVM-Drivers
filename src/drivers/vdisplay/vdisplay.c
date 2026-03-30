/*
 * Virtual Display Driver (vdisplay.dll)
 * 
 * Indirect Display Driver (IDD) for Windows 10/11.
 * Creates virtual monitors and captures framebuffer for remote streaming.
 */

#include "vdisplay.h"
#include <iddcx.h>
#include <d3d11.h>

// Global plugin instance
static VDISPLAY_CONTEXT g_DisplayContext = {0};

// IDD Callback implementations
HRESULT WINAPI IddSamplePlugin::InitAdapter(
    IDDCX_ADAPTER AdapterObject,
    const IDD_CX_ADAPTER_INIT_PARAMS* pInArgs,
    IDD_CX_ADAPTER_INIT_PARAMS* pOutArgs
)
{
    UNREFERENCED_PARAMETER(pInArgs);
    
    // Store adapter handle
    g_DisplayContext.Adapter = AdapterObject;
    
    // Set adapter capabilities
    pOutArgs->pCaps->Size = sizeof(IDDCX_ADAPTER_CAPS);
    pOutArgs->pCaps->AdapterContainerId = GUID_CONTAINER_ID; // Need to define this
    
    // Max monitors this adapter supports
    pOutArgs->pCaps->MaxMonitors = 4;
    
    // Endpoints (one per monitor)
    pOutArgs->pCaps->MaxMonitorsSupportedOnEndpoints = 4;
    
    // Support all standard modes
    pOutArgs->pCaps->SupportsVirtualModes = true;
    
    return S_OK;
}

HRESULT WINAPI IddSamplePlugin::ParseMonitorDescription(
    const IDD_CX_MONITOR_DESCRIPTION* pInDescription,
    IDD_CX_MONITOR_DESCRIPTION* pOutDescription
)
{
    UNREFERENCED_PARAMETER(pInDescription);
    
    // Provide a generic monitor description
    // In production, this would come from a configurable EDID
    
    static const BYTE GenericEdid[128] = {
        0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, // Header
        0x5A, 0x63, 0x20, 0x20, 0x01, 0x01, 0x01, 0x01, // Vendor & Product ID
        0x1A, 0x20, 0x01, 0x03, 0x80, 0x30, 0x1B, 0x78, // Basic params
        0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Color characteristics
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        // Detailed timing descriptor for 1920x1080@60Hz
        0x02, 0x3A, 0x80, 0x18, 0x71, 0x38, 0x2D, 0x40,
        0x58, 0x2C, 0x45, 0x00, 0xFD, 0x1E, 0x11, 0x00,
        0x00, 0x1E, // Preferred timing
        0x00, 0x00, 0x00, 0xFD, 0x00, 0x32, 0x4B, 0x1E,
        0x52, 0x11, 0x00, 0x0A, 0x20, 0x20, 0x20, 0x20,
        0x20, 0x20, // Monitor range limits
        0x00, 0x00, 0x00, 0xFC, 0x00, 0x4B, 0x56, 0x4D,
        0x20, 0x56, 0x69, 0x72, 0x74, 0x75, 0x61, 0x6C,
        0x0A, 0x20, // Monitor name "KVM Virtual"
    };
    
    pOutDescription->Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
    pOutDescription->DataSize = sizeof(GenericEdid);
    memcpy(pOutDescription->pData, GenericEdid, sizeof(GenericEdid));
    
    return S_OK;
}

HRESULT WINAPI IddSamplePlugin::FinishFrameProcessing(
    IDD_CX_FRAME_PROCESSING_CONTEXT Context,
    const IDD_CX_FINISH_FRAME_PROCESSING_PARAMS* pParams
)
{
    UNREFERENCED_PARAMETER(Context);
    
    if (!pParams || !pParams->pFrameInfo) {
        return E_INVALIDARG;
    }
    
    // Access the shared texture from the OS
    ID3D11Texture2D* pFrameTexture = (ID3D11Texture2D*)pParams->pFrameInfo->pSurface;
    if (!pFrameTexture) {
        return E_FAIL;
    }
    
    // Get texture description
    D3D11_TEXTURE2D_DESC desc;
    pFrameTexture->GetDesc(&desc);
    
    // Update context with current resolution
    g_DisplayContext.Width = desc.Width;
    g_DisplayContext.Height = desc.Height;
    
    // Publish the DXGI shared handle so consumers (video pipeline, VNC server)
    // can call ID3D11Device::OpenSharedResource on their own D3D11 device.
    IDXGIResource* pDxgiRes = NULL;
    HRESULT hrShare = pFrameTexture->QueryInterface(
        &IID_IDXGIResource, (void**)&pDxgiRes);
    if (SUCCEEDED(hrShare) && pDxgiRes) {
        HANDLE hNew = NULL;
        if (SUCCEEDED(pDxgiRes->GetSharedHandle(&hNew)) && hNew) {
            EnterCriticalSection(&g_DisplayContext.FrameMutex);
            g_DisplayContext.SharedTextureHandle = hNew;
            g_DisplayContext.FrameCount++;
            LeaveCriticalSection(&g_DisplayContext.FrameMutex);
        }
        pDxgiRes->Release();
    }

    if (g_DisplayContext.FrameCaptureEvent) {
        SetEvent(g_DisplayContext.FrameCaptureEvent);
    }
    
    return S_OK;
}

// Additional IDD callbacks for monitor management
HRESULT WINAPI IddSamplePlugin::AssignSwapChain(
    IDDCX_MONITOR MonitorObject,
    const IDD_CX_ASSIGN_SWAP_CHAIN_SET* pParams,
    IDD_CX_ASSIGN_SWAP_CHAIN_SET* pOutParams
)
{
    UNREFERENCED_PARAMETER(pOutParams);
    
    g_DisplayContext.Monitor = MonitorObject;
    
    // Store swap chain info for frame processing
    // The pParams->hSwapChain contains the shared handle
    
    return S_OK;
}

HRESULT WINAPI IddSamplePlugin::UnassignSwapChain(
    IDDCX_MONITOR MonitorObject
)
{
    UNREFERENCED_PARAMETER(MonitorObject);
    
    // Cleanup swap chain resources
    g_DisplayContext.Monitor = nullptr;
    
    return S_OK;
}

// Entry point for IDD
extern "C" __declspec(dllexport) HRESULT WINAPI IddPluginInit(
    IDDCX_ADAPTER Adapter,
    const IDD_CX_PLUGIN_INIT_PARAMS* pParams
)
{
    if (!pParams || pParams->Size < sizeof(IDD_CX_PLUGIN_INIT_PARAMS)) {
        return E_INVALIDARG;
    }
    
    // Register plugin callbacks
    static IDD_CX_CALLBACKS callbacks = {};
    callbacks.Size = sizeof(IDD_CX_CALLBACKS);
    callbacks.pfnInitAdapter = IddSamplePlugin::InitAdapter;
    callbacks.pfnParseMonitorDescription = IddSamplePlugin::ParseMonitorDescription;
    callbacks.pfnFinishFrameProcessing = IddSamplePlugin::FinishFrameProcessing;
    callbacks.pfnAssignSwapChain = IddSamplePlugin::AssignSwapChain;
    callbacks.pfnUnassignSwapChain = IddSamplePlugin::UnassignSwapChain;
    
    // Report callbacks to OS
    pParams->pOutCallbacks->Size = sizeof(IDD_CX_CALLBACKS);
    *pParams->pOutCallbacks = callbacks;
    
    // Initialize context
    g_DisplayContext.Adapter = Adapter;
    g_DisplayContext.FrameCaptureEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    InitializeCriticalSection(&g_DisplayContext.FrameMutex);
    
    return S_OK;
}

// IOCTL handler for user-mode communication
extern "C" __declspec(dllexport) BOOL WINAPI VDispIoctlHandler(
    DWORD dwIoControlCode,
    LPVOID lpInBuffer,
    DWORD nInBufferSize,
    LPVOID lpOutBuffer,
    DWORD nOutBufferSize,
    LPDWORD lpBytesReturned
)
{
    UNREFERENCED_PARAMETER(nInBufferSize);
    UNREFERENCED_PARAMETER(nOutBufferSize);
    
    switch (dwIoControlCode) {
    case IOCTL_VDISP_GET_FRAMEBUFFER: {
        if (lpOutBuffer && lpBytesReturned) {
            PVDISP_FRAMEBUFFER_INFO info = (PVDISP_FRAMEBUFFER_INFO)lpOutBuffer;
            info->Width = g_DisplayContext.Width;
            info->Height = g_DisplayContext.Height;
            info->Stride = g_DisplayContext.Width * 4; // RGBA32
            info->Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            EnterCriticalSection(&g_DisplayContext.FrameMutex);
            info->SharedTextureHandle = g_DisplayContext.SharedTextureHandle;
            LeaveCriticalSection(&g_DisplayContext.FrameMutex);
            *lpBytesReturned = sizeof(VDISP_FRAMEBUFFER_INFO);
            return TRUE;
        }
        break;
    }
    
    case IOCTL_VDISP_WAIT_FRAME: {
        if (g_DisplayContext.FrameCaptureEvent) {
            // Wait for next frame with timeout
            DWORD timeout = (lpInBuffer) ? *(DWORD*)lpInBuffer : 1000;
            DWORD result = WaitForSingleObject(g_DisplayContext.FrameCaptureEvent, timeout);
            return (result == WAIT_OBJECT_0) ? TRUE : FALSE;
        }
        break;
    }
    }
    
    return FALSE;
}

// DLL entry point
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    UNREFERENCED_PARAMETER(lpReserved);
    
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        break;
        
    case DLL_PROCESS_DETACH:
        if (g_DisplayContext.FrameCaptureEvent) {
            CloseHandle(g_DisplayContext.FrameCaptureEvent);
            g_DisplayContext.FrameCaptureEvent = nullptr;
        }
        DeleteCriticalSection(&g_DisplayContext.FrameMutex);
        break;
    }
    
    return TRUE;
}
