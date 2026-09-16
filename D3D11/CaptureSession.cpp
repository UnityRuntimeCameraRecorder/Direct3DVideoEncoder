#include <d3d11.h>
#include <d3d10.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <sstream>
#include "EncoderSessionFactory.h"
#include "CaptureSessionFactory.h"

namespace
{
    // Associates one reusable GPU surface with its source presentation timestamp.
    struct ReadyFrame { int surfaceIndex; long long timestampMicroseconds; };

    // Owns all Direct3D and vendor-specific encoding state for one independent encoder instance.
    class D3D11CaptureSession final : public CaptureSession
    {
    public:
        // Creates an inactive encoder instance.
        D3D11CaptureSession() = default;

        // Releases the session if its caller did not stop it explicitly.
        ~D3D11CaptureSession() override { Stop(); }

        // Initializes one video session against the supplied Direct3D texture.
        void Start(
            void* texturePointer,
            int width,
            int height,
            int frameRate,
            int preset,
            PacketCallback callback,
            int codec)
        {
            std::lock_guard<std::mutex> lock(_captureMutex);
            auto* texture = static_cast<ID3D11Texture2D*>(texturePointer);
            if (!texture || !callback)
            {
                throw std::invalid_argument("A source texture and packet callback are required.");
            }
            D3D11_TEXTURE2D_DESC description = {};
            texture->GetDesc(&description);
            ID3D11Device* device = nullptr;
            texture->GetDevice(&device);
            try
            {
                device->GetImmediateContext(&_context);
                if (FAILED(_context->QueryInterface(__uuidof(ID3D10Multithread), reinterpret_cast<void**>(&_multithread))))
                {
                    throw std::runtime_error("The Direct3D 11 context does not expose multithread protection.");
                }
                _multithread->SetMultithreadProtected(TRUE);
                D3D11_QUERY_DESC queryDescription = {};
                queryDescription.Query = D3D11_QUERY_EVENT;
                _copyQueries.resize(EncoderSession::InputSurfaceCount, nullptr);
                for (ID3D11Query*& query : _copyQueries)
                {
                    if (FAILED(device->CreateQuery(&queryDescription, &query)))
                    {
                        throw std::runtime_error("Direct3D could not create a texture copy synchronization query.");
                    }
                }
                _encoder = CreateEncoderSession(device);
                if (codec != 0)
                {
                    _encoder->ConfigureCodec(codec);
                }
                _encoder->Start(device, description.Format, width, height, frameRate, preset);
                _asynchronous = _encoder->UsesAsyncCompletion();
                _encoderDiagnostics = _encoder->DiagnosticsJson();
                device->Release();
                device = nullptr;
                _packetCallback = callback;
                _workerRunning = true;
                if (_asynchronous)
                {
                    _completionRunning = true;
                    _completionThread = std::thread(&D3D11CaptureSession::CompletionWorker, this);
                }
                for (int index = 0; index < EncoderSession::InputSurfaceCount; ++index)
                {
                    _freeSurfaces.push_back(index);
                }
                _encodeThread = std::thread(&D3D11CaptureSession::EncodeWorker, this);
            }
            catch (...)
            {
                if (device)
                {
                    device->Release();
                }
                throw;
            }
        }

        // Publishes the latest texture and timestamp for the render thread.
        void QueueTexture(void* texturePointer, long long timestampMicroseconds) override
        {
            std::lock_guard<std::mutex> lock(_pendingMutex);
            _pendingFrames.push_back({ texturePointer, timestampMicroseconds });
            _requested.fetch_add(1);
        }

        // Copies one queued texture into a free encoder surface.
        void ProcessRenderEvent() override
        {
            PendingFrame pending;
            {
                std::lock_guard<std::mutex> pendingLock(_pendingMutex);
                if (_pendingFrames.empty())
                {
                    return;
                }
                pending = _pendingFrames.front();
                _pendingFrames.pop_front();
            }
            std::unique_lock<std::mutex> lock(_captureMutex, std::try_to_lock);
            if (!lock.owns_lock())
            {
                _lockDrops.fetch_add(1);
                _dropped.fetch_add(1);
                return;
            }
            if (!_encoder || _failed || _freeSurfaces.empty())
            {
                _surfaceDrops.fetch_add(1);
                _dropped.fetch_add(1);
                return;
            }
            try
            {
                int index = _freeSurfaces.front();
                _freeSurfaces.pop_front();
                auto copyStart = std::chrono::steady_clock::now();
                _context->CopyResource(
                    _encoder->InputTexture(index),
                    static_cast<ID3D11Texture2D*>(pending.texture));
                _context->End(_copyQueries.at(index));
                _copyMicroseconds.fetch_add(ElapsedMicroseconds(copyStart));
                _readyFrames.push_back({ index, pending.timestampMicroseconds });
                _maximumReady.store((std::max)(_maximumReady.load(), _readyFrames.size()));
                _queued.fetch_add(1);
                _frameReady.notify_one();
            }
            catch (const std::exception& exception) { StoreError(exception); }
        }

        // Flushes the encoder and releases every instance resource.
        void Stop() override
        {
            { std::lock_guard<std::mutex> lock(_pendingMutex); _pendingFrames.clear(); }
            { std::lock_guard<std::mutex> lock(_captureMutex); _workerRunning = false; }
            _frameReady.notify_one();
            if (_encodeThread.joinable())
            {
                _encodeThread.join();
            }
            {
                std::lock_guard<std::mutex> lock(_captureMutex);
                _completionRunning = false;
            }
            _outputReady.notify_one();
            if (_completionThread.joinable())
            {
                _completionThread.join();
            }
            std::lock_guard<std::mutex> lock(_captureMutex);
            try
            {
                if (_encoder) { DeliverPackets(_encoder->Stop(), 0); _encoder.reset(); }
            }
            catch (const std::exception& exception) { StoreError(exception); }
            if (_context) { _context->Release(); _context = nullptr; }
            for (ID3D11Query* query : _copyQueries)
            {
                if (query)
                {
                    query->Release();
                }
            }
            _copyQueries.clear();
            if (_multithread) { _multithread->Release(); _multithread = nullptr; }
            _packetCallback = nullptr;
            _freeSurfaces.clear();
            _readyFrames.clear();
        }

        // Returns a stable copy of the latest instance error.
        std::string LastError() const override { std::lock_guard<std::mutex> lock(_errorMutex); return _lastError; }
        // Returns the number of accepted frames.
        unsigned long long Queued() const override { return _queued.load(); }
        // Returns the number of encoded frames.
        unsigned long long Encoded() const override { return _encoded.load(); }
        // Returns the number of dropped frames.
        unsigned long long Dropped() const override { return _dropped.load(); }

        // Reports stage timings measured on the CPU, not GPU execution timestamps.
        std::string Telemetry() const override
        {
            std::ostringstream json;
            auto submitted = _submitted.load();
            auto completed = _encoded.load();
            auto average = [](unsigned long long total, unsigned long long count)
            {
                return count ? static_cast<double>(total) / count / 1000.0 : 0.0;
            };
            json << "{\"async\":" << (_asynchronous ? "true" : "false")
                << ",\"encoder\":" << _encoderDiagnostics
                << ",\"surfaceCount\":" << EncoderSession::InputSurfaceCount
                << ",\"requested\":" << _requested.load() << ",\"copied\":" << _queued.load()
                << ",\"submitted\":" << submitted << ",\"completed\":" << completed
                << ",\"dropped\":" << _dropped.load() << ",\"lockDrops\":" << _lockDrops.load()
                << ",\"surfaceDrops\":" << _surfaceDrops.load()
                << ",\"maxReadyDepth\":" << _maximumReady.load() << ",\"maxCompletionDepth\":" << _maximumCompletion.load()
                << ",\"copyCallAverageMs\":" << average(_copyMicroseconds.load(), _queued.load())
                << ",\"gpuWaitAverageMs\":" << average(_gpuWaitMicroseconds.load(), submitted)
                << ",\"submitAverageMs\":" << average(_submitMicroseconds.load(), submitted)
                << ",\"completionAverageMs\":" << average(_completionMicroseconds.load(), completed)
                << ",\"callbackAverageMs\":" << average(_callbackMicroseconds.load(), completed)
                << ",\"failed\":" << (_failed ? "true" : "false") << "}";
            return json.str();
        }

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
        std::unique_ptr<EncoderSession> _encoder;
        ID3D11DeviceContext* _context = nullptr;
        ID3D10Multithread* _multithread = nullptr;
        std::vector<ID3D11Query*> _copyQueries;
        PacketCallback _packetCallback = nullptr;
        std::deque<PendingFrame> _pendingFrames;
        std::thread _encodeThread;
        std::thread _completionThread;
        std::condition_variable _outputReady;
        std::deque<ReadyFrame> _outputFrames;
        bool _completionRunning = false;
        bool _asynchronous = false;
        std::string _encoderDiagnostics = "{}";
        std::atomic<bool> _failed = false;
        bool _workerRunning = false;
        std::deque<int> _freeSurfaces;
        std::deque<ReadyFrame> _readyFrames;
        std::atomic<unsigned long long> _queued = 0, _encoded = 0, _dropped = 0;
        std::atomic<unsigned long long> _requested = 0, _submitted = 0, _lockDrops = 0, _surfaceDrops = 0;
        std::atomic<unsigned long long> _copyMicroseconds = 0, _gpuWaitMicroseconds = 0, _submitMicroseconds = 0;
        std::atomic<unsigned long long> _completionMicroseconds = 0, _callbackMicroseconds = 0;
        std::atomic<size_t> _maximumReady = 0, _maximumCompletion = 0;
        mutable std::mutex _errorMutex;
        std::string _lastError;

        // Converts a CPU stage interval to integer microseconds.
        static unsigned long long ElapsedMicroseconds(std::chrono::steady_clock::time_point start)
        {
            return static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start).count());
        }

        // Waits until the Direct3D texture copy has completed.
        void WaitForGpuCopy(int surfaceIndex)
        {
            BOOL completed = FALSE;
            HRESULT result = S_FALSE;
            auto start = std::chrono::steady_clock::now();
            while (result == S_FALSE)
            {
                result = _context->GetData(_copyQueries.at(surfaceIndex), &completed, sizeof(completed), 0);
                if (result == S_FALSE)
                {
                    if (std::chrono::steady_clock::now() - start > std::chrono::seconds(10))
                    {
                        throw std::runtime_error("Direct3D texture copy did not complete within ten seconds.");
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            if (FAILED(result) || !completed)
            {
                throw std::runtime_error("Direct3D did not complete the camera texture operation.");
            }
        }

        // Delivers every encoded packet to this instance's caller.
        void DeliverPackets(const std::vector<EncoderSession::Packet>& packets, long long timestamp)
        {
            if (!_packetCallback)
            {
                return;
            }
            for (const auto& packet : packets)
            {
                _packetCallback(packet.data(), static_cast<int>(packet.size()), timestamp);
            }
        }

        // Stores an exception message for this instance.
        void StoreError(const std::exception& exception)
        {
            std::lock_guard<std::mutex> lock(_errorMutex);
            _lastError = exception.what();
        }

        // Encodes copied frames away from the render thread.
        void EncodeWorker()
        {
            while (true)
            {
                std::unique_lock<std::mutex> lock(_captureMutex);
                _frameReady.wait(lock, [this] { return !_readyFrames.empty() || !_workerRunning; });
                if (_readyFrames.empty() && !_workerRunning)
                {
                    return;
                }
                ReadyFrame frame = _readyFrames.front();
                _readyFrames.pop_front();
                lock.unlock();
                try
                {
                    if (_failed)
                    {
                        continue;
                    }
                    auto stageStart = std::chrono::steady_clock::now();
                    WaitForGpuCopy(frame.surfaceIndex);
                    _gpuWaitMicroseconds.fetch_add(ElapsedMicroseconds(stageStart));
                    stageStart = std::chrono::steady_clock::now();
                    _encoder->Submit(frame.surfaceIndex, frame.timestampMicroseconds);
                    _submitMicroseconds.fetch_add(ElapsedMicroseconds(stageStart));
                    _submitted.fetch_add(1);
                    if (_asynchronous)
                    {
                        lock.lock();
                        _outputFrames.push_back(frame);
                        _maximumCompletion.store((std::max)(_maximumCompletion.load(), _outputFrames.size()));
                        _outputReady.notify_one();
                        continue;
                    }
                    CompleteFrame(frame);
                }
                catch (const std::exception& exception)
                {
                    StoreError(exception);
                    _failed = true;
                }
                lock.lock();
                _freeSurfaces.push_back(frame.surfaceIndex);
            }
        }

        // Retrieves a completed packet and measures output copy plus transport callback time.
        void CompleteFrame(const ReadyFrame& frame)
        {
            auto start = std::chrono::steady_clock::now();
            auto packets = _encoder->Complete(frame.surfaceIndex);
            _completionMicroseconds.fetch_add(ElapsedMicroseconds(start));
            _encoded.fetch_add(1);
            start = std::chrono::steady_clock::now();
            DeliverPackets(packets, frame.timestampMicroseconds);
            _callbackMicroseconds.fetch_add(ElapsedMicroseconds(start));
        }

        // Drains completed asynchronous outputs without blocking the submission worker.
        void CompletionWorker()
        {
            while (true)
            {
                std::unique_lock<std::mutex> lock(_captureMutex);
                _outputReady.wait(lock, [this] { return !_outputFrames.empty() || !_completionRunning; });
                if (_outputFrames.empty() && !_completionRunning)
                {
                    return;
                }
                ReadyFrame frame = _outputFrames.front();
                _outputFrames.pop_front();
                lock.unlock();
                try
                {
                    CompleteFrame(frame);
                }
                catch (const std::exception& exception)
                {
                    StoreError(exception);
                    _failed = true;
                }
                lock.lock();
                _freeSurfaces.push_back(frame.surfaceIndex);
            }
        }
    };

}

// Initializes a Direct3D 11 capture session behind the common lifecycle interface.
std::shared_ptr<CaptureSession> CreateD3D11CaptureSession(
    void* texture, int width, int height, int frameRate, int preset, PacketCallback callback, int codec)
{
    auto instance = std::make_shared<D3D11CaptureSession>();
    instance->Start(texture, width, height, frameRate, preset, callback, codec);
    return instance;
}
