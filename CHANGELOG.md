# Changelog

## Unreleased

- Prepare the standalone repository: MIT license, contribution guidance, issue templates and CI.
- Default to native ICE + OpenSSL; disable optional backends and media examples by default.
- Pin OpenSSL to 1.1.1w, with an isolated dependency build script and a PAL-backed datagram BIO for DTLS.
- Track OpenSSL, libSRTP, Mbed TLS, libjuice and optional Opus sources as pinned Git submodules; prepare dependencies explicitly before offline builds.
- Add modular dependency build scripts shared by native and Luckfox builds, while retaining external installations and opt-in version checks.
- Preserve static Mbed TLS and Opus link ordering and validate dependency source errors, all backends and installed consumers in CI.
- Add CMake presets, modular build configuration and an installed-package minimal example.
- Repair former `sdk/` source paths; generate player test media locally.
- Limit 0.x CMake package compatibility to the same minor version.

## 0.2.0 — existing development baseline

No historical release tag or date is asserted. This is the version already declared by the project before publication preparation.

- Explicit shared context, PAL injection and bounded session queues.
- H.264 / Opus media, send/receive directions and local/remote offer workflows.
- Native/libjuice ICE and OpenSSL/Mbed TLS adapters.
- Linux player and Luckfox camera examples, component and integration validation tools.
