build_mbedtls() {
    # Configure a build-local copy so public installed headers match the library,
    # while the pinned submodule and its nested framework remain untouched.
    local source_dir="$deps_build/mbedtls-source"
    # A fresh snapshot also removes files deleted by an upstream upgrade.
    cmake -E rm -rf "$source_dir"
    mkdir -p "$source_dir"
    tar -C "$root/third_party/mbedtls" --exclude=.git -cf - . | tar -C "$source_dir" -xf -
    python3 "$source_dir/scripts/config.py" -f "$source_dir/include/mbedtls/mbedtls_config.h" set MBEDTLS_SSL_DTLS_SRTP
    build_cmake_dependency mbedtls "$source_dir" \
        -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF \
        -DUSE_SHARED_MBEDTLS_LIBRARY=OFF -DUSE_STATIC_MBEDTLS_LIBRARY=ON
}
