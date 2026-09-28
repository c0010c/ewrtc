#!/usr/bin/env bash
# Dependencies come from pinned third_party submodules; see README.md.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
build=${EWRTC_LUCKFOX_BUILD:-"$root/build-luckfox"}
export LUCKFOX_TOOLCHAIN=${LUCKFOX_TOOLCHAIN:-"$build/arm-rockchip830-linux-uclibcgnueabihf"}
stage="$build/staging"
common=(-DCMAKE_TOOLCHAIN_FILE="$root/cmake/luckfox-rv1103.cmake"
        -DCMAKE_BUILD_TYPE=MinSizeRel -DCMAKE_INSTALL_PREFIX="$stage"
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5)
EWRTC_DEPS_BUILD_DIR="$build/dependencies" \
EWRTC_DEPS_TOOLCHAIN_FILE="$root/cmake/luckfox-rv1103.cmake" \
EWRTC_DEPS_BUILD_TYPE=MinSizeRel EWRTC_OPUS_FIXED_POINT=ON \
bash "$root/tools/build_dependencies.sh" "$stage" mbedtls libsrtp opus
export PKG_CONFIG_LIBDIR="$stage/lib/pkgconfig"
export PKG_CONFIG_PATH=
# Rescan the mutually dependent archives at the end of the static link.
static_libs="-Wl,--start-group $stage/lib/libmbedtls.a $stage/lib/libmbedx509.a $stage/lib/libmbedcrypto.a -Wl,--end-group -lm -lpthread"
cmake -S "$root" -B "$build/sdk" "${common[@]}" \
    -DCMAKE_FIND_ROOT_PATH="$stage" -DCMAKE_EXE_LINKER_FLAGS=-static \
    -DCMAKE_C_STANDARD_LIBRARIES="$static_libs" \
    -DEWRTC_WITH_NATIVE_ICE=ON -DEWRTC_WITH_LIBJUICE=OFF \
    -DEWRTC_WITH_OPENSSL=OFF -DEWRTC_WITH_MBEDTLS=ON -DEWRTC_BUILD_TESTS=OFF \
    -DEWRTC_BUILD_EXAMPLES=ON -DEWRTC_ENFORCE_DEPENDENCY_LOCK=ON
cmake --build "$build/sdk" --target ewrtc_demo -j8
"$LUCKFOX_TOOLCHAIN/bin/arm-rockchip830-linux-uclibcgnueabihf-strip" "$build/sdk/ewrtc_demo"
# Camera adapter is an example-level module; the SDK stays hardware independent.
"$LUCKFOX_TOOLCHAIN/bin/arm-rockchip830-linux-uclibcgnueabihf-gcc" \
    -std=c11 -D_POSIX_C_SOURCE=200809L -Os \
    -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -static \
    -I"$root/include" -I"$stage/include" \
    "$root/examples/luckfox/camera_app.c" "$root/examples/luckfox/session_hub.c" \
    "$root/examples/luckfox/mux_io.c" "$root/examples/luckfox/camera_source.c" \
    "$root/examples/luckfox/camera_h264.c" "$root/examples/luckfox/idr_control.c" \
    "$build/sdk/libewrtc.a" "$build/sdk/libewrtc_pal_linux.a" \
    -Wl,--start-group "$stage/lib/libmbedtls.a" "$stage/lib/libmbedx509.a" \
    "$stage/lib/libmbedcrypto.a" "$stage/lib/libsrtp2.a" -Wl,--end-group \
    -lm -lpthread -o "$build/sdk/ewrtc_camera"
"$LUCKFOX_TOOLCHAIN/bin/arm-rockchip830-linux-uclibcgnueabihf-strip" "$build/sdk/ewrtc_camera"
# This small module runs inside the original rkipc to access its existing VENC channel.
"$LUCKFOX_TOOLCHAIN/bin/arm-rockchip830-linux-uclibcgnueabihf-gcc" \
    -std=c11 -Os -Wall -Wextra -Werror -fPIC -shared \
    -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard \
    "$root/examples/luckfox/rkipc_idr_bridge.c" -ldl -lpthread \
    -o "$build/sdk/libewrtc_idr.so"
"$LUCKFOX_TOOLCHAIN/bin/arm-rockchip830-linux-uclibcgnueabihf-strip" "$build/sdk/libewrtc_idr.so"
