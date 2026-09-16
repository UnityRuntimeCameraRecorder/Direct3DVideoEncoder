# Direct3DVideoEncoder

Generic native Windows library for efficient GPU video encoding.

Currently supports Direct3D 11 textures and NVIDIA NVENC only. AMD and Intel backends are placeholders.

Released under the [MIT License](LICENSE).

It receives timestamped Direct3D 11 textures and encodes them as H.264 High 8-bit 4:2:0 with the NVIDIA GPU through NVENC. The images remain on the GPU until they are encoded, and each output packet keeps its source presentation timestamp.

Each start call returns an independent session identifier. Texture queues, render events, worker threads, errors and frame counters are isolated per session, so several encoders can run concurrently in the same process. Stopping a session preserves its diagnostics until the caller destroys that session.

`EncoderSession` defines the common backend interface. The factory identifies the adapter owning the source texture: NVIDIA uses the working `NvencSession`, while `AmdEncoder` (AMF) and `IntelEncoder` (oneVPL) are placeholders. Their initialization and encoding methods throw a native `NotImplementedException`. The C export translates that exception into a failed start and a descriptive last-error message; C++ exceptions never cross the DLL boundary. No AMD or Intel SDK dependency is added yet.

The H.264 stream uses a 67.2 Mbit/s target, a 30-frame GOP without B-frames, and declares limited-range BT.709 color primaries, transfer characteristics and matrix coefficients. Callers select NVENC preset P5 for maximum quality or P4 for higher multi-session throughput.

## Requirements

- Windows 64-bit
- An NVIDIA graphics card with NVENC support
- A recent NVIDIA driver
- Visual Studio 2022 C++ build tools and the Windows SDK to build the project

The NVIDIA Video Codec SDK header required to build the library is included as `Common/nvEncodeAPI.h`. This third-party header retains its original copyright and license. The DLL loads the NVENC driver API supplied by the installed NVIDIA driver.

## Build

Build `Direct3DVideoEncoder.vcxproj` in `Release | x64`.

All generated files are kept under `build`. The release DLL is written to `build/x64/Release/Direct3DVideoEncoder.dll`.

`Direct3DVideoEncoder.cpp` exposes the C API and owns the session registry. `Common` contains graphics-independent capture control, exceptions and the original NVIDIA SDK header. `D3D11` contains texture copying, GPU synchronization, the D3D11 encoder interface and adapter selection. Vendor backends are grouped in `D3D11/Nvidia`, `D3D11/Amd` and `D3D11/Intel`.

The library only encodes video. Camera control, audio capture and container creation remain the responsibility of the calling application.

## References

- [`nvEncodeAPI.h` from FFmpeg/nv-codec-headers](https://github.com/FFmpeg/nv-codec-headers/blob/eddcea9e27f6b772057c9b3f87de2cc1737faffc/include/ffnvcodec/nvEncodeAPI.h), NVENC API 13.1. The original NVIDIA copyright and permissive license are kept at the top of the file.
- [NVIDIA Video Codec SDK 13.1 documentation](https://docs.nvidia.com/video-technologies/video-codec-sdk/13.1/index.html)
- [NVENC Video Encoder API Programming Guide](https://docs.nvidia.com/video-technologies/video-codec-sdk/13.0/nvenc-video-encoder-api-prog-guide/)
- [Microsoft Direct3D 11 documentation](https://learn.microsoft.com/windows/win32/direct3d11/atoc-dx-graphics-direct3d-11)

The encoder implementation in this project is original code built directly against the NVENC C API. It does not include NVIDIA's C++ sample encoder classes.
