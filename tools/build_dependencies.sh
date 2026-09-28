#!/usr/bin/env bash
# Build selected pinned sources; never download or update them during a build.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
if [[ ${1:-} == --help ]]; then
    cat <<'EOF'
Usage: bash tools/build_dependencies.sh [PREFIX [openssl libsrtp mbedtls opus ...]]
Default: .local/deps, openssl libsrtp. libjuice is built by the SDK when enabled.
Environment: JOBS, EWRTC_DEPS_BUILD_DIR, EWRTC_DEPS_BUILD_TYPE,
EWRTC_DEPS_TOOLCHAIN_FILE, EWRTC_OPUS_FIXED_POINT (ON/OFF).
Cross OpenSSL also requires OPENSSL_CONFIGURE_TARGET and CROSS_COMPILE.
Initialize selected submodules with git submodule update --init (add --recursive for mbedtls).
Use a separate build directory and prefix for every compiler/target configuration.
EOF
    exit 0
fi
prefix=${1:-"$root/.local/deps"}
if (( $# )); then shift; fi
if (( ! $# )); then set -- openssl libsrtp; fi
for dependency in "$@"; do
    case "$dependency" in
        openssl|libsrtp|mbedtls|opus) ;;
        *) echo "Unknown dependency: $dependency (see --help)" >&2; exit 2 ;;
    esac
done
mkdir -p "$prefix" "${EWRTC_DEPS_BUILD_DIR:-$root/build-deps/native}"
prefix=$(cd "$prefix" && pwd)
deps_build=$(cd "${EWRTC_DEPS_BUILD_DIR:-$root/build-deps/native}" && pwd)
source "$root/tools/dependencies/common.sh"
# Check all requested sources before starting any compilation.
for dependency in "$@"; do require_source "$dependency"; done
for dependency in "$@"; do
    source "$root/tools/dependencies/$dependency.sh"
    "build_$dependency"
done
# Bash quoting keeps paths with spaces safe when the environment file is sourced.
{
    printf 'export OPENSSL_ROOT_DIR=%q\n' "$prefix"
    printf 'export CMAKE_PREFIX_PATH=%q${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}\n' "$prefix"
    if [[ -n ${EWRTC_DEPS_TOOLCHAIN_FILE:-} ]]; then
        printf 'export PKG_CONFIG_LIBDIR=%q\nexport PKG_CONFIG_PATH=\n' "$prefix/lib/pkgconfig"
    else
        printf 'export PKG_CONFIG_PATH=%q${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}\n' "$prefix/lib/pkgconfig"
    fi
} > "$prefix/env.sh"
printf 'Dependencies installed. Load their environment with:\n  source %q\n' "$prefix/env.sh"
