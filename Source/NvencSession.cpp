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

// Releases every native resource owned by the session.
NvencSession::~NvencSession()
{
    ReleaseResources();
}

// Opens NVENC and allocates input textures compatible with the source.
void NvencSession::Start(ID3D11Device* device, DXGI_FORMAT sourceFormat, int width, int height, int frameRate)
{
    _width = width;
    _height = height;
    _bufferFormat = ResolveBufferFormat(sourceFormat);
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

// Encodes the current input texture and returns complete HEVC packets.
std::vector<NvencSession::Packet> NvencSession::Encode(int surfaceIndex, long long timestampMicroseconds)
{
    const Surface& surface = _surfaces.at(surfaceIndex);
    NV_ENC_INPUT_PTR input = MapInput(surface);
    NV_ENC_PIC_PARAMS picture = {};
    picture.version = NV_ENC_PIC_PARAMS_VER;
    picture.inputBuffer = input;
    picture.bufferFmt = _bufferFormat;
    picture.inputWidth = _width;
    picture.inputHeight = _height;
    picture.outputBitstream = surface.bitstream;
    picture.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    picture.inputTimeStamp = static_cast<uint64_t>(timestampMicroseconds);
    NVENCSTATUS status = _api.nvEncEncodePicture(_encoder, &picture);
    if (status != NV_ENC_SUCCESS)
    {
        _api.nvEncUnmapInputResource(_encoder, input);
        Check(status, "nvEncEncodePicture");
    }

    Packet packet = ReadBitstream(surface);
    Check(_api.nvEncUnmapInputResource(_encoder, input), "nvEncUnmapInputResource");
    return { std::move(packet) };
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
    Check(_api.nvEncEncodePicture(_encoder, &end), "nvEncEncodePicture(EOS)");
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

// Applies a current high-quality HEVC configuration and initializes NVENC.
void NvencSession::InitializeEncoder(int frameRate)
{
    NV_ENC_PRESET_CONFIG preset = {};
    preset.version = NV_ENC_PRESET_CONFIG_VER;
    preset.presetCfg.version = NV_ENC_CONFIG_VER;
    Check(_api.nvEncGetEncodePresetConfigEx(_encoder, NV_ENC_CODEC_HEVC_GUID,
        NV_ENC_PRESET_P5_GUID, NV_ENC_TUNING_INFO_HIGH_QUALITY, &preset), "nvEncGetEncodePresetConfigEx");
    NV_ENC_CONFIG configuration = preset.presetCfg;
    configuration.version = NV_ENC_CONFIG_VER;
    configuration.profileGUID = NV_ENC_HEVC_PROFILE_FREXT_GUID;
    configuration.gopLength = frameRate * 2;
    configuration.frameIntervalP = 1;
    configuration.rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR;
    configuration.rcParams.averageBitRate = 160000000;
    configuration.rcParams.maxBitRate = 320000000;
    configuration.rcParams.targetQuality = 8;
    configuration.rcParams.multiPass = NV_ENC_MULTI_PASS_DISABLED;
    configuration.encodeCodecConfig.hevcConfig.chromaFormatIDC = 3;
    configuration.encodeCodecConfig.hevcConfig.inputBitDepth = NV_ENC_BIT_DEPTH_8;
    configuration.encodeCodecConfig.hevcConfig.outputBitDepth = NV_ENC_BIT_DEPTH_8;
    configuration.encodeCodecConfig.hevcConfig.repeatSPSPPS = 1;
    NV_ENC_CONFIG_HEVC_VUI_PARAMETERS& vui =
        configuration.encodeCodecConfig.hevcConfig.hevcVUIParameters;
    vui.videoSignalTypePresentFlag = 1;
    vui.videoFormat = NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED;
    vui.videoFullRangeFlag = 0;
    vui.colourDescriptionPresentFlag = 1;
    vui.colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
    vui.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
    vui.colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT709;
    NV_ENC_INITIALIZE_PARAMS initialize = {};
    initialize.version = NV_ENC_INITIALIZE_PARAMS_VER;
    initialize.encodeGUID = NV_ENC_CODEC_HEVC_GUID;
    initialize.presetGUID = NV_ENC_PRESET_P5_GUID;
    initialize.tuningInfo = NV_ENC_TUNING_INFO_HIGH_QUALITY;
    initialize.encodeWidth = _width;
    initialize.encodeHeight = _height;
    initialize.darWidth = _width;
    initialize.darHeight = _height;
    initialize.frameRateNum = frameRate;
    initialize.frameRateDen = 1;
    initialize.enablePTD = 1;
    initialize.enableEncodeAsync = 0;
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
    lock.doNotWait = 0;
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
        if (_encoder && surface.registered) _api.nvEncUnregisterResource(_encoder, surface.registered);
        if (_encoder && surface.bitstream) _api.nvEncDestroyBitstreamBuffer(_encoder, surface.bitstream);
        if (surface.texture) surface.texture->Release();
    }
    _surfaces.clear();
    if (_encoder) _api.nvEncDestroyEncoder(_encoder);
    if (_library) FreeLibrary(_library);
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
