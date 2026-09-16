# Direct3DVideoEncoder

A native Windows DLL that turns Direct3D 11 GPU textures into encoded video packets. Images stay on the GPU until encoding. It does not create MP4 files, record audio or control cameras.

## Requirements

Windows x64, an NVIDIA GPU with NVENC and a recent driver. AMD and Intel backends are placeholders, not working implementations. No Unity or FFmpeg dependency is required by this DLL.

Supported formats: H.264 High or HEVC Main, 8-bit 4:2:0, limited BT.709. Current settings: 67.2 Mbit/s, GOP30, no B-frames and presets P1–P7. NVENC completion is asynchronous by default.

## Unity/BepInEx integration example

These C# declarations and snippets belong inside your Unity component or [BepInEx](https://github.com/BepInEx/BepInEx) plugin. Add `System`, `System.Runtime.InteropServices` and `UnityEngine` imports. Keep the callback in a field so garbage collection cannot remove it.

```csharp
[UnmanagedFunctionPointer(CallingConvention.StdCall)]
private delegate void PacketCallback(IntPtr data, int size, long timestampUs);
private PacketCallback _packetCallback;
private int _session;

// Starts one session: codec 1 = H.264, codec 2 = HEVC.
[DllImport("Direct3DVideoEncoder", CallingConvention = CallingConvention.StdCall)]
private static extern int Direct3DVideoEncoderStartWithCodec(
    IntPtr texture, int width, int height, int fps, int preset,
    int codec, PacketCallback callback);

// Supplies a rendered texture and its presentation timestamp.
[DllImport("Direct3DVideoEncoder", CallingConvention = CallingConvention.StdCall)]
private static extern void Direct3DVideoEncoderQueueTexture(
    int session, IntPtr texture, long timestampUs);

// Returns the render-thread dispatcher.
[DllImport("Direct3DVideoEncoder", CallingConvention = CallingConvention.StdCall)]
private static extern IntPtr Direct3DVideoEncoderGetRenderEventFunction();

// Drains encoding and releases GPU resources.
[DllImport("Direct3DVideoEncoder", CallingConvention = CallingConvention.StdCall)]
private static extern void Direct3DVideoEncoderStop(int session);

// Removes the stopped session.
[DllImport("Direct3DVideoEncoder", CallingConvention = CallingConvention.StdCall)]
private static extern void Direct3DVideoEncoderDestroy(int session);
```

Here, `writer` is your already-configured packet consumer (for example FFmpegMediaWriter with HEVC selected). `target` is a created, single-sample RGBA/BGRA RenderTexture of the requested size.

```csharp
_packetCallback = (data, size, timestampUs) =>
{
    var packet = new byte[size];
    Marshal.Copy(data, packet, 0, size);
    bool accepted = writer.WriteVideoPacket(packet, timestampUs);
    // Handle rejected packets without calling Unity APIs from this worker.
};
_session = Direct3DVideoEncoderStartWithCodec(
    target.GetNativeTexturePtr(), target.width, target.height,
    60, 5, 2, _packetCallback);
if (_session == 0)
{
    throw new InvalidOperationException("Native encoder initialization failed.");
}
```

After rendering each frame, supply a monotonic session timestamp in microseconds and schedule the matching Unity render event:

```csharp
Direct3DVideoEncoderQueueTexture(_session, target.GetNativeTexturePtr(), timestampUs);
GL.IssuePluginEvent(Direct3DVideoEncoderGetRenderEventFunction(), _session);
```

When finished, stop producing frames, let pending render events execute, then call `Direct3DVideoEncoderStop(_session)` followed by `Direct3DVideoEncoderDestroy(_session)`. Keep the source texture and callback alive until then. Each session is independent. The callback runs on a worker thread: copy its borrowed bytes before returning and preserve its timestamp.

For complete camera/audio/MP4 orchestration, use [UnityMediaRecorder](https://github.com/end3rbyte/UnityMediaRecorder). `Direct3DVideoEncoderGetTelemetry` reports configuration, frame counts, drops and stage timings after stopping, until destruction. Set `DIRECT3D_NVENC_ASYNC=0` before launch only to diagnose synchronous completion. The old start export remains compatible; new callers should select the codec explicitly.

## Build and snapshots

Build `Direct3DVideoEncoder.vcxproj` with Visual Studio C++ tools (v143) and the Windows SDK, configuration `Release | x64`. Output: `build/x64/Release/Direct3DVideoEncoder.dll`.

**Actions > Snapshot build** creates downloadable artifacts on pushes to `main`, pull requests or manual launches. The ZIP contains the DLL, import library, license notices and commit/checksum metadata, retained for 30 days. No tag or Release is created. Hosted runners compile only; runtime testing needs an NVIDIA GPU.

## License and references

Our code uses [MIT](LICENSE). The bundled header keeps its original NVIDIA notice; codec patent rights are separate. The encoder implementation is original code, not NVIDIA's C++ sample classes.

- [Original nvEncodeAPI.h](https://github.com/FFmpeg/nv-codec-headers/blob/eddcea9e27f6b772057c9b3f87de2cc1737faffc/include/ffnvcodec/nvEncodeAPI.h), NVENC API 13.1; included in `Common/nvEncodeAPI.h`.
- [NVIDIA SDK 13.1 programming guide](https://docs.nvidia.com/video-technologies/video-codec-sdk/13.1/nvenc-video-encoder-api-prog-guide/index.html).
- [Microsoft Direct3D 11](https://learn.microsoft.com/windows/win32/direct3d11/atoc-dx-graphics-direct3d-11).
