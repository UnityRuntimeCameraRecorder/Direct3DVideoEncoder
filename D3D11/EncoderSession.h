#pragma once

#include <d3d11.h>
#include <vector>
#include <stdexcept>
#include <string>

// Defines the vendor-neutral lifecycle of one Direct3D 11 video encoder session.
class EncoderSession
{
public:
    using Packet = std::vector<unsigned char>;
    static constexpr int InputSurfaceCount = 4;
    // Returns optional encoder configuration diagnostics without vendor-specific caller logic.
    virtual std::string DiagnosticsJson() const { return "{}"; }

    // Selects a codec explicitly; placeholders only accept the original H.264 contract.
    virtual void ConfigureCodec(int codec)
    {
        if (codec != 1)
        {
            throw std::runtime_error("The selected codec is not supported by this encoder.");
        }
    }

    // Releases the concrete encoder through the common interface.
    virtual ~EncoderSession() = default;

    // Initializes the encoder and its owned input textures.
    virtual void Start(ID3D11Device* device, DXGI_FORMAT format, int width, int height, int frameRate, int preset) = 0;

    // Returns an owned input texture for a reusable frame slot.
    virtual ID3D11Texture2D* InputTexture(int surfaceIndex) const = 0;

    // Encodes one input texture and preserves its presentation timestamp.
    virtual std::vector<Packet> Encode(int surfaceIndex, long long timestampMicroseconds) = 0;

    // Indicates whether submission and output completion can run on separate workers.
    virtual bool UsesAsyncCompletion() const { return false; }

    // Submits an input surface without waiting for its encoded output.
    virtual void Submit(int, long long) { throw std::runtime_error("Asynchronous submission is not implemented by this encoder."); }

    // Waits for an asynchronous output and releases its mapped input.
    virtual std::vector<Packet> Complete(int) { throw std::runtime_error("Asynchronous completion is not implemented by this encoder."); }

    // Flushes delayed packets and releases session resources.
    virtual std::vector<Packet> Stop() = 0;
};
