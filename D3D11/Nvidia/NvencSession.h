#pragma once

#include <d3d11.h>
#include <string>
#include <vector>
#include "../../Common/nvEncodeAPI.h"
#include "../EncoderSession.h"

// Owns an NVENC 13.1 session backed by reusable Direct3D 11 input surfaces.
class NvencSession final : public EncoderSession
{
public:
    // Creates an empty encoder session wrapper.
    NvencSession() = default;

    // Releases every native resource owned by the session.
    ~NvencSession() override;

    // Opens NVENC and allocates input textures compatible with the source.
    void Start(ID3D11Device* device, DXGI_FORMAT sourceFormat, int width, int height, int frameRate, int preset) override;

    // Returns the Direct3D texture that must receive the next camera frame.
    ID3D11Texture2D* InputTexture(int surfaceIndex) const override;

    // Encodes the current input texture and returns complete video packets.
    std::vector<Packet> Encode(int surfaceIndex, long long timestampMicroseconds) override;

    // Reports the explicitly requested experimental NVENC completion mode.
    bool UsesAsyncCompletion() const override { return _asynchronous; }

    // Reports the selected codec and the fixed quality configuration for benchmark logs.
    std::string DiagnosticsJson() const override;

    // Selects H.264 or HEVC before starting this independent session.
    void ConfigureCodec(int codec) override;

    // Submits one frame while retaining its input until output completion.
    void Submit(int surfaceIndex, long long timestampMicroseconds) override;

    // Waits on the registered output event and retrieves the completed packet.
    std::vector<Packet> Complete(int surfaceIndex) override;

    // Flushes and destroys the encoder session and Direct3D resources.
    std::vector<Packet> Stop() override;

private:
    // Groups the resources required to encode one independently reusable frame.
    struct Surface
    {
        ID3D11Texture2D* texture = nullptr;
        NV_ENC_REGISTERED_PTR registered = nullptr;
        NV_ENC_OUTPUT_PTR bitstream = nullptr;
        NV_ENC_INPUT_PTR mapped = nullptr;
        HANDLE completionEvent = nullptr;
        bool eventRegistered = false;
    };

    HMODULE _library = nullptr;
    NV_ENCODE_API_FUNCTION_LIST _api = {};
    void* _encoder = nullptr;
    std::vector<Surface> _surfaces;
    NV_ENC_BUFFER_FORMAT _bufferFormat = NV_ENC_BUFFER_FORMAT_UNDEFINED;
    int _width = 0;
    int _height = 0;
    int _preset = 5;
    bool _asynchronous = false;
    bool _hevc = false;
    int _codec = 0;

    // Loads the current NVENC API entry points from the NVIDIA display driver.
    void LoadApi();

    // Opens an NVENC session against the supplied Direct3D device.
    void OpenEncoder(ID3D11Device* device);

    // Applies the target H.264 High or HEVC Main 4:2:0 configuration and initializes NVENC.
    void InitializeEncoder(int frameRate);

    // Allocates and registers the reusable packed RGB Direct3D input textures.
    void CreateInputSurfaces(ID3D11Device* device);

    // Allocates one video bitstream buffer per reusable input surface.
    void CreateBitstreams();

    // Maps the registered input texture for one encode operation.
    NV_ENC_INPUT_PTR MapInput(const Surface& surface);

    // Copies and unlocks the current encoded bitstream packet.
    Packet ReadBitstream(const Surface& surface);

    // Releases resources without attempting to emit delayed packets.
    void ReleaseResources();

    // Throws a descriptive exception when an NVENC call did not succeed.
    static void Check(NVENCSTATUS status, const char* operation);
};
