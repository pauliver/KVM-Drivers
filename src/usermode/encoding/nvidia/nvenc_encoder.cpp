// NVIDIA NVENC encoder implementation
#include <windows.h>
#include <d3d11.h>
#include <nvEncodeAPI.h>
#include <iostream>
#include <vector>
#include <cstdint>

#pragma comment(lib, "nvencodeapi.lib")
#pragma comment(lib, "d3d11.lib")

typedef NVENCSTATUS (NVENCAPI *NvEncodeAPICreateInstanceFunc)(NV_ENCODE_API_FUNCTION_LIST *functionList);

class NvencEncoder {
public:
    NvencEncoder() : encoder(nullptr), cudaContext(nullptr), initialized(false)
                     , bitstreamBuffer(nullptr) {
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

        // NVENC requires a valid device pointer. We use D3D11 here because
        // creating a full CUDA context requires the CUDA runtime library.
        // Create a minimal D3D11 device just for NVENC session open.
        ID3D11Device* d3dDev = nullptr;
        D3D_FEATURE_LEVEL fl;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                D3D11_SDK_VERSION, &d3dDev, &fl, nullptr))) {
            std::cerr << "[NVENC] D3D11 device creation failed" << std::endl;
            return false;
        }

        NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS params = {};
        params.version    = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
        params.apiVersion = NVENCAPI_VERSION;
        params.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
        params.device     = d3dDev;

        NVENCSTATUS openStatus = nvencFuncs.nvEncOpenEncodeSessionEx(&params, &encoder);
        d3dDev->Release();  // NVENC adds its own reference; release ours
        if (openStatus != NV_ENC_SUCCESS) {
            std::cerr << "nvEncOpenEncodeSessionEx failed: " << openStatus << std::endl;
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

    bool EncodeFrame(void* nv12Data, void** output, size_t* outputSize) {
        if (!initialized || !encoder) return false;

        // Register input resource (NV12 format)
        NV_ENC_REGISTER_RESOURCE regResource = {};
        regResource.version = NV_ENC_REGISTER_RESOURCE_VER;
        regResource.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
        regResource.resourceToRegister = nv12Data;
        regResource.width = width;
        regResource.height = height;
        regResource.pitch = width; // NV12 pitch
        regResource.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;

        if (nvencFuncs.nvEncRegisterResource(encoder, &regResource) != NV_ENC_SUCCESS) {
            std::cerr << "Failed to register input resource" << std::endl;
            return false;
        }

        // Map resource for encoding
        NV_ENC_MAP_INPUT_RESOURCE mapResource = {};
        mapResource.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
        mapResource.registeredResource = regResource.registeredResource;

        if (nvencFuncs.nvEncMapInputResource(encoder, &mapResource) != NV_ENC_SUCCESS) {
            std::cerr << "Failed to map input resource" << std::endl;
            nvencFuncs.nvEncUnregisterResource(encoder, regResource.registeredResource);
            return false;
        }

        // Create output bitstream buffer if not exists
        if (!bitstreamBuffer) {
            NV_ENC_CREATE_BITSTREAM_BUFFER createBitstreamBuffer = {};
            createBitstreamBuffer.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
            createBitstreamBuffer.size = 1024 * 1024; // 1MB buffer
            createBitstreamBuffer.memoryHeap = NV_ENC_MEMORY_HEAP_SYSMEM_CACHED;

            if (nvencFuncs.nvEncCreateBitstreamBuffer(encoder, &createBitstreamBuffer) != NV_ENC_SUCCESS) {
                std::cerr << "Failed to create bitstream buffer" << std::endl;
                nvencFuncs.nvEncUnmapInputResource(encoder, mapResource.mappedResource);
                nvencFuncs.nvEncUnregisterResource(encoder, regResource.registeredResource);
                return false;
            }
            bitstreamBuffer = createBitstreamBuffer.bitstreamBuffer;
        }

        // Encode frame
        // Only force IDR on the very first frame; subsequent frames are
        // P-frames, which dramatically reduces bandwidth.
        static bool firstFrame = true;
        NV_ENC_PIC_PARAMS picParams = {};
        picParams.version         = NV_ENC_PIC_PARAMS_VER;
        picParams.inputBuffer     = mapResource.mappedResource;
        picParams.bufferFmt       = NV_ENC_BUFFER_FORMAT_NV12;
        picParams.outputBitstream = bitstreamBuffer;
        picParams.inputWidth      = width;
        picParams.inputHeight     = height;
        picParams.inputPitch      = width;
        if (firstFrame) {
            picParams.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR;
            picParams.pictureType    = NV_ENC_PIC_TYPE_IDR;
            firstFrame = false;
        }
        picParams.codecPicParams.h264PicParams.sliceMode     = 0;
        picParams.codecPicParams.h264PicParams.sliceModeData = 0;

        NVENCSTATUS status = nvencFuncs.nvEncEncodePicture(encoder, &picParams);
        if (status != NV_ENC_SUCCESS) {
            std::cerr << "nvEncEncodePicture failed: " << status << std::endl;
            nvencFuncs.nvEncUnmapInputResource(encoder, mapResource.mappedResource);
            nvencFuncs.nvEncUnregisterResource(encoder, regResource.registeredResource);
            return false;
        }

        // Lock bitstream to get encoded data
        NV_ENC_LOCK_BITSTREAM lockBitstream = {};
        lockBitstream.version = NV_ENC_LOCK_BITSTREAM_VER;
        lockBitstream.outputBitstream = bitstreamBuffer;

        status = nvencFuncs.nvEncLockBitstream(encoder, &lockBitstream);
        if (status != NV_ENC_SUCCESS) {
            std::cerr << "nvEncLockBitstream failed: " << status << std::endl;
            nvencFuncs.nvEncUnmapInputResource(encoder, mapResource.mappedResource);
            nvencFuncs.nvEncUnregisterResource(encoder, regResource.registeredResource);
            return false;
        }

        // Copy encoded data to output
        size_t dataSize = lockBitstream.bitstreamSizeInBytes;
        if (encodedDataBuffer.size() < dataSize) {
            encodedDataBuffer.resize(dataSize);
        }
        memcpy(encodedDataBuffer.data(), lockBitstream.bitstreamBufferPtr, dataSize);

        *output = encodedDataBuffer.data();
        *outputSize = dataSize;

        // Cleanup
        nvencFuncs.nvEncUnlockBitstream(encoder, bitstreamBuffer);
        nvencFuncs.nvEncUnmapInputResource(encoder, mapResource.mappedResource);
        nvencFuncs.nvEncUnregisterResource(encoder, regResource.registeredResource);

        return true;
    }

    void Shutdown() {
        if (initialized && encoder) {
            if (bitstreamBuffer) {
                nvencFuncs.nvEncDestroyBitstreamBuffer(encoder, bitstreamBuffer);
                bitstreamBuffer = nullptr;
            }
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
    int width;
    int height;
    void* bitstreamBuffer;
    std::vector<uint8_t> encodedDataBuffer;
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
