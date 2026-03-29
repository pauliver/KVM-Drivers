// encoder_manager.cpp - Hardware encoder abstraction and auto-selection
#include <windows.h>
#include <dxgi.h>
#include <iostream>
#include <memory>
#include <vector>

#pragma comment(lib, "dxgi.lib")

// Forward declarations for encoder C interfaces
extern "C" {
    // Intel QSV
    void* IntelEncoder_Create();
    bool IntelEncoder_Init(void* enc, int w, int h, int fps);
    bool IntelEncoder_Encode(void* enc, void* frame, void** out, size_t* size);
    void IntelEncoder_Destroy(void* enc);

    // AMD AMF
    void* Amf_Create();
    bool Amf_Init(void* enc, int w, int h, int fps);
    bool Amf_Encode(void* enc, void* frame, void** out, size_t* size);
    void Amf_Destroy(void* enc);

    // NVIDIA NVENC
    void* Nvenc_Create();
    bool Nvenc_Init(void* enc, int w, int h, int fps);
    bool Nvenc_Encode(void* enc, void* frame, void** out, size_t* size);
    void Nvenc_Destroy(void* enc);
}

enum class EncoderType {
    None,
    Intel_QSV,
    AMD_AMF,
    NVIDIA_NVENC,
    Software // Fallback
};

struct GpuInfo {
    std::wstring name;
    UINT vendorId;
    UINT deviceId;
    UINT dedicatedVideoMemory;
    bool isDiscrete;
    EncoderType supportedEncoder;
};

class EncoderManager {
public:
    EncoderManager() : currentEncoder(nullptr), currentType(EncoderType::None), width(1920), height(1080), fps(60) {}

    ~EncoderManager() {
        Shutdown();
    }

    // Detect available GPUs and their encoder support
    std::vector<GpuInfo> DetectGPUs() {
        std::vector<GpuInfo> gpus;

        IDXGIFactory1* pFactory = nullptr;
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&pFactory))) {
            std::cerr << "Failed to create DXGI factory" << std::endl;
            return gpus;
        }

        IDXGIAdapter1* pAdapter = nullptr;
        for (UINT i = 0; pFactory->EnumAdapters1(i, &pAdapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc;
            if (SUCCEEDED(pAdapter->GetDesc1(&desc))) {
                GpuInfo info;
                info.name = desc.Description;
                info.vendorId = desc.VendorId;
                info.deviceId = desc.DeviceId;
                info.dedicatedVideoMemory = static_cast<UINT>(desc.DedicatedVideoMemory / (1024 * 1024)); // MB
                info.isDiscrete = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 && desc.DedicatedVideoMemory > 0;

                // Determine encoder support based on vendor ID
                switch (desc.VendorId) {
                case 0x8086: // Intel
                    info.supportedEncoder = EncoderType::Intel_QSV;
                    break;
                case 0x1022: // AMD
                case 0x1002: // AMD (older PCI ID)
                    info.supportedEncoder = EncoderType::AMD_AMF;
                    break;
                case 0x10DE: // NVIDIA
                    info.supportedEncoder = EncoderType::NVIDIA_NVENC;
                    break;
                default:
                    info.supportedEncoder = EncoderType::None;
                }

                // Skip software adapters
                if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
                    gpus.push_back(info);
                }
            }
            pAdapter->Release();
        }

        pFactory->Release();
        return gpus;
    }

    // Select best encoder based on GPU availability and priority
    EncoderType SelectBestEncoder(const std::vector<GpuInfo>& gpus) {
        // Priority: NVIDIA NVENC > AMD AMF > Intel QSV > Software
        // But prefer discrete GPUs over integrated

        EncoderType bestDiscrete = EncoderType::None;
        EncoderType bestIntegrated = EncoderType::None;

        for (const auto& gpu : gpus) {
            if (gpu.isDiscrete && gpu.supportedEncoder != EncoderType::None) {
                // Prefer NVENC over AMF for discrete GPUs
                if (gpu.supportedEncoder == EncoderType::NVIDIA_NVENC) {
                    bestDiscrete = EncoderType::NVIDIA_NVENC;
                } else if (bestDiscrete == EncoderType::None && gpu.supportedEncoder == EncoderType::AMD_AMF) {
                    bestDiscrete = EncoderType::AMD_AMF;
                }
            } else if (!gpu.isDiscrete && gpu.supportedEncoder != EncoderType::None) {
                if (bestIntegrated == EncoderType::None) {
                    bestIntegrated = gpu.supportedEncoder;
                }
            }
        }

        // Prefer discrete GPU encoder
        if (bestDiscrete != EncoderType::None) {
            return bestDiscrete;
        }

        // Fall back to integrated
        if (bestIntegrated != EncoderType::None) {
            return bestIntegrated;
        }

        // No hardware encoder available
        return EncoderType::Software;
    }

    bool Initialize(int w, int h, int frameRate) {
        width = w;
        height = h;
        fps = frameRate;

        // Detect GPUs
        auto gpus = DetectGPUs();
        
        std::cout << "Detected " << gpus.size() << " GPU(s):" << std::endl;
        for (const auto& gpu : gpus) {
            std::wcout << L"  - " << gpu.name << L" (" << gpu.dedicatedVideoMemory << L" MB)";
            if (gpu.isDiscrete) std::wcout << L" [Discrete]";
            std::wcout << std::endl;
        }

        // Select best encoder
        currentType = SelectBestEncoder(gpus);

        // Initialize selected encoder
        switch (currentType) {
        case EncoderType::NVIDIA_NVENC:
            std::cout << "Using NVIDIA NVENC encoder" << std::endl;
            currentEncoder = Nvenc_Create();
            return Nvenc_Init(currentEncoder, width, height, fps);

        case EncoderType::AMD_AMF:
            std::cout << "Using AMD AMF encoder" << std::endl;
            currentEncoder = Amf_Create();
            return Amf_Init(currentEncoder, width, height, fps);

        case EncoderType::Intel_QSV:
            std::cout << "Using Intel QSV encoder" << std::endl;
            currentEncoder = IntelEncoder_Create();
            return IntelEncoder_Init(currentEncoder, width, height, fps);

        case EncoderType::Software:
        default:
            std::cout << "No hardware encoder available, will use software fallback" << std::endl;
            currentType = EncoderType::Software;
            return true; // Software encoder initialized separately
        }
    }

    bool EncodeFrame(void* nv12Data, void** output, size_t* outputSize) {
        if (!currentEncoder && currentType != EncoderType::Software) {
            return false;
        }

        switch (currentType) {
        case EncoderType::NVIDIA_NVENC:
            return Nvenc_Encode(currentEncoder, nv12Data, output, outputSize);

        case EncoderType::AMD_AMF:
            return Amf_Encode(currentEncoder, nv12Data, output, outputSize);

        case EncoderType::Intel_QSV:
            return IntelEncoder_Encode(currentEncoder, nv12Data, output, outputSize);

        case EncoderType::Software:
            // TODO: Implement software encoder fallback (e.g., x264 via OpenH264)
            return false;

        default:
            return false;
        }
    }

    void Shutdown() {
        if (currentEncoder) {
            switch (currentType) {
            case EncoderType::NVIDIA_NVENC:
                Nvenc_Destroy(currentEncoder);
                break;
            case EncoderType::AMD_AMF:
                Amf_Destroy(currentEncoder);
                break;
            case EncoderType::Intel_QSV:
                IntelEncoder_Destroy(currentEncoder);
                break;
            default:
                break;
            }
            currentEncoder = nullptr;
        }
        currentType = EncoderType::None;
    }

    EncoderType GetCurrentEncoderType() const {
        return currentType;
    }

    const char* GetEncoderName() const {
        switch (currentType) {
        case EncoderType::NVIDIA_NVENC: return "NVIDIA NVENC";
        case EncoderType::AMD_AMF: return "AMD AMF";
        case EncoderType::Intel_QSV: return "Intel QSV";
        case EncoderType::Software: return "Software (x264)";
        default: return "None";
        }
    }

private:
    void* currentEncoder;
    EncoderType currentType;
    int width;
    int height;
    int fps;
};

// C interface for DLL export
extern "C" {
    __declspec(dllexport) void* EncoderManager_Create() {
        return new EncoderManager();
    }

    __declspec(dllexport) bool EncoderManager_Init(void* manager, int w, int h, int fps) {
        return ((EncoderManager*)manager)->Initialize(w, h, fps);
    }

    __declspec(dllexport) bool EncoderManager_Encode(void* manager, void* frame, void** out, size_t* size) {
        return ((EncoderManager*)manager)->EncodeFrame(frame, out, size);
    }

    __declspec(dllexport) const char* EncoderManager_GetName(void* manager) {
        return ((EncoderManager*)manager)->GetEncoderName();
    }

    __declspec(dllexport) void EncoderManager_Destroy(void* manager) {
        delete (EncoderManager*)manager;
    }
}
