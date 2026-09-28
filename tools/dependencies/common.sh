# Shared by build_dependencies.sh. All build artifacts stay outside submodules.
require_source() {
    local name=$1 status
    local options=()
    # Only Mbed TLS needs nested sources to build the library. OpenSSL's nested
    # repositories are optional upstream test suites, not SDK dependencies.
    if [[ $name == mbedtls ]]; then options+=(--recursive); fi
    status=$(git -C "$root" submodule status "${options[@]}" -- "third_party/$name")
    if [[ -z $status ]] || [[ -n $(printf '%s\n' "$status" | sed -n '/^[^ ]/p') ]]; then
        echo "Missing or mismatched $name source. Run: git submodule update --init ${options[*]} third_party/$name" >&2
        return 1
    fi
}

build_cmake_dependency() {
    local name=$1 source_dir=$2
    shift 2
    local options=(-DCMAKE_BUILD_TYPE="${EWRTC_DEPS_BUILD_TYPE:-Release}"
                   -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib
                   -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON
                   -DCMAKE_POLICY_VERSION_MINIMUM=3.5)
    if [[ -n ${EWRTC_DEPS_TOOLCHAIN_FILE:-} ]]; then
        options+=(-DCMAKE_TOOLCHAIN_FILE="$EWRTC_DEPS_TOOLCHAIN_FILE")
    fi
    cmake -S "$source_dir" -B "$deps_build/$name" "${options[@]}" "$@"
    cmake --build "$deps_build/$name" --parallel "${JOBS:-2}"
    cmake --install "$deps_build/$name"
}
