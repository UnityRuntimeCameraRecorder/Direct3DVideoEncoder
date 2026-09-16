#include "NvencSession.h"
#include <stdexcept>

namespace
{
    using CreateApiFunction = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
    using GetVersionFunction = NVENCSTATUS(NVENCAPI*)(unsigned int*);

    // Resolves the NVENC packed RGB format for a Direct3D source texture.
    NV_ENC_BUFFER_FORMAT ResolveBufferFormat(DXGI_FORMAT format)
    {
        switch (format)
        {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            return NV_ENC_BUFFER_FORMAT_ABGR;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return NV_ENC_BUFFER_FORMAT_ARGB;
        default:
            throw std::runtime_error("The caller supplied unsupported Direct3D texture format " +
                std::to_string(static_cast<int>(format)) + ".");
        }
    }

    // Resolves the writable UNORM Direct3D format expected by NVENC.
    DXGI_FORMAT ResolveTextureFormat(NV_ENC_BUFFER_FORMAT format)
    {
        return format == NV_ENC_BUFFER_FORMAT_ABGR
            ? DXGI_FORMAT_R8G8B8A8_UNORM
            : DXGI_FORMAT_B8G8R8A8_UNORM;
    }
}

// Releases every native resource owned by the NVIDIA session.
NvencSession::~NvencSession()
{
    ReleaseResources();
}

// Opens NVENC and allocates input textures compatible with the source.
void NvencSession::Start(ID3D11Device* device, DXGI_FORMAT sourceFormat, int width, int height, int frameRate, int preset)
{
    _width = width;
    _height = height;
    _bufferFormat = ResolveBufferFormat(sourceFormat);
    _preset = preset;
    wchar_t mode[8] = {};
    _asynchronous = !(GetEnvironmentVariableW(L"DIRECT3D_NVENC_ASYNC", mode, 8) == 1 && mode[0] == L'0');
    LoadApi();
    OpenEncoder(device);
    InitializeEncoder(frameRate);
    CreateInputSurfaces(device);
    CreateBitstreams();
}

// Returns the Direct3D texture that must receive the next camera frame.
ID3D11Texture2D* NvencSession::InputTexture(int surfaceIndex) const
{
    return _surfaces.at(surfaceIndex).texture;
}

// Encodes the current input texture and returns complete video packets.
std::vector<NvencSession::Packet> NvencSession::Encode(int surfaceIndex, long long timestampMicroseconds)
{
    Submit(surfaceIndex, timestampMicroseconds);
    return Complete(surfaceIndex);
}

// Submits a frame without waiting for NVENC completion.
void NvencSession::Submit(int surfaceIndex, long long timestampMicroseconds)
{
    Surface& surface = _surfaces.at(surfaceIndex);
    surface.mapped = MapInput(surface);
    NV_ENC_PIC_PARAMS picture = {};
    picture.version = NV_ENC_PIC_PARAMS_VER;
    picture.inputBuffer = surface.mapped;
    picture.bufferFmt = _bufferFormat;
    picture.inputWidth = _width;
    picture.inputHeight = _height;
    picture.outputBitstream = surface.bitstream;
    picture.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    picture.inputTimeStamp = static_cast<uint64_t>(timestampMicroseconds);
    picture.completionEvent = surface.completionEvent;
    NVENCSTATUS status = _api.nvEncEncodePicture(_encoder, &picture);
    if (status != NV_ENC_SUCCESS)
    {
        _api.nvEncUnmapInputResource(_encoder, surface.mapped);
        surface.mapped = nullptr;
        Check(status, "nvEncEncodePicture");
    }
}

// Retrieves an output only after its completion event has been signaled.
std::vector<NvencSession::Packet> NvencSession::Complete(int surfaceIndex)
{
    Surface& surface = _surfaces.at(surfaceIndex);
    if (_asynchronous && WaitForSingleObject(surface.completionEvent, 10000) != WAIT_OBJECT_0)
    {
        throw std::runtime_error("NVENC completion event did not signal within ten seconds.");
    }
    try
    {
        Packet packet = ReadBitstream(surface);
        Check(_api.nvEncUnmapInputResource(_encoder, surface.mapped), "nvEncUnmapInputResource");
        surface.mapped = nullptr;
        return { std::move(packet) };
    }
    catch (...)
    {
        if (surface.mapped)
        {
            _api.nvEncUnmapInputResource(_encoder, surface.mapped);
            surface.mapped = nullptr;
        }
        throw;
    }
}

// Flushes and destroys the encoder session and Direct3D resources.
std::vector<NvencSession::Packet> NvencSession::Stop()
{
    if (_encoder == nullptr)
    {
        return {};
    }

    NV_ENC_PIC_PARAMS end = {};
    end.version = NV_ENC_PIC_PARAMS_VER;
    end.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
    end.completionEvent = _asynchronous ? _surfaces[0].completionEvent : nullptr;
    Check(_api.nvEncEncodePicture(_encoder, &end), "nvEncEncodePicture(EOS)");
    if (_asynchronous && WaitForSingleObject(end.completionEvent, 10000) != WAIT_OBJECT_0)
    {
        throw std::runtime_error("NVENC end-of-stream event did not signal within ten seconds.");
    }
    ReleaseResources();
    return {};
}

// Loads the current NVENC API entry points from the NVIDIA display driver.
void NvencSession::LoadApi()
{
    _library = LoadLibraryW(L"nvEncodeAPI64.dll");
    if (_library == nullptr)
    {
        throw std::runtime_error("The NVIDIA NVENC driver library is unavailable.");
    }

    auto getVersion = reinterpret_cast<GetVersionFunction>(GetProcAddress(_library, "NvEncodeAPIGetMaxSupportedVersion"));
    auto createApi = reinterpret_cast<CreateApiFunction>(GetProcAddress(_library, "NvEncodeAPICreateInstance"));
    if (getVersion == nullptr || createApi == nullptr)
    {
        throw std::runtime_error("The NVIDIA driver does not expose the required NVENC API.");
    }

    unsigned int supportedVersion = 0;
    Check(getVersion(&supportedVersion), "NvEncodeAPIGetMaxSupportedVersion");
    constexpr unsigned int requiredVersion = (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
    if (supportedVersion < requiredVersion)
    {
        throw std::runtime_error("The NVIDIA driver is older than the required NVENC API version.");
    }

    _api.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    Check(createApi(&_api), "NvEncodeAPICreateInstance");
}

// Opens an NVENC session against the supplied Direct3D device.
void NvencSession::OpenEncoder(ID3D11Device* device)
{
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS parameters = {};
    parameters.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    parameters.device = device;
    parameters.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    parameters.apiVersion = NVENCAPI_VERSION;
    Check(_api.nvEncOpenEncodeSessionEx(&parameters, &_encoder), "nvEncOpenEncodeSessionEx");
}

// Selects H.264 or HEVC before starting this independent session.
void NvencSession::ConfigureCodec(int codec)
{
    if (codec != 1 && codec != 2)
    {
        throw std::invalid_argument("Video codec must be 1 (H.264) or 2 (HEVC).");
    }
    _codec = codec;
}

// Reports the selected codec and the fixed quality configuration for benchmark logs.
std::string NvencSession::DiagnosticsJson() const
{
    return std::string("{\"codec\":\"") + (_hevc ? "hevc" : "h264") +
        "\",\"preset\":" + std::to_string(_preset) +
        ",\"tuning\":\"high-quality\",\"bitrate\":67200000,\"gop\":30,\"bFrames\":0,\"multipass\":false}";
}

// Applies the target H.264 High or HEVC Main 4:2:0 configuration and initializes NVENC.
void NvencSession::InitializeEncoder(int frameRate)
{
    wchar_t codecMode[8] = {};
    const bool hevc = _codec == 2 || (_codec == 0 && GetEnvironmentVariableW(L"DIRECT3D_NVENC_HEVC", codecMode, 8) == 1 && codecMode[0] == L'1');
    _hevc = hevc;
    const GUID codec = hevc ? NV_ENC_CODEC_HEVC_GUID : NV_ENC_CODEC_H264_GUID;
    if (_asynchronous)
    {
        NV_ENC_CAPS_PARAM caps = {};
        caps.version = NV_ENC_CAPS_PARAM_VER;
        caps.capsToQuery = NV_ENC_CAPS_ASYNC_ENCODE_SUPPORT;
        int supported = 0;
        Check(_api.nvEncGetEncodeCaps(_encoder, codec, &caps, &supported), "nvEncGetEncodeCaps(ASYNC)");
        if (!supported)
        {
            throw std::runtime_error("This driver does not support asynchronous NVENC completion.");
        }
    }
    if (_preset < 1 || _preset > 7)
    {
        throw std::runtime_error("NVENC preset must be between P1 and P7.");
    }
    const GUID presets[] = { NV_ENC_PRESET_P1_GUID, NV_ENC_PRESET_P2_GUID, NV_ENC_PRESET_P3_GUID,
        NV_ENC_PRESET_P4_GUID, NV_ENC_PRESET_P5_GUID, NV_ENC_PRESET_P6_GUID, NV_ENC_PRESET_P7_GUID };
    const GUID presetGuid = presets[_preset - 1];
    NV_ENC_PRESET_CONFIG preset = {};
    preset.version = NV_ENC_PRESET_CONFIG_VER;
    preset.presetCfg.version = NV_ENC_CONFIG_VER;
    Check(_api.nvEncGetEncodePresetConfigEx(_encoder, codec,
        presetGuid, NV_ENC_TUNING_INFO_HIGH_QUALITY, &preset), "nvEncGetEncodePresetConfigEx");
    NV_ENC_CONFIG configuration = preset.presetCfg;
    configuration.version = NV_ENC_CONFIG_VER;
    configuration.profileGUID = hevc ? NV_ENC_HEVC_PROFILE_MAIN_GUID : NV_ENC_H264_PROFILE_HIGH_GUID;
    configuration.gopLength = 30;
    configuration.frameIntervalP = 1;
    configuration.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    configuration.rcParams.averageBitRate = 67200000;
    configuration.rcParams.maxBitRate = 67200000;
    configuration.rcParams.targetQuality = 0;
    configuration.rcParams.multiPass = NV_ENC_MULTI_PASS_DISABLED;
    if (hevc)
    {
        configuration.encodeCodecConfig.hevcConfig.chromaFormatIDC = 1;
        configuration.encodeCodecConfig.hevcConfig.inputBitDepth = NV_ENC_BIT_DEPTH_8;
        configuration.encodeCodecConfig.hevcConfig.outputBitDepth = NV_ENC_BIT_DEPTH_8;
        configuration.encodeCodecConfig.hevcConfig.idrPeriod = 30;
        configuration.encodeCodecConfig.hevcConfig.repeatSPSPPS = 1;
    }
    else
    {
        configuration.encodeCodecConfig.h264Config.chromaFormatIDC = 1;
        configuration.encodeCodecConfig.h264Config.inputBitDepth = NV_ENC_BIT_DEPTH_8;
        configuration.encodeCodecConfig.h264Config.outputBitDepth = NV_ENC_BIT_DEPTH_8;
        configuration.encodeCodecConfig.h264Config.idrPeriod = 30;
        configuration.encodeCodecConfig.h264Config.repeatSPSPPS = 1;
    }
    NV_ENC_CONFIG_H264_VUI_PARAMETERS& vui =
        hevc ? configuration.encodeCodecConfig.hevcConfig.hevcVUIParameters : configuration.encodeCodecConfig.h264Config.h264VUIParameters;
    vui.videoSignalTypePresentFlag = 1;
    vui.videoFormat = NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED;
    vui.videoFullRangeFlag = 0;
    vui.colourDescriptionPresentFlag = 1;
    vui.colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
    vui.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
    vui.colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT709;
    NV_ENC_INITIALIZE_PARAMS initialize = {};
    initialize.version = NV_ENC_INITIALIZE_PARAMS_VER;
    initialize.encodeGUID = codec;
    initialize.presetGUID = presetGuid;
    initialize.tuningInfo = NV_ENC_TUNING_INFO_HIGH_QUALITY;
    initialize.encodeWidth = _width;
    initialize.encodeHeight = _height;
    initialize.darWidth = _width;
    initialize.darHeight = _height;
    initialize.frameRateNum = frameRate;
    initialize.frameRateDen = 1;
    initialize.enablePTD = 1;
    initialize.enableEncodeAsync = _asynchronous ? 1 : 0;
    initialize.encodeConfig = &configuration;
    Check(_api.nvEncInitializeEncoder(_encoder, &initialize), "nvEncInitializeEncoder");
}

// Allocates and registers the packed RGB Direct3D input textures.
void NvencSession::CreateInputSurfaces(ID3D11Device* device)
{
    D3D11_TEXTURE2D_DESC description = {};
    description.Width = _width;
    description.Height = _height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = ResolveTextureFormat(_bufferFormat);
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_RENDER_TARGET;
    _surfaces.resize(InputSurfaceCount);
    for (Surface& surface : _surfaces)
    {
        if (FAILED(device->CreateTexture2D(&description, nullptr, &surface.texture)))
        {
            throw std::runtime_error("Direct3D could not allocate an NVENC input texture.");
        }

        NV_ENC_REGISTER_RESOURCE resource = {};
        resource.version = NV_ENC_REGISTER_RESOURCE_VER;
        resource.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
        resource.resourceToRegister = surface.texture;
        resource.width = _width;
        resource.height = _height;
        resource.bufferFormat = _bufferFormat;
        Check(_api.nvEncRegisterResource(_encoder, &resource), "nvEncRegisterResource");
        surface.registered = resource.registeredResource;
    }
}

// Allocates one encoded bitstream buffer per reusable input surface.
void NvencSession::CreateBitstreams()
{
    for (Surface& surface : _surfaces)
    {
        NV_ENC_CREATE_BITSTREAM_BUFFER buffer = {};
        buffer.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
        Check(_api.nvEncCreateBitstreamBuffer(_encoder, &buffer), "nvEncCreateBitstreamBuffer");
        surface.bitstream = buffer.bitstreamBuffer;
        if (_asynchronous)
        {
            surface.completionEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!surface.completionEvent)
            {
                throw std::runtime_error("Cannot create an NVENC completion event.");
            }
            NV_ENC_EVENT_PARAMS event = {};
            event.version = NV_ENC_EVENT_PARAMS_VER;
            event.completionEvent = surface.completionEvent;
            Check(_api.nvEncRegisterAsyncEvent(_encoder, &event), "nvEncRegisterAsyncEvent");
            surface.eventRegistered = true;
        }
    }
}

// Maps the registered input texture for one encode operation.
NV_ENC_INPUT_PTR NvencSession::MapInput(const Surface& surface)
{
    NV_ENC_MAP_INPUT_RESOURCE map = {};
    map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    map.registeredResource = surface.registered;
    Check(_api.nvEncMapInputResource(_encoder, &map), "nvEncMapInputResource");
    return map.mappedResource;
}

// Copies and unlocks the current encoded bitstream packet.
NvencSession::Packet NvencSession::ReadBitstream(const Surface& surface)
{
    NV_ENC_LOCK_BITSTREAM lock = {};
    lock.version = NV_ENC_LOCK_BITSTREAM_VER;
    lock.outputBitstream = surface.bitstream;
    lock.doNotWait = _asynchronous ? 1 : 0;
    Check(_api.nvEncLockBitstream(_encoder, &lock), "nvEncLockBitstream");
    const auto* begin = static_cast<const unsigned char*>(lock.bitstreamBufferPtr);
    Packet packet(begin, begin + lock.bitstreamSizeInBytes);
    Check(_api.nvEncUnlockBitstream(_encoder, surface.bitstream), "nvEncUnlockBitstream");
    return packet;
}

// Releases resources without attempting to emit delayed packets.
void NvencSession::ReleaseResources()
{
    for (Surface& surface : _surfaces)
    {
        if (_encoder && surface.mapped)
        {
            _api.nvEncUnmapInputResource(_encoder, surface.mapped);
        }
        if (_encoder && surface.eventRegistered)
        {
            NV_ENC_EVENT_PARAMS event = {};
            event.version = NV_ENC_EVENT_PARAMS_VER;
            event.completionEvent = surface.completionEvent;
            _api.nvEncUnregisterAsyncEvent(_encoder, &event);
        }
        if (surface.completionEvent)
        {
            CloseHandle(surface.completionEvent);
        }
        if (_encoder && surface.registered)
        {
            _api.nvEncUnregisterResource(_encoder, surface.registered);
        }
        if (_encoder && surface.bitstream)
        {
            _api.nvEncDestroyBitstreamBuffer(_encoder, surface.bitstream);
        }
        if (surface.texture)
        {
            surface.texture->Release();
        }
    }
    _surfaces.clear();
    if (_encoder)
    {
        _api.nvEncDestroyEncoder(_encoder);
    }
    if (_library)
    {
        FreeLibrary(_library);
    }
    _encoder = nullptr;
    _library = nullptr;
}

// Throws a descriptive exception when an NVENC call did not succeed.
void NvencSession::Check(NVENCSTATUS status, const char* operation)
{
    if (status != NV_ENC_SUCCESS)
    {
        throw std::runtime_error(std::string(operation) + " failed with NVENC status " + std::to_string(status) + ".");
    }
}
