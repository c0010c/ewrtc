build_opus() {
    local options=(-DOPUS_BUILD_TESTING=OFF -DOPUS_BUILD_PROGRAMS=OFF
                   -DOPUS_DEEP_PLC=OFF -DOPUS_DRED=OFF -DOPUS_OSCE=OFF
                   -DOPUS_FIXED_POINT="${EWRTC_OPUS_FIXED_POINT:-OFF}")
    if [[ ${EWRTC_OPUS_FIXED_POINT:-OFF} == ON ]]; then
        options+=(-DOPUS_DISABLE_INTRINSICS=ON)
    fi
    build_cmake_dependency opus "$root/third_party/opus" "${options[@]}"
}
