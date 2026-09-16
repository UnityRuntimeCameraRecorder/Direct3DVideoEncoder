# Direct3DVideoEncoder

A native Windows DLL that encodes Direct3D 11 GPU textures into H.264 or HEVC video packets using NVIDIA NVENC.

Used by [UnityMediaRecorder](https://github.com/end3rbyte/UnityMediaRecorder) for GPU video encoding. The DLL itself does not depend on Unity or FFmpeg.

## Requirements

Windows x64, an NVIDIA GPU supporting NVENC and a recent driver. AMD and Intel backends are not implemented yet.

## Usage

Start a session, queue rendered textures with timestamps in microseconds, and dispatch encoding on the render thread. Encoded packets are delivered through a worker-thread callback.

Stop and destroy the session when finished. Keep textures and callbacks alive until encoding stops; copy callback data before returning.

For Unity integration and camera/audio/MP4 recording, use [UnityMediaRecorder](https://github.com/end3rbyte/UnityMediaRecorder).

## Build

Build `Direct3DVideoEncoder.vcxproj` with Visual Studio C++ tools (v143) and the Windows SDK, configuration `Release | x64`.

Output: `build/x64/Release/Direct3DVideoEncoder.dll`. Snapshot binaries are available under **Actions > Snapshot build**.

## License and references

Our code uses [MIT](LICENSE). The bundled NVIDIA header retains its original license notice. Codec patent rights are separate.

- [Original nvEncodeAPI.h](https://github.com/FFmpeg/nv-codec-headers/blob/eddcea9e27f6b772057c9b3f87de2cc1737faffc/include/ffnvcodec/nvEncodeAPI.h), NVENC API 13.1; stored in `Common/nvEncodeAPI.h`.
- [NVIDIA SDK programming guide](https://docs.nvidia.com/video-technologies/video-codec-sdk/13.1/nvenc-video-encoder-api-prog-guide/index.html).
- [Microsoft Direct3D 11](https://learn.microsoft.com/windows/win32/direct3d11/atoc-dx-graphics-direct3d-11).
