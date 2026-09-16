#pragma once

#include <d3d11.h>
#include <vector>

// Defines the vendor-neutral lifecycle of one Direct3D 11 video encoder session.
class EncoderSession
{
public:
    using Packet = std::vector<unsigned char>;
    static constexpr int InputSurfaceCount = 3;

    // Releases the concrete encoder through the common interface.
    virtual ~EncoderSession() = default;

    // Initializes the encoder and its owned input textures.
    virtual void Start(ID3D11Device* device, DXGI_FORMAT format, int width, int height, int frameRate, int preset) = 0;

    // Returns an owned input texture for a reusable frame slot.
    virtual ID3D11Texture2D* InputTexture(int surfaceIndex) const = 0;

    // Encodes one input texture and preserves its presentation timestamp.
    virtual std::vector<Packet> Encode(int surfaceIndex, long long timestampMicroseconds) = 0;

    // Flushes delayed packets and releases session resources.
    virtual std::vector<Packet> Stop() = 0;
};
