# scrcpy-camera - OBS Studio Plugin

> [!WARNING]
> **Hey there!** Just a quick heads-up: this plugin was purely "vibe-coded" in some day with the help of various AI models (GPT-5 mini, GPT-5.4 mini, GPT-5.3-Codex, Opus 4.6, Gemini 3.1 Pro, and Gemini 3.5 Flash). Because of this, the code quality isn't guaranteed and you might run into some bugs. PRs to fix issues or improve the code are more than welcome! Thanks for checking it out.

## Introduction

`scrcpy-camera` is an OBS Studio plugin that allows you to directly capture the screen or camera of an Android device using the [scrcpy](https://github.com/Genymobile/scrcpy) protocol. 

By integrating `scrcpy` directly into OBS Studio, this plugin eliminates the need to run the separate scrcpy desktop application and window-capture it. It provides a native OBS source for Android devices with low latency, hardware decoding support, and direct integration into the OBS pipeline.

> [!NOTE]
> **For Linux Users:** This plugin is generally not necessary on Linux. `scrcpy` already provides native Video4Linux (V4L2) support which allows you to use your Android device as a standard webcam. It is highly recommended to use the native V4L2 feature instead. See the [scrcpy V4L2 documentation](https://github.com/Genymobile/scrcpy/blob/master/doc/v4l2.md) for more details.

## Features

- **Direct Android Capture:** Connect to your Android device via ADB and capture its screen or camera directly in OBS.
- **Screen & Camera Modes:** Choose to mirror the device's screen or use its front/back cameras as a webcam.
- **Audio Forwarding:** Optionally capture device audio (Android 11+) with support for Opus, AAC, FLAC, and raw PCM codecs.
- **Hardware Decoding:** Utilizes FFmpeg hardware decoding (CUDA, QSV, D3D11VA, DXVA2) for optimized performance.
- **Configurable Video Settings:** Adjust video codec (H.264, H.265), bitrate, maximum resolution, and camera size right from the OBS source properties.
- **Automatic Device Discovery:** Easily select from a list of connected ADB devices.

## Showcase

![Camera and Screen with scrcpy-camera](web/1.png)
*Camera and Screen with scrcpy-camera*

![Low CPU usage with active hardware decoding while using the Camera with scrcpy-camera](web/2.png)
*Low CPU usage with active hardware decoding while using the Camera with scrcpy-camera*

## Requirements

- **OBS Studio:** Compatible with the latest versions of OBS Studio.
- **ADB (Android Debug Bridge):** Required to communicate with the Android device. You can specify a custom path to the ADB executable in the source properties.
- **scrcpy-server.jar:** The server component of `scrcpy` that runs on the Android device. This needs to be available on your system.

> [!IMPORTANT]
> This plugin is currently only compatible with **scrcpy v4.0**. You must download the exact `scrcpy-server-v4.0` file from the [scrcpy v4.0 release page](https://github.com/Genymobile/scrcpy/releases/tag/v4.0) and rename it to `scrcpy-server.jar`. Other versions are not guaranteed to work.

## Usage

1. Connect your Android device to your computer via USB (or wireless ADB) and ensure **USB debugging** is enabled in the device's Developer Options.
2. In OBS Studio, add a new source and select **scrcpy Camera**.
3. In the source properties, configure the following:
   - **ADB executable:** Path to your `adb` executable (default is `adb.exe`).
   - **ADB device:** Select your connected device from the dropdown, or click "Refresh device list" to scan.
   - **scrcpy-server.jar path:** Path to the `scrcpy-server.jar` file on your computer.
   - **Video source:** Choose between **Screen** (display mirroring) and **Camera**.
   - **Video codec:** Choose between H.264 (AVC) and H.265 (HEVC).
   - **Video bitrate:** Set the desired bitrate for the stream.
   - **Max resolution:** Limit the maximum resolution of the stream.
4. If using **Camera** mode, you can specify the **Camera ID** (e.g., `0` for Back, `1` for Front) and **Camera Size** (e.g., `1920x1080`).
5. To enable **Audio Forwarding**, check "Enable Audio (Android 11+)" and select your preferred audio codec (Opus, AAC, FLAC, or Raw PCM). You can also adjust the audio bitrate (default: 128 kbps).

> [!NOTE]
> Audio forwarding requires **Android 11 or higher**. If your device does not support the Opus encoder, try switching to AAC as a fallback.

## Building from Source

This project uses CMake. To build the plugin:

1. Ensure you have CMake, a C compiler (Visual Studio, GCC, Clang), and the OBS Studio dependencies configured.
2. Generate the build files using CMake.
3. Build the project.

```bash
mkdir build
cd build
cmake ..
cmake --build .
```

## License

This project is licensed under the GNU General Public License v2 (or later) - see the [LICENSE](LICENSE) file for details.

Copyright (C) 2026 NanKill <nankill@nankill.xyz>
