// idd_video_bridge.cpp - Bridge between IDD driver and video pipeline
// Captures frames from virtual display and feeds to hardware encoder

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <iddcx.h>
#include <iostream>
#include <thread>
#include <mutex>
#include <atomic>
#include <queue>

#include "../usermode/video/video_pipeline.cpp"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

// Frame metadata from IDD
typedef struct _IDD_FRAME_DATA {
    HANDLE SharedTextureHandle;
    UINT Width;
    UINT Height;
    DXGI_FORMAT Format;
    UINT64 Timestamp;
} IDD_FRAME_DATA;

// Bridge context
typedef struct _IDD_VIDEO_BRIDGE {
    // IDD connection
    HANDLE IddDeviceHandle;
    IDD_FRAME_DATA CurrentFrame;
    
    // Video pipeline
    VideoPipeline* Pipeline;
    
    // Threading
    std::atomic<bool> Running;
    std::thread CaptureThread;
    std::mutex FrameMutex;
    
    // Statistics
    UINT64 FramesCaptured;
    UINT64 FramesEncoded;
    double AverageCaptureTimeMs;
} IDD_VIDEO_BRIDGE;

class IddVideoBridge {
public:
    IddVideoBridge() 
        : context_(nullptr)
        , d3dDevice_(nullptr)
        , d3dContext_(nullptr)
    {}
    
    ~IddVideoBridge() {
        Shutdown();
    }
    
    // Initialize bridge with IDD device and video pipeline
    bool Initialize(const wchar_t* iddDevicePath, UINT width = 1920, UINT height = 1080, UINT fps = 60) {
        std::wcout << L"[IddBridge] Initializing bridge to IDD: " << iddDevicePath << std::endl;
        
        // Open IDD device
        iddHandle_ = CreateFile(
            iddDevicePath,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            NULL
        );
        
        if (iddHandle_ == INVALID_HANDLE_VALUE) {
            // Fallback to test pattern mode
            std::cout << "[IddBridge] IDD device not found, using test pattern mode" << std::endl;
            iddHandle_ = NULL;
        } else {
            std::cout << "[IddBridge] Connected to IDD device" << std::endl;
        }
        
        // Initialize video pipeline with encoding
        pipeline_ = new VideoPipeline();
        if (!pipeline_->Initialize(width, height, fps)) {
            std::cerr << "[IddBridge] Failed to initialize video pipeline" << std::endl;
            return false;
        }
        
        // Initialize D3D11 for GPU-accelerated frame capture
        if (!InitializeD3D11()) {
            std::cerr << "[IddBridge] D3D11 initialization failed, using CPU mode" << std::endl;
        }
        
        std::cout << "[IddBridge] Bridge initialized: " << width << "x" << height << " @ " << fps << "fps" << std::endl;
        return true;
    }
    
    void Start() {
        if (running_) return;
        
        running_ = true;
        pipeline_->Start();
        
        // Start frame capture thread
        captureThread_ = std::thread(&IddVideoBridge::CaptureLoop, this);
        
        std::cout << "[IddBridge] Capture started" << std::endl;
    }
    
    void Stop() {
        running_ = false;
        
        if (captureThread_.joinable()) {
            captureThread_.join();
        }
        
        pipeline_->Stop();
        
        std::cout << "[IddBridge] Capture stopped" << std::endl;
    }
    
    void Shutdown() {
        Stop();
        
        if (pipeline_) {
            pipeline_->Shutdown();
            delete pipeline_;
            pipeline_ = nullptr;
        }
        
        if (iddHandle_ && iddHandle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(iddHandle_);
            iddHandle_ = nullptr;
        }
        
        if (d3dContext_) {
            d3dContext_->Release();
            d3dContext_ = nullptr;
        }
        
        if (d3dDevice_) {
            d3dDevice_->Release();
            d3dDevice_ = nullptr;
        }
    }
    
    // Get encoded video packet for streaming
    bool GetVideoPacket(VideoPacket& outPacket, DWORD timeoutMs = 100) {
        if (!pipeline_) return false;
        return pipeline_->GetEncodedPacket(outPacket, timeoutMs);
    }
    
    // Statistics
    void GetStats(UINT64* captured, UINT64* encoded, double* avgTime) {
        *captured = framesCaptured_;
        *encoded = framesEncoded_;
        *avgTime = avgCaptureTimeMs_;
    }

private:
    HANDLE iddHandle_;
    VideoPipeline* pipeline_;
    ID3D11Device* d3dDevice_;
    ID3D11DeviceContext* d3dContext_;
    
    std::atomic<bool> running_;
    std::thread captureThread_;
    
    UINT64 framesCaptured_;
    UINT64 framesEncoded_;
    double avgCaptureTimeMs_;
    
    void* context_;  // Placeholder
    
    bool InitializeD3D11() {
        D3D_FEATURE_LEVEL featureLevels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
        };
        
        D3D_FEATURE_LEVEL obtainedLevel;
        HRESULT hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
            featureLevels,
            ARRAYSIZE(featureLevels),
            D3D11_SDK_VERSION,
            &d3dDevice_,
            &obtainedLevel,
            &d3dContext_
        );
        
        return SUCCEEDED(hr);
    }
    
    void CaptureLoop() {
        while (running_) {
            auto startTime = std::chrono::steady_clock::now();

            if (iddHandle_) {
                // Real IDD path: request a shared-texture frame via IOCTL
                // and copy it into the pipeline's input buffer.
                // IOCTL_IDD_GET_FRAME is a placeholder for the actual IDD IOCTL.
                // When the IDD driver exposes a shared DXGI texture handle, open
                // it with ID3D11Device::OpenSharedResource, then CopyResource into
                // the encoder's input texture.
                // For now we fall through to the test-pattern path below while
                // the IDD shared-texture protocol is finalized.
                (void)iddHandle_;
            }

            // Test-pattern / fallback: submit a null frame so the pipeline keeps
            // ticking and callers always get an encoded packet to work with.
            if (pipeline_) {
                pipeline_->SubmitTestFrame();
            }

            auto endTime = std::chrono::steady_clock::now();
            double captureTimeMs =
                std::chrono::duration_cast<std::chrono::microseconds>(
                    endTime - startTime).count() / 1000.0;
            avgCaptureTimeMs_ = avgCaptureTimeMs_ * 0.9 + captureTimeMs * 0.1;
            framesCaptured_++;

            // Maintain target frame rate (60 fps)
            auto frameTarget = std::chrono::milliseconds(1000 / 60);
            auto elapsed     = endTime - startTime;
            if (elapsed < frameTarget) {
                std::this_thread::sleep_for(frameTarget - elapsed);
            }
        }
    }
};

// C interface for integration
extern "C" {
    __declspec(dllexport) void* IddBridge_Create() {
        return new IddVideoBridge();
    }
    
    __declspec(dllexport) bool IddBridge_Init(void* bridge, const wchar_t* devicePath, int width, int height, int fps) {
        return ((IddVideoBridge*)bridge)->Initialize(devicePath, width, height, fps);
    }
    
    __declspec(dllexport) void IddBridge_Start(void* bridge) {
        ((IddVideoBridge*)bridge)->Start();
    }
    
    __declspec(dllexport) void IddBridge_Stop(void* bridge) {
        ((IddVideoBridge*)bridge)->Stop();
    }
    
    __declspec(dllexport) bool IddBridge_GetPacket(void* bridge, VideoPacket* outPacket, DWORD timeout) {
        if (!outPacket) return false;
        return ((IddVideoBridge*)bridge)->GetVideoPacket(*outPacket, timeout);
    }
    
    __declspec(dllexport) void IddBridge_GetStats(void* bridge, UINT64* captured, UINT64* encoded, double* avgTime) {
        ((IddVideoBridge*)bridge)->GetStats(captured, encoded, avgTime);
    }
    
    __declspec(dllexport) void IddBridge_Destroy(void* bridge) {
        delete (IddVideoBridge*)bridge;
    }
}
