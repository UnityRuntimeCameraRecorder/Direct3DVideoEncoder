#pragma once

#include "../EncoderSession.h"
#include "../../Common/NotImplementedException.h"

// Reserves the AMD AMF implementation behind the shared encoder interface.
class AmdEncoder final : public EncoderSession
{
public:
    // Rejects initialization until the AMF backend is implemented.
    void Start(ID3D11Device*, DXGI_FORMAT, int, int, int, int) override
    {
        throw NotImplementedException("AMD AMF video encoding is not implemented.");
    }

    // Rejects texture access because no AMD surfaces exist yet.
    ID3D11Texture2D* InputTexture(int) const override
    {
        throw NotImplementedException("AMD AMF input textures are not implemented.");
    }

    // Rejects encoding until the AMF backend is implemented.
    std::vector<Packet> Encode(int, long long) override
    {
        throw NotImplementedException("AMD AMF video encoding is not implemented.");
    }

    // Safely completes cleanup for an uninitialized placeholder.
    std::vector<Packet> Stop() override { return {}; }
};
