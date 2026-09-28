build_openssl() (
    if [[ -n ${EWRTC_DEPS_TOOLCHAIN_FILE:-} && ( -z ${OPENSSL_CONFIGURE_TARGET:-} || -z ${CROSS_COMPILE:-} ) ]]; then
        echo 'Cross OpenSSL requires OPENSSL_CONFIGURE_TARGET and CROSS_COMPILE; CMake toolchains do not configure OpenSSL.' >&2
        exit 2
    fi
    mkdir -p "$deps_build/openssl"
    cd "$deps_build/openssl"
    if [[ -n ${OPENSSL_CONFIGURE_TARGET:-} ]]; then
        perl "$root/third_party/openssl/Configure" "$OPENSSL_CONFIGURE_TARGET" \
            --prefix="$prefix" --openssldir="$prefix/ssl" --libdir=lib no-shared
    else
        "$root/third_party/openssl/config" --prefix="$prefix" --openssldir="$prefix/ssl" --libdir=lib no-shared
    fi
    make -j"${JOBS:-2}"
    make install_sw
)
