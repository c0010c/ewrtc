# Third-party dependencies and media

The MIT license at the repository root applies to ewrtc-owned code. Dependencies and external assets retain their own licenses. The C/C++ libraries below are tracked as pinned Git submodules under `third_party/`. Their source trees and licenses are obtained during explicit submodule initialization; configuration and compilation do not download dependencies. Toolchains, firmware and Python packages remain separately installed. See [dependency management](third_party/README.md).

| Component | Use | Upstream license / source |
| --- | --- | --- |
| OpenSSL 1.1.1w | Default crypto and DTLS | [OpenSSL License and original SSLeay License](https://github.com/openssl/openssl/blob/OpenSSL_1_1_1w/LICENSE) |
| libSRTP 2.7.0 | SRTP | [BSD-style 3-clause license](https://github.com/cisco/libsrtp/blob/v2.7.0/LICENSE) |
| Mbed TLS 3.6.5 | Optional crypto and DTLS | [Apache-2.0 OR GPL-2.0-or-later](https://github.com/Mbed-TLS/mbedtls/blob/mbedtls-3.6.5/LICENSE) |
| libjuice | Optional ICE; commit `6d0356d092701dcce1f731559d38dc94f52eeb14` | [MPL-2.0](https://github.com/paullouisageneau/libjuice/blob/6d0356d092701dcce1f731559d38dc94f52eeb14/LICENSE) |
| Opus 1.6.1 | Demo audio codec; not required by the SDK itself | [COPYING, including patent-license references](https://github.com/xiph/opus/blob/v1.6.1/COPYING) |
| Python websockets | Example signaling and browser test tools | [BSD-3-Clause](https://github.com/python-websockets/websockets/blob/15.0.1/LICENSE) |
| Python psutil | Test process/resource sampling | [BSD-3-Clause](https://github.com/giampaolo/psutil/blob/release-7.1.0/LICENSE) |

For binary redistribution, collect the license texts and notices for the exact dependencies included in that binary. The optional libjuice backend's MPL license remains applicable even when it is linked into an MIT-licensed application. This inventory is not a replacement for upstream license texts.

## Development tools and board software

FFmpeg, Chrome and coturn are external tools used for demonstrations or verification; they are not part of the device SDK. Follow the licenses of the versions installed separately.

The Luckfox example uses a separately obtained Rockchip toolchain, vendor firmware and rkipc interfaces. These remain under their respective upstream terms and are not relicensed by this project. Firmware-specific bridge code and its interface references are described in the [Luckfox guide](examples/luckfox/README.md).

## Optional media

`test_samples/` retains historical provenance, checksums and inspection metadata. Downloaded H.264/MP4/Opus/Ogg files are excluded from Git; their redistribution rights have not been established here. The root MIT license does not cover those external media files.

The default player generates its test pattern locally with FFmpeg, so no downloaded music or video is required. To use your own authorized media, pass `--source /path/to/video.mp4` to `examples/run_player.py`.
