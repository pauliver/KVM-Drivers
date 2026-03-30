// video_pipeline.cpp - Frame capture to encoding to streaming pipeline
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <winsock2.h>
#include <thread>
#include <mutex>
#include <queue>
#include <vector>
#include <chrono>
#include <iostream>

#include "../encoding/encoder_manager.cpp"
#include "../core/driver_interface.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

// Video frame structure
struct VideoFrame {
    std::vector<BYTE> data;
    UINT width;
    UINT height;
    UINT pitch;
    DXGI_FORMAT format;
    std::chrono::steady_clock::time_point timestamp;
    bool isKeyFrame;
};

// Encoded video packet
struct VideoPacket {
    std::vector<BYTE> data;
    bool isKeyFrame;
    std::chrono::steady_clock::time_point timestamp;
};

// Video pipeline class
class VideoPipeline {
public:
    VideoPipeline() 
        : d3dDevice(nullptr)
        , d3dContext(nullptr)
        , dupl(nullptr)
        , stagingTexture(nullptr)
        , encoderManager(nullptr)
        , running(false)
        , targetFps(60)
        , frameWidth(1920)
        , frameHeight(1080)
    {}

    ~VideoPipeline() {
        Shutdown();
    }

    bool Initialize(UINT width = 1920, UINT height = 1080, UINT fps = 60) {
        frameWidth = width;
        frameHeight = height;
        targetFps = fps;

        std::cout << "[VideoPipeline] Initializing: " << width << "x" << height << " @ " << fps << "fps" << std::endl;

        // Initialize encoder
        encoderManager = new EncoderManager();
        if (!encoderManager->Initialize(width, height, fps)) {
            std::cerr << "[VideoPipeline] Failed to initialize encoder" << std::endl;
            return false;
        }

        std::cout << "[VideoPipeline] Using encoder: " << encoderManager->GetEncoderName() << std::endl;

        // Initialize capture (for now, use desktop duplication as example)
        // In production, this would come from IDD shared texture
        if (!InitializeDesktopDuplication()) {
            std::cerr << "[VideoPipeline] Failed to initialize desktop duplication" << std::endl;
            // Continue anyway - we'll use test pattern
        }

        return true;
    }

    void Start() {
        if (running) return;

        running = true;
        
        // Start capture thread
        captureThread = std::thread(&VideoPipeline::CaptureLoop, this);
        
        // Start encode thread
        encodeThread = std::thread(&VideoPipeline::EncodeLoop, this);

        std::cout << "[VideoPipeline] Started" << std::endl;
    }

    void Stop() {
        running = false;

        // Wake up any waiting threads
        frameAvailable.notify_all();
        encodedAvailable.notify_all();

        if (captureThread.joinable()) {
            captureThread.join();
        }
        if (encodeThread.joinable()) {
            encodeThread.join();
        }

        std::cout << "[VideoPipeline] Stopped" << std::endl;
    }

    void Shutdown() {
        Stop();

        if (stagingTexture) {
            stagingTexture->Release();
            stagingTexture = nullptr;
        }
        if (dupl) {
            dupl->Release();
            dupl = nullptr;
        }
        if (d3dContext) {
            d3dContext->Release();
            d3dContext = nullptr;
        }
        if (d3dDevice) {
            d3dDevice->Release();
            d3dDevice = nullptr;
        }

        if (encoderManager) {
            encoderManager->Shutdown();
            delete encoderManager;
            encoderManager = nullptr;
        }
    }

    // Get encoded packet for streaming
    // Push a test-pattern frame immediately into the capture queue
    // (called from IddVideoBridge when no real IDD frame is available)
    void SubmitTestFrame() {
        VideoFrame frame;
        GenerateTestPattern(frame);
        {
            std::unique_lock<std::mutex> lock(captureMutex);
            while (captureQueue.size() >= maxCaptureQueueSize)
                captureQueue.pop();
            captureQueue.push(std::move(frame));
            framesCaptured++;
        }
        frameAvailable.notify_one();
    }

    bool GetEncodedPacket(VideoPacket& packet, DWORD timeoutMs = 100) {
        std::unique_lock<std::mutex> lock(encodedMutex);
        
        auto timeout = std::chrono::milliseconds(timeoutMs);
        if (!encodedAvailable.wait_for(lock, timeout, [this]() { return !encodedQueue.empty() || !running; })) {
            return false;
        }

        if (!running && encodedQueue.empty()) {
            return false;
        }

        packet = std::move(encodedQueue.front());
        encodedQueue.pop();
        return true;
    }

    // Statistics
    struct Stats {
        UINT64 framesCaptured;
        UINT64 framesEncoded;
        UINT64 bytesEncoded;
        double averageEncodeTimeMs;
        double currentFps;
    };

    Stats GetStats() const {
        Stats s = {};
        s.framesCaptured = framesCaptured.load();
        s.framesEncoded = framesEncoded.load();
        s.bytesEncoded = bytesEncoded.load();
        return s;
    }

private:
    // DirectX resources for capture
    ID3D11Device* d3dDevice;
    ID3D11DeviceContext* d3dContext;
    IDXGIOutputDuplication* dupl;
    ID3D11Texture2D* stagingTexture;  // Cached: re-used across frames to avoid per-frame alloc
    UINT stagingWidth = 0;
    UINT stagingHeight = 0;

    // Encoder
    EncoderManager* encoderManager;

    // Threading
    std::atomic<bool> running;
    std::thread captureThread;
    std::thread encodeThread;

    // Frame queues
    std::queue<VideoFrame> captureQueue;
    std::mutex captureMutex;
    std::condition_variable frameAvailable;
    const size_t maxCaptureQueueSize = 3;  // Limit latency

    std::queue<VideoPacket> encodedQueue;
    std::mutex encodedMutex;
    std::condition_variable encodedAvailable;
    const size_t maxEncodedQueueSize = 10;

    // Settings
    UINT targetFps;
    UINT frameWidth;
    UINT frameHeight;

    // Statistics
    std::atomic<UINT64> framesCaptured{0};
    std::atomic<UINT64> framesEncoded{0};
    std::atomic<UINT64> bytesEncoded{0};

    bool InitializeDesktopDuplication() {
        HRESULT hr;

        // Create D3D11 device
        D3D_FEATURE_LEVEL featureLevel;
        hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr,
            0,
            D3D11_SDK_VERSION,
            &d3dDevice,
            &featureLevel,
            &d3dContext
        );

        if (FAILED(hr)) {
            std::cerr << "D3D11CreateDevice failed: 0x" << std::hex << hr << std::endl;
            return false;
        }

        // Get DXGI device
        IDXGIDevice* dxgiDevice = nullptr;
        hr = d3dDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
        if (FAILED(hr)) {
            return false;
        }

        // Get DXGI adapter
        IDXGIAdapter* dxgiAdapter = nullptr;
        hr = dxgiDevice->GetParent(__uuidof(IDXGIAdapter), (void**)&dxgiAdapter);
        dxgiDevice->Release();
        if (FAILED(hr)) {
            return false;
        }

        // Get output
        IDXGIOutput* dxgiOutput = nullptr;
        hr = dxgiAdapter->EnumOutputs(0, &dxgiOutput);
        dxgiAdapter->Release();
        if (FAILED(hr)) {
            return false;
        }

        // Get output 1
        IDXGIOutput1* dxgiOutput1 = nullptr;
        hr = dxgiOutput->QueryInterface(__uuidof(IDXGIOutput1), (void**)&dxgiOutput1);
        dxgiOutput->Release();
        if (FAILED(hr)) {
            return false;
        }

        // Create desktop duplication
        hr = dxgiOutput1->DuplicateOutput(d3dDevice, &dupl);
        dxgiOutput1->Release();
        if (FAILED(hr)) {
            std::cerr << "DuplicateOutput failed: 0x" << std::hex << hr << std::endl;
            return false;
        }

        std::cout << "[VideoPipeline] Desktop duplication initialized" << std::endl;
        return true;
    }

    void CaptureLoop() {
        auto frameInterval = std::chrono::milliseconds(1000 / targetFps);
        
        while (running) {
            auto startTime = std::chrono::steady_clock::now();

            VideoFrame frame;
            
            if (dupl) {
                // Capture from desktop duplication
                if (!CaptureFromDuplication(frame)) {
                    // Fall back to test pattern
                    GenerateTestPattern(frame);
                }
            } else {
                // Generate test pattern
                GenerateTestPattern(frame);
            }

            // Queue frame for encoding
            {
                std::unique_lock<std::mutex> lock(captureMutex);
                
                // Drop old frames if queue is full
                while (captureQueue.size() >= maxCaptureQueueSize) {
                    captureQueue.pop();
                }
                
                captureQueue.push(std::move(frame));
                framesCaptured++;
            }
            
            frameAvailable.notify_one();

            // Maintain target FPS
            auto elapsed = std::chrono::steady_clock::now() - startTime;
            auto sleepTime = frameInterval - elapsed;
            if (sleepTime > std::chrono::milliseconds(0)) {
                std::this_thread::sleep_for(sleepTime);
            }
        }
    }

    bool CaptureFromDuplication(VideoFrame& frame) {
        if (!dupl) return false;

        IDXGIResource* desktopResource = nullptr;
        DXGI_OUTDUPL_FRAME_FRAME_INFO frameInfo;

        HRESULT hr = dupl->AcquireNextFrame(100, &frameInfo, &desktopResource);
        if (FAILED(hr)) {
            return false;
        }

        // Get texture
        ID3D11Texture2D* desktopTexture = nullptr;
        hr = desktopResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&desktopTexture);
        desktopResource->Release();
        if (FAILED(hr)) {
            dupl->ReleaseFrame();
            return false;
        }

        // Get texture description
        D3D11_TEXTURE2D_DESC desc;
        desktopTexture->GetDesc(&desc);

        // Re-create staging texture only when dimensions change (or first frame)
        if (!stagingTexture || stagingWidth != desc.Width || stagingHeight != desc.Height) {
            if (stagingTexture) {
                stagingTexture->Release();
                stagingTexture = nullptr;
            }
            D3D11_TEXTURE2D_DESC stagingDesc = desc;
            stagingDesc.Usage          = D3D11_USAGE_STAGING;
            stagingDesc.BindFlags      = 0;
            stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            stagingDesc.MiscFlags      = 0;

            hr = d3dDevice->CreateTexture2D(&stagingDesc, nullptr, &stagingTexture);
            if (FAILED(hr)) {
                desktopTexture->Release();
                dupl->ReleaseFrame();
                return false;
            }
            stagingWidth  = desc.Width;
            stagingHeight = desc.Height;
        }

        // Copy to cached staging texture
        d3dContext->CopyResource(stagingTexture, desktopTexture);
        d3dContext->Flush();

        // Map and read
        D3D11_MAPPED_SUBRESOURCE mapped;
        hr = d3dContext->Map(stagingTexture, 0, D3D11_MAP_READ, 0, &mapped);
        if (SUCCEEDED(hr)) {
            // Copy frame data
            frame.width = desc.Width;
            frame.height = desc.Height;
            frame.pitch = mapped.RowPitch;
            frame.format = desc.Format;
            frame.timestamp = std::chrono::steady_clock::now();
            frame.isKeyFrame = false;

            // Allocate and copy
            size_t dataSize = mapped.RowPitch * desc.Height;
            frame.data.resize(dataSize);
            
            BYTE* src = (BYTE*)mapped.pData;
            BYTE* dst = frame.data.data();
            
            for (UINT row = 0; row < desc.Height; row++) {
                memcpy(dst + row * desc.Width * 4, src + row * mapped.RowPitch, desc.Width * 4);
            }

            d3dContext->Unmap(stagingTexture, 0);
        }

        // stagingTexture is NOT released here — it's cached for the next frame
        desktopTexture->Release();
        dupl->ReleaseFrame();

        return SUCCEEDED(hr);
    }

    void GenerateTestPattern(VideoFrame& frame) {
        // Generate a simple test pattern (color bars)
        frame.width = frameWidth;
        frame.height = frameHeight;
        frame.pitch = frameWidth * 4;
        frame.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        frame.timestamp = std::chrono::steady_clock::now();
        frame.isKeyFrame = true;

        size_t dataSize = frameWidth * frameHeight * 4;
        frame.data.resize(dataSize);

        // Simple color bar pattern
        UINT barWidth = frameWidth / 8;
        DWORD colors[] = {
            0xFFFFFFFF,  // White
            0xFFFFFF00,  // Yellow
            0xFF00FFFF,  // Cyan
            0xFF00FF00,  // Green
            0xFFFF00FF,  // Magenta
            0xFFFF0000,  // Red
            0xFF0000FF,  // Blue
            0xFF000000,  // Black
        };

        DWORD* pixels = (DWORD*)frame.data.data();
        for (UINT y = 0; y < frameHeight; y++) {
            for (UINT x = 0; x < frameWidth; x++) {
                UINT bar = x / barWidth;
                if (bar > 7) bar = 7;
                pixels[y * frameWidth + x] = colors[bar];
            }
        }
    }

    // Convert BGRA (4 bytes/pixel) to planar NV12 (Y plane + interleaved UV plane)
    // nv12 must be pre-allocated: width * height * 3 / 2 bytes
    static void ConvertBGRAtoNV12(const BYTE* bgra, BYTE* nv12, int width, int height) {
        BYTE* yPlane  = nv12;
        BYTE* uvPlane = nv12 + (size_t)width * height;

        for (int row = 0; row < height; row++) {
            for (int col = 0; col < width; col++) {
                const BYTE* px = bgra + ((size_t)row * width + col) * 4;
                BYTE b = px[0], g = px[1], r = px[2];

                // BT.601 limited-range
                int y = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
                yPlane[(size_t)row * width + col] = (BYTE)y;

                // UV sub-sampled 2x2: only write on even rows/cols
                if ((row & 1) == 0 && (col & 1) == 0) {
                    // Average over 2x2 block (simple — row/col are both even)
                    int cb = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
                    int cr = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
                    size_t uvOff = ((size_t)(row / 2) * width) + col;
                    uvPlane[uvOff]     = (BYTE)cb;
                    uvPlane[uvOff + 1] = (BYTE)cr;
                }
            }
        }
    }

    void EncodeLoop() {
        // Reusable NV12 conversion buffer
        std::vector<BYTE> nv12Buf;

        while (running) {
            VideoFrame frame;
            
            // Wait for frame
            {
                std::unique_lock<std::mutex> lock(captureMutex);
                frameAvailable.wait(lock, [this]() { return !captureQueue.empty() || !running; });
                
                if (!running && captureQueue.empty()) {
                    break;
                }
                
                frame = std::move(captureQueue.front());
                captureQueue.pop();
            }

            auto encodeStart = std::chrono::steady_clock::now();

            VideoPacket packet;
            packet.timestamp = frame.timestamp;
            packet.isKeyFrame = frame.isKeyFrame;

            // Attempt hardware encoding via EncoderManager
            bool encoded = false;
            if (encoderManager && !frame.data.empty()) {
                size_t nv12Size = (size_t)frame.width * frame.height * 3 / 2;
                if (nv12Buf.size() < nv12Size) nv12Buf.resize(nv12Size);

                ConvertBGRAtoNV12(frame.data.data(), nv12Buf.data(),
                    (int)frame.width, (int)frame.height);

                void* encodedData  = nullptr;
                size_t encodedSize = 0;
                if (encoderManager->EncodeFrame(nv12Buf.data(), &encodedData, &encodedSize)
                        && encodedData && encodedSize > 0) {
                    packet.data.assign(
                        static_cast<BYTE*>(encodedData),
                        static_cast<BYTE*>(encodedData) + encodedSize);
                    encoded = true;
                }
            }

            // Fallback: pass raw BGRA when encoder unavailable or failed
            if (!encoded) {
                packet.data = std::move(frame.data);
            }

            auto encodeEnd = std::chrono::steady_clock::now();
            auto encodeTime = std::chrono::duration_cast<std::chrono::milliseconds>(encodeEnd - encodeStart);
            (void)encodeTime;

            // Queue encoded packet
            {
                std::unique_lock<std::mutex> lock(encodedMutex);
                
                while (encodedQueue.size() >= maxEncodedQueueSize) {
                    encodedQueue.pop();
                }
                
                bytesEncoded += packet.data.size();
                encodedQueue.push(std::move(packet));
                framesEncoded++;
            }
            
            encodedAvailable.notify_one();
        }
    }
};

// C interface for integration
extern "C" {
    __declspec(dllexport) void* VideoPipeline_Create() {
        return new VideoPipeline();
    }

    __declspec(dllexport) bool VideoPipeline_Init(void* pipeline, int width, int height, int fps) {
        return ((VideoPipeline*)pipeline)->Initialize(width, height, fps);
    }

    __declspec(dllexport) void VideoPipeline_Start(void* pipeline) {
        ((VideoPipeline*)pipeline)->Start();
    }

    __declspec(dllexport) void VideoPipeline_Stop(void* pipeline) {
        ((VideoPipeline*)pipeline)->Stop();
    }

    __declspec(dllexport) bool VideoPipeline_GetPacket(void* pipeline, void** data, size_t* size, DWORD timeoutMs) {
        VideoPacket packet;
        if (!((VideoPipeline*)pipeline)->GetEncodedPacket(packet, timeoutMs)) {
            return false;
        }
        
        // Allocate memory that caller must release via VideoPipeline_FreePacket
        *size = packet.data.size();
        *data = new(std::nothrow) BYTE[*size];
        if (!*data) return false;
        
        memcpy(*data, packet.data.data(), *size);
        return true;
    }

    __declspec(dllexport) void VideoPipeline_FreePacket(void* data) {
        delete[] data;
    }

    __declspec(dllexport) void VideoPipeline_Destroy(void* pipeline) {
        delete (VideoPipeline*)pipeline;
    }
}
