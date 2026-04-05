// AMD AMF encoder implementation
#include <windows.h>
#include <AMF/core/Factory.h>
#include <AMF/components/VideoEncoderVCE.h>
#include <AMF/components/Component.h>
#include <iostream>
#include <vector>
#include <cstdint>

#pragma comment(lib, "amf.lib")

class AmfEncoder {
public:
    AmfEncoder() : factory(nullptr), context(nullptr), encoder(nullptr), initialized(false) {}

    bool Initialize(int w, int h, int fps) {
        width  = w;
        height = h;
        // Load AMF library
        HMODULE hModule = LoadLibraryA("amfrt64.dll");
        if (!hModule) {
            std::cerr << "Failed to load amfrt64.dll" << std::endl;
            return false;
        }

        auto initFunc = (AMFInit_Fn)GetProcAddress(hModule, AMF_INIT_FUNCTION_NAME);
        if (!initFunc) {
            std::cerr << "Failed to get AMFInit" << std::endl;
            return false;
        }

        if (initFunc(&factory) != AMF_OK) {
            std::cerr << "AMFInit failed" << std::endl;
            return false;
        }

        // Create context
        if (factory->CreateContext(&context) != AMF_OK) {
            std::cerr << "CreateContext failed" << std::endl;
            return false;
        }

        // Initialize DX11 (or DX12/Vulkan)
        if (context->InitDX11(nullptr) != AMF_OK) {
            std::cerr << "InitDX11 failed, trying OpenCL..." << std::endl;
            if (context->InitOpenCL() != AMF_OK) {
                std::cerr << "InitOpenCL failed" << std::endl;
                return false;
            }
        }

        // Create encoder component
        if (factory->CreateComponent(context, AMFVideoEncoderVCE_AVC, &encoder) != AMF_OK) {
            std::cerr << "CreateComponent (VCE AVC) failed" << std::endl;
            return false;
        }

        // Set encoder properties
        encoder->SetProperty(AMF_VIDEO_ENCODER_USAGE, AMF_VIDEO_ENCODER_USAGE_LOW_LATENCY);
        encoder->SetProperty(AMF_VIDEO_ENCODER_TARGET_BITRATE, 8000000); // 8 Mbps
        encoder->SetProperty(AMF_VIDEO_ENCODER_FRAMERATE, AMFConstructRate(fps, 1));
        encoder->SetProperty(AMF_VIDEO_ENCODER_FRAMESIZE, AMFConstructSize(width, height));
        encoder->SetProperty(AMF_VIDEO_ENCODER_PROFILE, AMF_VIDEO_ENCODER_PROFILE_HIGH);

        // Init
        if (encoder->Init(amf::AMF_SURFACE_NV12, width, height) != AMF_OK) {
            std::cerr << "Encoder Init failed" << std::endl;
            encoder->Terminate();
            encoder = nullptr;
            return false;
        }

        initialized = true;
        std::cout << "AMF encoder initialized: " << width << "x" << height << " @ " << fps << "fps" << std::endl;
        return true;
    }

    bool EncodeFrame(void* nv12Data, void** output, size_t* outputSize) {
        if (!initialized || !encoder || !context) return false;

        // Create AMF surface from NV12 data
        AMF_SURFACE_FORMAT format = AMF_SURFACE_NV12;
        amf::AMFSurface* surface = nullptr;
        
        AMF_RESULT res = context->AllocSurface(
            amf::AMF_MEMORY_HOST,  // Or AMF_MEMORY_DX11 for GPU
            format,
            width,
            height,
            &surface
        );
        
        if (res != AMF_OK || !surface) {
            std::cerr << "Failed to allocate AMF surface: " << res << std::endl;
            return false;
        }

        // Copy NV12 data to AMF surface
        // NV12 layout: Y plane (width * height), then UV plane (width * height / 2)
        amf_uint8* yPlane = (amf_uint8*)surface->GetPlaneAt(0)->GetNative();
        amf_uint8* uvPlane = (amf_uint8*)surface->GetPlaneAt(1)->GetNative();
        
        if (yPlane && uvPlane && nv12Data) {
            // Copy Y plane
            memcpy(yPlane, nv12Data, width * height);
            // Copy UV plane
            memcpy(uvPlane, (char*)nv12Data + width * height, width * height / 2);
        }

        // Submit frame to encoder
        res = encoder->SubmitInput(surface);
        surface->Release();
        
        if (res != AMF_OK) {
            std::cerr << "SubmitInput failed: " << res << std::endl;
            return false;
        }

        // Query output
        amf::AMFData* data = nullptr;
        res = encoder->QueryOutput(&data);
        
        if (res == AMF_OK && data) {
            amf::AMFBuffer* buffer = (amf::AMFBuffer*)data;
            
            size_t dataSize = buffer->GetSize();
            if (encodedBuffer.size() < dataSize) {
                encodedBuffer.resize(dataSize);
            }
            
            memcpy(encodedBuffer.data(), buffer->GetNative(), dataSize);
            *output = encodedBuffer.data();
            *outputSize = dataSize;
            
            buffer->Release();
            return true;
        }

        // No output yet (encoder buffering)
        return false;
    }

    void DrainEncoder() {
        if (encoder) {
            encoder->Drain();
        }
    }

    void Flush() {
        // Flush remaining frames
        while (true) {
            amf::AMFData* data = nullptr;
            AMF_RESULT res = encoder->QueryOutput(&data);
            if (res != AMF_OK || !data) break;
            data->Release();
        }
    }

    void Shutdown() {
        Flush();
        
        if (encoder) {
            encoder->Terminate();
            encoder = nullptr;
        }
        if (context) {
            context->Terminate();
            context = nullptr;
        }
        if (factory) {
            factory->Terminate();
            factory = nullptr;
        }
        initialized = false;
    }

private:
    amf::AMFFactory* factory;
    amf::AMFContext* context;
    amf::AMFComponent* encoder;
    bool initialized;
    int width;
    int height;
    std::vector<uint8_t> encodedBuffer;
};

extern "C" {
    __declspec(dllexport) void* Amf_Create() { return new AmfEncoder(); }
    __declspec(dllexport) bool Amf_Init(void* enc, int w, int h, int fps) { 
        return ((AmfEncoder*)enc)->Initialize(w, h, fps); 
    }
    __declspec(dllexport) bool Amf_Encode(void* enc, void* frame, void** out, size_t* size) { 
        return ((AmfEncoder*)enc)->EncodeFrame(frame, out, size); 
    }
    __declspec(dllexport) void Amf_Destroy(void* enc) {
        ((AmfEncoder*)enc)->Shutdown();
        delete (AmfEncoder*)enc;
    }
}
