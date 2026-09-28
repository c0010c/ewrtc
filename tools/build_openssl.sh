#!/usr/bin/env bash
# Compatibility entry point; the source now comes from the pinned submodule.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export EWRTC_DEPS_BUILD_DIR=${EWRTC_DEPS_BUILD_DIR:-"$root/build-deps/openssl-only"}
exec bash "$root/tools/build_dependencies.sh" "${1:-$root/.local/openssl-1.1.1w}" openssl
