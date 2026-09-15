#include <d3d11.h>
#include <d3d10.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "NvencSession.h"

namespace
{
    using PacketCallback = void(__stdcall*)(const unsigned char*, int, long long);

    // Associates one reusable GPU surface with its source presentation timestamp.
    struct ReadyFrame { int surfaceIndex; long long timestampMicroseconds; };

    // Owns all Direct3D and NVENC state for one independent encoder instance.
    class EncoderInstance final
    {
    public:
        // Creates an inactive encoder instance.
        EncoderInstance() = default;

        // Releases the session if its caller did not stop it explicitly.
        ~EncoderInstance() { Stop(); }

        // Initializes one H.264 session against the supplied Direct3D texture.
        void Start(void* texturePointer, int width, int height, int frameRate, PacketCallback callback)
        {
            std::lock_guard<std::mutex> lock(_captureMutex);
            auto* texture = static_cast<ID3D11Texture2D*>(texturePointer);
            if (!texture || !callback) throw std::invalid_argument("A source texture and packet callback are required.");
            D3D11_TEXTURE2D_DESC description = {};
            texture->GetDesc(&description);
            ID3D11Device* device = nullptr;
            texture->GetDevice(&device);
            try
            {
                device->GetImmediateContext(&_context);
                if (FAILED(_context->QueryInterface(__uuidof(ID3D10Multithread), reinterpret_cast<void**>(&_multithread))))
                    throw std::runtime_error("The Direct3D 11 context does not expose multithread protection.");
                _multithread->SetMultithreadProtected(TRUE);
                D3D11_QUERY_DESC queryDescription = {};
                queryDescription.Query = D3D11_QUERY_EVENT;
                _copyQueries.resize(NvencSession::InputSurfaceCount, nullptr);
                for (ID3D11Query*& query : _copyQueries)
                    if (FAILED(device->CreateQuery(&queryDescription, &query)))
                        throw std::runtime_error("Direct3D could not create a texture copy synchronization query.");
                _encoder.reset(new NvencSession());
                _encoder->Start(device, description.Format, width, height, frameRate);
                device->Release();
                device = nullptr;
                _packetCallback = callback;
                _workerRunning = true;
                for (int index = 0; index < NvencSession::InputSurfaceCount; ++index) _freeSurfaces.push_back(index);
                _encodeThread = std::thread(&EncoderInstance::EncodeWorker, this);
            }
            catch (...)
            {
                if (device) device->Release();
                throw;
            }
        }

        // Publishes the latest texture and timestamp for the render thread.
        void QueueTexture(void* texturePointer, long long timestampMicroseconds)
        {
            std::lock_guard<std::mutex> lock(_pendingMutex);
            _pendingFrames.push_back({ texturePointer, timestampMicroseconds });
        }

        // Copies one queued texture into a free NVENC surface.
        void ProcessRenderEvent()
        {
            PendingFrame pending;
            {
                std::lock_guard<std::mutex> pendingLock(_pendingMutex);
                if (_pendingFrames.empty()) return;
                pending = _pendingFrames.front();
                _pendingFrames.pop_front();
            }
            std::unique_lock<std::mutex> lock(_captureMutex, std::try_to_lock);
            if (!lock.owns_lock() || !_encoder || _freeSurfaces.empty()) { _dropped.fetch_add(1); return; }
            try
            {
                int index = _freeSurfaces.front();
                _freeSurfaces.pop_front();
                _context->CopyResource(
                    _encoder->InputTexture(index),
                    static_cast<ID3D11Texture2D*>(pending.texture));
                _context->End(_copyQueries.at(index));
                _readyFrames.push_back({ index, pending.timestampMicroseconds });
                _queued.fetch_add(1);
                _frameReady.notify_one();
            }
            catch (const std::exception& exception) { StoreError(exception); }
        }

        // Flushes the encoder and releases every instance resource.
        void Stop()
        {
            { std::lock_guard<std::mutex> lock(_pendingMutex); _pendingFrames.clear(); }
            { std::lock_guard<std::mutex> lock(_captureMutex); _workerRunning = false; }
            _frameReady.notify_one();
            if (_encodeThread.joinable()) _encodeThread.join();
            std::lock_guard<std::mutex> lock(_captureMutex);
            try
            {
                if (_encoder) { DeliverPackets(_encoder->Stop(), 0); _encoder.reset(); }
            }
            catch (const std::exception& exception) { StoreError(exception); }
            if (_context) { _context->Release(); _context = nullptr; }
            for (ID3D11Query* query : _copyQueries) if (query) query->Release();
            _copyQueries.clear();
            if (_multithread) { _multithread->Release(); _multithread = nullptr; }
            _packetCallback = nullptr;
            _freeSurfaces.clear();
            _readyFrames.clear();
        }

        // Returns a stable copy of the latest instance error.
        std::string LastError() const { std::lock_guard<std::mutex> lock(_errorMutex); return _lastError; }
        // Returns the number of accepted frames.
        unsigned long long Queued() const { return _queued.load(); }
        // Returns the number of encoded frames.
        unsigned long long Encoded() const { return _encoded.load(); }
        // Returns the number of dropped frames.
        unsigned long long Dropped() const { return _dropped.load(); }

    private:
        // Stores one texture submission until its matching Unity render event executes.
        struct PendingFrame
        {
            void* texture = nullptr;
            long long timestampMicroseconds = 0;
        };

        std::mutex _captureMutex;
        std::mutex _pendingMutex;
        std::condition_variable _frameReady;
        std::unique_ptr<NvencSession> _encoder;
        ID3D11DeviceContext* _context = nullptr;
        ID3D10Multithread* _multithread = nullptr;
        std::vector<ID3D11Query*> _copyQueries;
        PacketCallback _packetCallback = nullptr;
        std::deque<PendingFrame> _pendingFrames;
        std::thread _encodeThread;
        bool _workerRunning = false;
        std::deque<int> _freeSurfaces;
        std::deque<ReadyFrame> _readyFrames;
        std::atomic<unsigned long long> _queued = 0, _encoded = 0, _dropped = 0;
        mutable std::mutex _errorMutex;
        std::string _lastError;

        // Delivers every encoded packet to this instance's caller.
        void DeliverPackets(const std::vector<NvencSession::Packet>& packets, long long timestamp)
        {
            if (!_packetCallback) return;
            for (const auto& packet : packets) _packetCallback(packet.data(), static_cast<int>(packet.size()), timestamp);
        }

        // Stores an exception message for this instance.
        void StoreError(const std::exception& exception)
        {
            std::lock_guard<std::mutex> lock(_errorMutex);
            _lastError = exception.what();
        }

        // Waits for one asynchronous Direct3D texture copy.
        void WaitForGpuCopy(int surfaceIndex)
        {
            BOOL completed = FALSE;
            HRESULT result = S_FALSE;
            while (result == S_FALSE)
            {
                result = _context->GetData(_copyQueries.at(surfaceIndex), &completed, sizeof(completed), 0);
                if (result == S_FALSE) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (FAILED(result) || !completed) throw std::runtime_error("Direct3D did not complete the camera texture copy.");
        }

        // Encodes copied frames away from the render thread.
        void EncodeWorker()
        {
            while (true)
            {
                std::unique_lock<std::mutex> lock(_captureMutex);
                _frameReady.wait(lock, [this] { return !_readyFrames.empty() || !_workerRunning; });
                if (_readyFrames.empty() && !_workerRunning) return;
                ReadyFrame frame = _readyFrames.front();
                _readyFrames.pop_front();
                lock.unlock();
                try
                {
                    WaitForGpuCopy(frame.surfaceIndex);
                    DeliverPackets(_encoder->Encode(frame.surfaceIndex, frame.timestampMicroseconds), frame.timestampMicroseconds);
                    _encoded.fetch_add(1);
                }
                catch (const std::exception& exception) { StoreError(exception); }
                lock.lock();
                _freeSurfaces.push_back(frame.surfaceIndex);
            }
        }
    };

    std::mutex registryMutex;
    std::map<int, std::shared_ptr<EncoderInstance>> instances;
    std::atomic<int> nextInstanceId = 1;
    thread_local std::string exportedError;

    // Finds an instance while retaining it beyond the registry lock.
    std::shared_ptr<EncoderInstance> FindInstance(int id)
    {
        std::lock_guard<std::mutex> lock(registryMutex);
        auto iterator = instances.find(id);
        return iterator == instances.end() ? nullptr : iterator->second;
    }

    // Dispatches a Unity render event to its instance.
    void __stdcall ProcessRenderEvent(int eventId)
    {
        auto instance = FindInstance(eventId);
        if (instance) instance->ProcessRenderEvent();
    }
}

extern "C"
{
    // Creates an independent encoder and returns its positive identifier.
    __declspec(dllexport) int __stdcall D3D11NvencEncoderStart(void* texture, int width, int height, int frameRate, PacketCallback callback)
    {
        try
        {
            auto instance = std::make_shared<EncoderInstance>();
            instance->Start(texture, width, height, frameRate, callback);
            int id = nextInstanceId.fetch_add(1);
            if (id <= 0) throw std::overflow_error("The encoder session identifier space is exhausted.");
            { std::lock_guard<std::mutex> lock(registryMutex); instances.emplace(id, std::move(instance)); }
            exportedError.clear();
            return id;
        }
        catch (const std::exception& exception) { exportedError = exception.what(); return 0; }
    }

    // Queues a texture for one encoder instance.
    __declspec(dllexport) void __stdcall D3D11NvencEncoderQueueTexture(int id, void* texture, long long timestamp)
    {
        auto instance = FindInstance(id);
        if (instance) instance->QueueTexture(texture, timestamp);
    }

    // Returns the shared Unity render-event dispatcher.
    __declspec(dllexport) void* __stdcall D3D11NvencEncoderGetRenderEventFunction() { return reinterpret_cast<void*>(ProcessRenderEvent); }

    // Flushes one instance while preserving its diagnostics.
    __declspec(dllexport) void __stdcall D3D11NvencEncoderStop(int id)
    {
        auto instance = FindInstance(id);
        if (instance) instance->Stop();
    }

    // Removes a stopped instance from the registry.
    __declspec(dllexport) void __stdcall D3D11NvencEncoderDestroy(int id)
    {
        std::lock_guard<std::mutex> lock(registryMutex);
        instances.erase(id);
    }

    // Returns the latest instance or creation error.
    __declspec(dllexport) const char* __stdcall D3D11NvencEncoderGetLastError(int id)
    {
        auto instance = FindInstance(id);
        if (instance) exportedError = instance->LastError();
        return exportedError.c_str();
    }

    // Returns the accepted frame count for one instance.
    __declspec(dllexport) unsigned long long __stdcall D3D11NvencEncoderGetQueuedFrameCount(int id)
    { auto instance = FindInstance(id); return instance ? instance->Queued() : 0; }

    // Returns the encoded frame count for one instance.
    __declspec(dllexport) unsigned long long __stdcall D3D11NvencEncoderGetEncodedFrameCount(int id)
    { auto instance = FindInstance(id); return instance ? instance->Encoded() : 0; }

    // Returns the dropped frame count for one instance.
    __declspec(dllexport) unsigned long long __stdcall D3D11NvencEncoderGetDroppedFrameCount(int id)
    { auto instance = FindInstance(id); return instance ? instance->Dropped() : 0; }
}
