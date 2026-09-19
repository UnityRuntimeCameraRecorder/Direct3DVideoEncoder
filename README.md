# Direct3DVideoEncoder

A native Windows DLL that encodes Direct3D 11 GPU textures into H.264 or HEVC video packets using NVIDIA NVENC.

Used by [UnityRuntimeCameraRecorder](https://github.com/end3rbyte/UnityRuntimeCameraRecorder) for GPU video encoding. The DLL itself does not depend on Unity or FFmpeg.

## Requirements

Windows x64, an NVIDIA GPU supporting NVENC and a recent driver. AMD and Intel backends are not implemented yet.

## Usage

Start a session, queue rendered textures with timestamps in microseconds, and dispatch encoding on the render thread. Encoded packets are delivered through a worker-thread callback.

Stop and destroy the session when finished. Keep textures and callbacks alive until encoding stops; copy callback data before returning.

For Unity integration and camera/audio/MP4 recording, use [UnityRuntimeCameraRecorder](https://github.com/end3rbyte/UnityRuntimeCameraRecorder).

## Build

Build `Direct3DVideoEncoder.vcxproj` with Visual Studio C++ tools (v143) and the Windows SDK, configuration `Release | x64`.

Output: `build/x64/Release/Direct3DVideoEncoder.dll`. Snapshot binaries are available under **Actions > Snapshot build**.

## License and references

Our code uses [MIT](LICENSE). The bundled NVIDIA header retains its original license notice. Codec patent rights are separate.

- [Original nvEncodeAPI.h](https://github.com/FFmpeg/nv-codec-headers/blob/eddcea9e27f6b772057c9b3f87de2cc1737faffc/include/ffnvcodec/nvEncodeAPI.h), NVENC API 13.1; stored in `Common/nvEncodeAPI.h`.
- [NVIDIA SDK programming guide](https://docs.nvidia.com/video-technologies/video-codec-sdk/13.1/nvenc-video-encoder-api-prog-guide/index.html).
- [Microsoft Direct3D 11](https://learn.microsoft.com/windows/win32/direct3d11/atoc-dx-graphics-direct3d-11).

## VBR quality configuration

`Direct3DVideoEncoderStartWithQuality` adds average bitrate, maximum bitrate and CQ arguments after the codec, before the callback. Rates are in bits/s; average must be positive, maximum at least average, CQ 1–51. NVENC uses VBR, VBV equal to maximum bitrate, GOP 30, 0 B-frames, no multipass, lookahead or AQ. The caller supplies the native preset (legacy VBR profiles may use P4). Existing start exports preserve the original configuration. Final telemetry reports configured target, ceiling, CQ and rate-control mode; these are not measured file bitrates.

## SDR constant-QP quality entry point

`Direct3DVideoEncoderStartWithConstantQP(texture, width, height, fps, codec, qp, callback)` applies the following settings. Media Recorder computes QP from its quality profile; the caller does not supply P4/P5.

| NVENC setting | Implemented value |
|---|---|
| Codec / profile | 1: H.264 High; 2: HEVC Main; 8-bit 4:2:0 SDR |
| Rate control / QP | CQP; I/P/B = supplied QP, range 1-51 |
| Preset / tuning | P5 / High Quality |
| Target / maximum bitrate / VBV / CQ | 0 |
| GOP / IDR | 250 frames |
| B-frames | Up to 2, according to GPU caps; B references disabled |
| Lookahead | 8 frames if supported, otherwise 0 |
| AQ | Spatial strength 8; temporal if supported |
| Multipass / filler | Disabled |
| Colors / headers | BT.709 limited range; SPS/PPS repeated at IDR |
| Delayed output | 16 surfaces; EOS before output drain |

**HDR is not supported today.** Dimensions must be positive, even and within GPU/codec limits. One initialization retry without AQ is allowed on `NV_ENC_ERR_INVALID_PARAM` while AQ is enabled; this status alone does not establish AQ as the cause; effective settings and fallback are reported in telemetry. Callback timestamps use `outputTimeStamp`; consumers must preserve PTS/DTS when packets arrive in decode order. Media Recorder uses MPEG-TS transport. Legacy bitrate exports remain available.

Sources: [NVIDIA NVENC programming guide](https://docs.nvidia.com/video-technologies/video-codec-sdk/13.1/nvenc-video-encoder-api-prog-guide/index.html), [OBS NVENC configuration](https://github.com/obsproject/obs-studio/blob/master/plugins/obs-nvenc/nvenc.c). These document the API and reference behavior, not an exact ShadowPlay profile; disabled multipass and repeated SPS/PPS are our integration choices.
