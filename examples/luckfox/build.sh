#!/usr/bin/env bash
# Source archives are unpacked into build-luckfox; see README.md.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
build=${EWRTC_LUCKFOX_BUILD:-"$root/build-luckfox"}
export LUCKFOX_TOOLCHAIN=${LUCKFOX_TOOLCHAIN:-"$build/arm-rockchip830-linux-uclibcgnueabihf"}
stage="$build/staging"
common=(-DCMAKE_TOOLCHAIN_FILE="$root/cmake/luckfox-rv1103.cmake"
        -DCMAKE_BUILD_TYPE=MinSizeRel -DCMAKE_INSTALL_PREFIX="$stage"
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5)
build_dependency() {
    local source=$1 destination=$2
    shift 2
    cmake -S "$source" -B "$destination" "${common[@]}" "$@"
    cmake --build "$destination" -j8
    cmake --install "$destination"
}
python3 "$build/mbedtls-3.6.5/scripts/config.py" \
    -f "$build/mbedtls-3.6.5/include/mbedtls/mbedtls_config.h" set MBEDTLS_SSL_DTLS_SRTP
build_dependency "$build/mbedtls-3.6.5" "$build/mbedtls-release-build" \
    -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF -DUSE_SHARED_MBEDTLS_LIBRARY=OFF
build_dependency "$build/libsrtp-2.7.0" "$build/srtp-build" \
    -DLIBSRTP_TEST_APPS=OFF -DENABLE_WARNINGS_AS_ERRORS=OFF
build_dependency "$build/opus-1.6.1" "$build/opus-build" \
    -DOPUS_BUILD_TESTING=OFF -DOPUS_BUILD_PROGRAMS=OFF -DOPUS_FIXED_POINT=ON \
    -DOPUS_DNN=OFF -DOPUS_USE_NEON=OFF -DOPUS_MAY_HAVE_NEON=OFF
# libSRTP's CMake install doesn't generate its pkg-config file.
cat > "$stage/lib/pkgconfig/libsrtp2.pc" <<EOF
prefix=$stage
libdir=\${prefix}/lib
includedir=\${prefix}/include
Name: libsrtp2
Description: Secure RTP library
Version: 2.7.0
Libs: -L\${libdir} -lsrtp2
Cflags: -I\${includedir}
EOF
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
