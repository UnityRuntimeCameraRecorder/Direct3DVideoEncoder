#include "EncoderSessionFactory.h"
#include "Amd/AmdEncoder.h"
#include "Intel/IntelEncoder.h"
#include "Nvidia/NvencSession.h"
#include <dxgi.h>
#include <wrl/client.h>
#include <stdexcept>

// Creates the backend matching the adapter that owns the source Direct3D device.
std::unique_ptr<EncoderSession> CreateEncoderSession(ID3D11Device* device)
{
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC description = {};
    if (!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) ||
        FAILED(dxgiDevice->GetAdapter(&adapter)) || FAILED(adapter->GetDesc(&description)))
        throw std::runtime_error("The source Direct3D adapter could not be identified.");

    switch (description.VendorId)
    {
    case 0x10DE: return std::make_unique<NvencSession>();
    case 0x1002: return std::make_unique<AmdEncoder>();
    case 0x8086: return std::make_unique<IntelEncoder>();
    default: throw std::runtime_error("The source GPU vendor has no video encoder backend.");
    }
}
