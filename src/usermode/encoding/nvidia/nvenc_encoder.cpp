// NVIDIA NVENC encoder implementation
#include <windows.h>
#include <nvEncodeAPI.h>
#include <cuda.h>
#include <iostream>

#pragma comment(lib, "nvencodeapi.lib")
#pragma comment(lib, "cuda.lib")

typedef NVENCSTATUS (NVENCAPI *NvEncodeAPICreateInstanceFunc)(NV_ENCODE_API_FUNCTION_LIST *functionList);

class NvencEncoder {
public:
    NvencEncoder() : encoder(nullptr), cudaContext(nullptr), initialized(false) {
        memset(&nvencFuncs, 0, sizeof(nvencFuncs));
    }

    bool Initialize(int width, int height, int fps) {
        HMODULE hModule = LoadLibraryA("nvEncodeAPI64.dll");
        if (!hModule) {
            std::cerr << "Failed to load nvEncodeAPI64.dll" << std::endl;
            return false;
        }

        auto createInstance = (NvEncodeAPICreateInstanceFunc)GetProcAddress(hModule, "NvEncodeAPICreateInstance");
        if (!createInstance) {
            std::cerr << "Failed to get NvEncodeAPICreateInstance" << std::endl;
            return false;
        }

        nvencFuncs.version = NV_ENCODE_API_FUNCTION_LIST_VER;
        if (createInstance(&nvencFuncs) != NV_ENC_SUCCESS) {
            std::cerr << "NvEncodeAPICreateInstance failed" << std::endl;
            return false;
        }

        NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS params = {};
        params.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
        params.apiVersion = NVENCAPI_VERSION;
        params.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
        params.device = cudaContext;

        if (nvencFuncs.nvEncOpenEncodeSessionEx(&params, &encoder) != NV_ENC_SUCCESS) {
            std::cerr << "nvEncOpenEncodeSessionEx failed" << std::endl;
            return false;
        }

        NV_ENC_INITIALIZE_PARAMS initParams = {};
        initParams.version = NV_ENC_INITIALIZE_PARAMS_VER;
        initParams.encodeGUID = NV_ENC_CODEC_H264_GUID;
        initParams.presetGUID = NV_ENC_PRESET_LOW_LATENCY_HQ_GUID;
        initParams.encodeWidth = width;
        initParams.encodeHeight = height;
        initParams.frameRateNum = fps;
        initParams.frameRateDen = 1;
        initParams.enablePTD = 1;

        NV_ENC_CONFIG encConfig = {};
        encConfig.version = NV_ENC_CONFIG_VER;
        encConfig.profileGUID = NV_ENC_H264_PROFILE_HIGH_GUID;
        encConfig.gopLength = NVENC_INFINITE_GOPLENGTH;
        encConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR_LOWDELAY_HQ;
        encConfig.rcParams.averageBitRate = 8000000;
        initParams.encodeConfig = &encConfig;

        if (nvencFuncs.nvEncInitializeEncoder(encoder, &initParams) != NV_ENC_SUCCESS) {
            std::cerr << "nvEncInitializeEncoder failed" << std::endl;
            nvencFuncs.nvEncDestroyEncoder(encoder);
            encoder = nullptr;
            return false;
        }

        initialized = true;
        std::cout << "NVENC initialized: " << width << "x" << height << " @ " << fps << "fps" << std::endl;
        return true;
    }

    bool EncodeFrame(void* frameData, void** output, size_t* size) {
        if (!initialized) return false;
        // TODO: Full encode implementation
        return true;
    }

    void Shutdown() {
        if (initialized && encoder) {
            nvencFuncs.nvEncDestroyEncoder(encoder);
            encoder = nullptr;
            initialized = false;
        }
    }

private:
    void* encoder;
    void* cudaContext;
    NV_ENCODE_API_FUNCTION_LIST nvencFuncs;
    bool initialized;
};

extern "C" {
    __declspec(dllexport) void* Nvenc_Create() { return new NvencEncoder(); }
    __declspec(dllexport) bool Nvenc_Init(void* enc, int w, int h, int fps) { 
        return ((NvencEncoder*)enc)->Initialize(w, h, fps); 
    }
    __declspec(dllexport) bool Nvenc_Encode(void* enc, void* frame, void** out, size_t* size) { 
        return ((NvencEncoder*)enc)->EncodeFrame(frame, out, size); 
    }
    __declspec(dllexport) void Nvenc_Destroy(void* enc) {
        ((NvencEncoder*)enc)->Shutdown();
        delete (NvencEncoder*)enc;
    }
}
