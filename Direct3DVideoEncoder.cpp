#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include "Common/CaptureSession.h"
#include "D3D11/CaptureSessionFactory.h"

namespace
{
    std::mutex registryMutex;
    std::map<int, std::shared_ptr<CaptureSession>> instances;
    std::atomic<int> nextInstanceId = 1;
    thread_local std::string exportedError;
    thread_local std::string exportedTelemetry;

    // Finds an instance while retaining it beyond the registry lock.
    std::shared_ptr<CaptureSession> FindInstance(int id)
    {
        std::lock_guard<std::mutex> lock(registryMutex);
        auto iterator = instances.find(id);
        return iterator == instances.end() ? nullptr : iterator->second;
    }

    // Dispatches a Unity render event to its instance.
    void __stdcall ProcessRenderEvent(int eventId)
    {
        auto instance = FindInstance(eventId);
        if (instance)
        {
            instance->ProcessRenderEvent();
        }
    }
}

extern "C"
{
    // Creates an independent session with an explicit codec: 1 is H.264 and 2 is HEVC.
    __declspec(dllexport) int __stdcall Direct3DVideoEncoderStartWithCodec(
        void* texture, int width, int height, int frameRate, int preset, int codec, PacketCallback callback)
    {
        try
        {
            if (codec != 1 && codec != 2)
            {
                throw std::invalid_argument("Video codec must be 1 (H.264) or 2 (HEVC).");
            }
            auto instance = CreateD3D11CaptureSession(texture, width, height, frameRate, preset, callback, codec);
            int id = nextInstanceId.fetch_add(1);
            if (id <= 0)
            {
                throw std::overflow_error("The encoder session identifier space is exhausted.");
            }
            { std::lock_guard<std::mutex> lock(registryMutex); instances.emplace(id, std::move(instance)); }
            exportedError.clear();
            return id;
        }
        catch (const std::exception& exception) { exportedError = exception.what(); return 0; }
    }

    // Creates an independent encoder and returns its positive identifier.
    __declspec(dllexport) int __stdcall Direct3DVideoEncoderStart(
        void* texture,
        int width,
        int height,
        int frameRate,
        int preset,
        PacketCallback callback)
    {
        try
        {
            auto instance = CreateD3D11CaptureSession(texture, width, height, frameRate, preset, callback);
            int id = nextInstanceId.fetch_add(1);
            if (id <= 0)
            {
                throw std::overflow_error("The encoder session identifier space is exhausted.");
            }
            { std::lock_guard<std::mutex> lock(registryMutex); instances.emplace(id, std::move(instance)); }
            exportedError.clear();
            return id;
        }
        catch (const std::exception& exception) { exportedError = exception.what(); return 0; }
    }

    // Queues a texture for one encoder instance.
    __declspec(dllexport) void __stdcall Direct3DVideoEncoderQueueTexture(int id, void* texture, long long timestamp)
    {
        auto instance = FindInstance(id);
        if (instance)
        {
            instance->QueueTexture(texture, timestamp);
        }
    }

    // Returns the shared Unity render-event dispatcher.
    __declspec(dllexport) void* __stdcall Direct3DVideoEncoderGetRenderEventFunction() { return reinterpret_cast<void*>(ProcessRenderEvent); }

    // Flushes one instance while preserving its diagnostics.
    __declspec(dllexport) void __stdcall Direct3DVideoEncoderStop(int id)
    {
        auto instance = FindInstance(id);
        if (instance)
        {
            instance->Stop();
        }
    }

    // Removes a stopped instance from the registry.
    __declspec(dllexport) void __stdcall Direct3DVideoEncoderDestroy(int id)
    {
        std::lock_guard<std::mutex> lock(registryMutex);
        instances.erase(id);
    }

    // Returns the latest instance or creation error.
    __declspec(dllexport) const char* __stdcall Direct3DVideoEncoderGetLastError(int id)
    {
        auto instance = FindInstance(id);
        if (instance)
        {
            exportedError = instance->LastError();
        }
        return exportedError.c_str();
    }

    // Returns the accepted frame count for one instance.
    __declspec(dllexport) unsigned long long __stdcall Direct3DVideoEncoderGetQueuedFrameCount(int id)
    { auto instance = FindInstance(id); return instance ? instance->Queued() : 0; }

    // Returns the encoded frame count for one instance.
    __declspec(dllexport) unsigned long long __stdcall Direct3DVideoEncoderGetEncodedFrameCount(int id)
    { auto instance = FindInstance(id); return instance ? instance->Encoded() : 0; }

    // Returns the dropped frame count for one instance.
    __declspec(dllexport) unsigned long long __stdcall Direct3DVideoEncoderGetDroppedFrameCount(int id)
    { auto instance = FindInstance(id); return instance ? instance->Dropped() : 0; }

    // Returns session telemetry while retaining the returned string until the next call on this thread.
    __declspec(dllexport) const char* __stdcall Direct3DVideoEncoderGetTelemetry(int id)
    {
        auto instance = FindInstance(id);
        exportedTelemetry = instance ? instance->Telemetry() : "{}";
        return exportedTelemetry.c_str();
    }
}
