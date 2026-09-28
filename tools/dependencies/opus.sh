build_opus() {
    # require_source already validates the pinned submodule revision. Upstream
    # derives its version from git describe, which falls back to 0 in tagless
    # shallow checkouts. Supply that revision's release version explicitly and
    # disable Git discovery so it cannot overwrite the supplied value.
    local options=(-DOPUS_PACKAGE_VERSION=1.6.1 -DCMAKE_DISABLE_FIND_PACKAGE_Git=TRUE
                   -DOPUS_BUILD_TESTING=OFF -DOPUS_BUILD_PROGRAMS=OFF
                   -DOPUS_DEEP_PLC=OFF -DOPUS_DRED=OFF -DOPUS_OSCE=OFF
                   -DOPUS_FIXED_POINT="${EWRTC_OPUS_FIXED_POINT:-OFF}")
    if [[ ${EWRTC_OPUS_FIXED_POINT:-OFF} == ON ]]; then
        options+=(-DOPUS_DISABLE_INTRINSICS=ON)
    fi
    build_cmake_dependency opus "$root/third_party/opus" "${options[@]}"
}
