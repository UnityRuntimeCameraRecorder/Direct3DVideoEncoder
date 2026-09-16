#pragma once

#include "../EncoderSession.h"
#include "../../Common/NotImplementedException.h"

// Reserves the Intel oneVPL implementation behind the shared encoder interface.
class IntelEncoder final : public EncoderSession
{
public:
    // Rejects initialization until the oneVPL backend is implemented.
    void Start(ID3D11Device*, DXGI_FORMAT, int, int, int, int) override
    {
        throw NotImplementedException("Intel oneVPL video encoding is not implemented.");
    }

    // Rejects texture access because no Intel surfaces exist yet.
    ID3D11Texture2D* InputTexture(int) const override
    {
        throw NotImplementedException("Intel oneVPL input textures are not implemented.");
    }

    // Rejects encoding until the oneVPL backend is implemented.
    std::vector<Packet> Encode(int, long long) override
    {
        throw NotImplementedException("Intel oneVPL video encoding is not implemented.");
    }

    // Safely completes cleanup for an uninitialized placeholder.
    std::vector<Packet> Stop() override { return {}; }
};
