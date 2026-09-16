#pragma once

#include "../Common/CaptureSession.h"
#include <memory>

// Initializes a Direct3D 11 capture session behind the common lifecycle interface.
std::shared_ptr<CaptureSession> CreateD3D11CaptureSession(
    void* texture, int width, int height, int frameRate, int preset, PacketCallback callback, int codec = 0);
