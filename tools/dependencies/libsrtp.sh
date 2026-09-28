build_libsrtp() {
    build_cmake_dependency libsrtp "$root/third_party/libsrtp" \
        -DLIBSRTP_TEST_APPS=OFF -DENABLE_WARNINGS_AS_ERRORS=OFF \
        -DENABLE_OPENSSL=OFF -DENABLE_MBEDTLS=OFF -DENABLE_NSS=OFF
    # Upstream's CMake install omits pkg-config; use its actual project version.
    local version
    version=$(sed -n 's/^project(libsrtp2 VERSION \([^ ]*\).*/\1/p' "$root/third_party/libsrtp/CMakeLists.txt")
    mkdir -p "$prefix/lib/pkgconfig"
    cat > "$prefix/lib/pkgconfig/libsrtp2.pc" <<EOF
prefix=$prefix
libdir=\${prefix}/lib
includedir=\${prefix}/include
Name: libsrtp2
Description: Secure RTP library
Version: $version
Libs: -L"\${libdir}" -lsrtp2
Cflags: -I"\${includedir}"
EOF
}
