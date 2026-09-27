# Changelog

## Unreleased

- Prepare the standalone repository: MIT license, contribution guidance, issue templates and CI.
- Default to native ICE + OpenSSL; disable optional backends and media examples by default.
- Pin OpenSSL to 1.1.1w, with an isolated dependency build script and a PAL-backed datagram BIO for DTLS.
- Use system versions for other dependencies by default; retain opt-in exact-version checks for Mbed TLS and libSRTP.
- Add CMake presets, modular build configuration and an installed-package minimal example.
- Repair former `sdk/` source paths; generate player test media locally.
- Limit 0.x CMake package compatibility to the same minor version.

## 0.2.0 — existing development baseline

No historical release tag or date is asserted. This is the version already declared by the project before publication preparation.

- Explicit shared context, PAL injection and bounded session queues.
- H.264 / Opus media, send/receive directions and local/remote offer workflows.
- Native/libjuice ICE and OpenSSL/Mbed TLS adapters.
- Linux player and Luckfox camera examples, component and integration validation tools.
