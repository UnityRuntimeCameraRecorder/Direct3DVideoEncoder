#pragma once

#include <memory>
#include "EncoderSession.h"

// Creates the backend matching the adapter that owns the source Direct3D device.
std::unique_ptr<EncoderSession> CreateEncoderSession(ID3D11Device* device);
