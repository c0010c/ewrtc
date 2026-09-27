# ewrtc

**English** | [简体中文](README.zh-CN.md)

A modular C11 WebRTC SDK for embedded devices. Stream encoded H.264 / Opus media from a device to a browser, receive media, or initiate a connection from the device.

The device SDK does not depend on Google WebRTC's C++ libraries. Platform services are injected through the Platform Abstraction Layer (PAL), and applications provide their own signaling transport. The default backends are native ICE and OpenSSL; libjuice and Mbed TLS are optional.

**Current version: 0.2.0 — experimental.** APIs may change between minor versions. Recorded validation includes local Linux x86_64 / Chrome interoperability and a Luckfox RV1103 camera example. Public Internet NAT traversal, other browsers, and additional platforms need further validation. See [support status](docs/support.md).

## Features

- One H.264 track and one Opus track, with send-only, receive-only, or bidirectional operation; local and remote offers.
- IPv4 / UDP, BUNDLE, RTCP mux, trickle ICE, and TURN/UDP.
- DTLS 1.2 and SRTP_AES128_CM_SHA1_80; video NACK / RTX / PLI.
- Explicit contexts with shared workers, bounded queues, backpressure, and session statistics.
- Separate protocol modules, platform adapters, and selectable backends.

IPv6, DataChannel, TURN/TCP, TURN/TLS, automatic ICE restart, and dynamic bitrate control are not currently supported. H.264 must use Constrained Baseline with no B-frames, and IDR access units must include SPS/PPS. Applications are responsible for audio and video encoding and decoding.

## Quick start

Run all commands from the cloned repository root. The default build requires Linux, a C11 compiler, CMake 3.20+, Make, pkg-config, OpenSSL 3.x, and libSRTP 2.x (minimum build version: 2.5). Python 3.10+ is used for architecture and development checks. Minimum build versions do not imply that every later version has been validated; see the [build guide](docs/building.md) for version records.

Install the default build dependencies on Ubuntu / Debian:

```bash
sudo apt-get update
sudo apt-get install build-essential cmake pkg-config libssl-dev libsrtp2-dev python3
cmake --preset default
cmake --build --preset default --parallel
ctest --preset default
```

The default build produces `build-default/libewrtc.a`, the Linux PAL static library, and tests. It does not require Opus, Mbed TLS, Chrome, or a libjuice download. Add `-DEWRTC_BUILD_TESTS=OFF` to the configure command to build only the libraries.

Install the SDK and run the minimal integration example:

```bash
cmake --install build-default --prefix "$PWD/install"
cmake -S examples/minimal -B build-minimal -DCMAKE_PREFIX_PATH="$PWD/install"
cmake --build build-minimal --parallel
./build-minimal/ewrtc_minimal
```

This example creates and destroys a context and a session. For actual media transport, use the browser player example below. Link an application with CMake:

```cmake
find_package(ewrtc CONFIG REQUIRED COMPONENTS session pal_linux)
target_link_libraries(my_app PRIVATE ewrtc::session ewrtc::pal_linux)
```

## Browser player example

Also install `libopus-dev`, FFmpeg with libx264, Python venv support, and Chrome:

```bash
sudo apt-get install libopus-dev ffmpeg python3-venv
python3 -m venv .venv
. .venv/bin/activate
python3 -m pip install -r examples/requirements.txt
python3 examples/run_player.py
```

Open the [local player](http://127.0.0.1:8080/player/?ws=8765) in Chrome. The script builds the demo and generates a test pattern, so no camera or external media is needed. Press Ctrl+C to stop it. See the [player guide](examples/player/README.md) for options and module details, or the [Luckfox example](examples/luckfox/README.md) for a board camera integration.

## Architecture

```text
Application: signaling / codecs / device capture
                       | Public API
                    Session --- Context / bounded task queues
                       |
             SDP / ICE / DTLS / SRTP / Media
                    |                  |
                STUN / TURN         RTP / RTCP
                       |
                 PAL service contracts
                       |
              Linux provider / custom platform
```

This diagram illustrates the layers; see the [module architecture](docs/modular-architecture.md) for exact dependencies. Protocol headers remain in `src/`; external applications use only the public interfaces in `include/`. The Linux PAL is linked separately from the SDK. Board capture adapters and browser signaling live in `examples/`.

## Documentation

Most detailed guides are currently available in Simplified Chinese. The quick-start commands above are the same as those in the Chinese README.

| Topic | Guide |
| --- | --- |
| Build options, backends, and cross-compilation | [Building](docs/building.md) |
| APIs, threading, ownership, and signaling contracts | [Integration guide](docs/usage.md), [contexts](docs/context.md) |
| Video reception, bidirectional media, and local offers | [Receiving and negotiation](docs/receiving-and-offers.md) |
| Module boundaries and platform adaptation | [Architecture](docs/modular-architecture.md), [public API](docs/public-api.md) |
| Support status and historical test evidence | [Support status](docs/support.md) |
| Development, testing, and contributing changes | [Contributing](CONTRIBUTING.md) |
| Release changes and security reports | [Changelog](CHANGELOG.md), [security policy](SECURITY.md) |

## License

ewrtc-owned code is licensed under the [MIT License](LICENSE). Third-party dependencies, optional media, and vendor software retain their own licenses; see [third-party notices](THIRD_PARTY_NOTICES.md).
