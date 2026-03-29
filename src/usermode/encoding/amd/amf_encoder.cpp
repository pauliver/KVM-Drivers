// AMD AMF encoder implementation
#include <windows.h>
#include <AMF/core/Factory.h>
#include <AMF/components/VideoEncoderVCE.h>
#include <AMF/components/Component.h>
#include <iostream>

#pragma comment(lib, "amf.lib")

class AmfEncoder {
public:
    AmfEncoder() : factory(nullptr), context(nullptr), encoder(nullptr), initialized(false) {}

    bool Initialize(int width, int height, int fps) {
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

    bool EncodeFrame(void* frameData, void** output, size_t* size) {
        if (!initialized) return false;
        // TODO: Full encode implementation
        return true;
    }

    void Shutdown() {
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
