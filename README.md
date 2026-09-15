# Landoria.D3D11NvencEncoder

Generic native Windows library for efficient GPU video encoding.

Released under the [MIT License](LICENSE).

It receives timestamped Direct3D 11 textures and encodes them as H.265/HEVC with the NVIDIA GPU through NVENC. The images remain on the GPU until they are encoded, and each output packet keeps its source presentation timestamp.

Each start call returns an independent session identifier. Texture queues, render events, worker threads, errors and frame counters are isolated per session, so several encoders can run concurrently in the same process. Stopping a session preserves its diagnostics until the caller destroys that session.

The HEVC stream declares limited-range BT.709 color primaries, transfer characteristics and matrix coefficients.

## Requirements

- Windows 64-bit
- An NVIDIA graphics card with NVENC support
- A recent NVIDIA driver
- Visual Studio 2022 C++ build tools and the Windows SDK to build the project

The NVIDIA Video Codec SDK header required to build the library is included in `ThirdParty/Nvidia`. The DLL loads the NVENC driver API supplied by the installed NVIDIA driver.

## Build

Build `Landoria.D3D11NvencEncoder.vcxproj` in `Release | x64`.

All generated files are kept under `build`. The release DLL is written to `build/x64/Release/Landoria.D3D11NvencEncoder.dll`.

The native C++ implementation is under `Source`, and third-party headers are under `ThirdParty`.

The library only encodes video. Camera control, audio capture and container creation remain the responsibility of the calling application.

## References

- [`nvEncodeAPI.h` from FFmpeg/nv-codec-headers](https://github.com/FFmpeg/nv-codec-headers/blob/eddcea9e27f6b772057c9b3f87de2cc1737faffc/include/ffnvcodec/nvEncodeAPI.h), NVENC API 13.1. The original NVIDIA copyright and permissive license are kept at the top of the file.
- [NVIDIA Video Codec SDK 13.1 documentation](https://docs.nvidia.com/video-technologies/video-codec-sdk/13.1/index.html)
- [NVENC Video Encoder API Programming Guide](https://docs.nvidia.com/video-technologies/video-codec-sdk/13.0/nvenc-video-encoder-api-prog-guide/)
- [Microsoft Direct3D 11 documentation](https://learn.microsoft.com/windows/win32/direct3d11/atoc-dx-graphics-direct3d-11)

The encoder implementation in this project is original code built directly against the NVENC C API. It does not include NVIDIA's C++ sample encoder classes.
