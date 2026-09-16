#pragma once

#include <string>

using PacketCallback = void(__stdcall*)(const unsigned char*, int, long long);

// Defines graphics-API-independent capture control and session diagnostics.
class CaptureSession
{
public:
    // Releases the concrete capture session through the common interface.
    virtual ~CaptureSession() = default;

    // Publishes a native texture and its presentation timestamp.
    virtual void QueueTexture(void* texturePointer, long long timestampMicroseconds) = 0;

    // Processes a render-thread submission using the concrete graphics API.
    virtual void ProcessRenderEvent() = 0;

    // Drains pending frames and releases the concrete capture resources.
    virtual void Stop() = 0;

    // Returns the latest session error.
    virtual std::string LastError() const = 0;

    // Returns the number of accepted frames.
    virtual unsigned long long Queued() const = 0;

    // Returns the number of encoded frames.
    virtual unsigned long long Encoded() const = 0;

    // Returns the number of dropped frames.
    virtual unsigned long long Dropped() const = 0;

    // Returns a JSON snapshot of pipeline counters and CPU stage timings.
    virtual std::string Telemetry() const = 0;
};
