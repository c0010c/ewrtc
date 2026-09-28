find_package(PkgConfig REQUIRED)
if(EWRTC_ENFORCE_DEPENDENCY_LOCK)
  pkg_check_modules(OPUS REQUIRED IMPORTED_TARGET opus=1.6.1)
else()
  pkg_check_modules(OPUS REQUIRED IMPORTED_TARGET opus)
endif()
# pkg-config omits Libs.private for ordinary discovery. A static Opus needs
# libm after the archive; attach it to the dependency, not the executable.
set_property(TARGET PkgConfig::OPUS APPEND PROPERTY INTERFACE_LINK_LIBRARIES m)
