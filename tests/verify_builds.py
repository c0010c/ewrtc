#!/usr/bin/env python3
"""Verify backend/component builds and the installed application API boundary."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
COMPONENTS = "common pal pal_linux crypto sdp stun turn ice dtls srtp rtp media session".split()


def run(*args):
    subprocess.run([str(a) for a in args], check=True)


def check_package(build, output, components, *, session, linux):
    # Always use a clean prefix: an old install can retain removed headers/libraries.
    with tempfile.TemporaryDirectory(prefix="package-", dir=output) as tmp:
        prefix = Path(tmp) / "sdk"
        run("cmake", "--install", build, "--prefix", prefix)
        expected = {p.relative_to(ROOT / "include").as_posix() for p in (ROOT / "include").rglob("*.h")}
        if not session:
            expected.remove("ewrtc.h")
        if not linux:
            expected.remove("ewrtc/platform/linux.h")
        actual = {p.relative_to(prefix / "include").as_posix() for p in (prefix / "include").rglob("*.h")}
        assert actual == expected, (actual, expected)
        allowed_libraries = {"libewrtc.a", "libewrtc_pal_linux.a", "libjuice-static.a"}
        libraries = {p.name for p in prefix.rglob("*.a")}
        assert libraries <= allowed_libraries, libraries
        for component in components:
            consumer = Path(tmp) / component
            run("cmake", "-S", ROOT / "tests/consumer", "-B", consumer,
                f"-DCMAKE_PREFIX_PATH={prefix}", f"-DCOMPONENT={component}")
            run("cmake", "--build", consumer, "-j8")
            run(consumer / "consumer")
        rejected = subprocess.run(["cmake", "-S", str(ROOT / "tests/consumer"), "-B", str(Path(tmp) / "rejected"),
                                   f"-DCMAKE_PREFIX_PATH={prefix}", "-DCOMPONENT=stun"],
                                  capture_output=True, text=True)
        assert rejected.returncode != 0 and "ewrtc_FOUND" in rejected.stderr, rejected.stdout + rejected.stderr
        # This integration test uses only public headers and an application-owned PAL.
        if session and not linux:
            custom = Path(tmp) / "custom-consumer"
            custom.mkdir()
            (custom / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.20)\nproject(custom_pal_consumer C)\n"
                "set(CMAKE_C_STANDARD 11)\nfind_package(ewrtc CONFIG REQUIRED)\n"
                "find_package(Threads REQUIRED)\n"
                f'add_executable(custom "{ROOT / "tests/test_custom_pal.c"}")\n'
                "target_compile_options(custom PRIVATE -UNDEBUG)\n"
                "target_link_libraries(custom PRIVATE ewrtc::ewrtc Threads::Threads)\n")
            run("cmake", "-S", custom, "-B", custom / "build", f"-DCMAKE_PREFIX_PATH={prefix}")
            run("cmake", "--build", custom / "build")
            run(custom / "build/custom")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=ROOT / "build-component-validation")
    parser.add_argument("--juice-source", type=Path)
    parser.add_argument("--no-dependency-lock", action="store_true",
                        help="Relax Mbed TLS/libSRTP version locks; OpenSSL remains pinned to 1.1.1w")
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    extra = [f"-DEWRTC_ENFORCE_DEPENDENCY_LOCK={'OFF' if args.no_dependency_lock else 'ON'}",
             "-DEWRTC_WARNINGS_AS_ERRORS=ON"]
    if args.juice_source:
        extra.append(f"-DEWRTC_LIBJUICE_SOURCE_DIR={args.juice_source.resolve()}")
    for ice in ("native", "juice"):
        for tls in ("openssl", "mbedtls"):
            build = args.output / f"{ice}-{tls}"
            run("cmake", "-S", ROOT, "-B", build, "-DCMAKE_BUILD_TYPE=Release", "-DEWRTC_COMPONENTS=session",
                f"-DEWRTC_WITH_NATIVE_ICE={'ON' if ice == 'native' else 'OFF'}",
                f"-DEWRTC_WITH_LIBJUICE={'ON' if ice == 'juice' else 'OFF'}",
                f"-DEWRTC_WITH_OPENSSL={'ON' if tls == 'openssl' else 'OFF'}",
                f"-DEWRTC_WITH_MBEDTLS={'ON' if tls == 'mbedtls' else 'OFF'}",
                "-DEWRTC_WITH_LINUX_PAL=ON", "-DEWRTC_BUILD_TESTS=ON", "-DEWRTC_BUILD_EXAMPLES=ON",
                "-DEWRTC_BUILD_INTERNAL_EXAMPLES=ON", *extra)
            run("cmake", "--build", build, "-j8")
            run("ctest", "--test-dir", build, "--output-on-failure")
            check_package(build, args.output, ["session", "ewrtc", "pal_linux"], session=True, linux=True)
    for component in COMPONENTS:
        build = args.output / component
        run("cmake", "-S", ROOT, "-B", build, f"-DEWRTC_COMPONENTS={component}",
            "-DCMAKE_BUILD_TYPE=Release", "-DEWRTC_WITH_LIBJUICE=OFF", "-DEWRTC_WITH_MBEDTLS=OFF",
            "-DEWRTC_BUILD_EXAMPLES=OFF", "-DEWRTC_BUILD_TESTS=ON", *extra)
        run("cmake", "--build", build, "-j8")
        run("ctest", "--test-dir", build, "--output-on-failure")
        if component in ("session", "pal_linux"):
            check_package(build, args.output, [component], session=component == "session", linux=True)
        else:
            assert not (build / "ewrtcConfig.cmake").exists(), "Internal-only build must not publish a package"
        if component in ("common", "pal", "sdp", "rtp"):
            cache = (build / "CMakeCache.txt").read_text()
            assert not any(s in cache for s in ("OPENSSL_CRYPTO_LIBRARY:", "SRTP_LIBRARY_DIRS:", "CMAKE_HAVE_LIBC_PTHREAD:", "EWRTC_LIBJUICE_SOURCE_DIR:"))
    build = args.output / "custom-pal"
    run("cmake", "-S", ROOT, "-B", build, "-DEWRTC_COMPONENTS=session",
        "-DEWRTC_WITH_LINUX_PAL=OFF", "-DEWRTC_WITH_LIBJUICE=OFF", "-DEWRTC_WITH_MBEDTLS=OFF",
        "-DEWRTC_BUILD_EXAMPLES=OFF", "-DEWRTC_BUILD_TESTS=ON", *extra)
    run("cmake", "--build", build, "-j8")
    run("ctest", "--test-dir", build, "--output-on-failure")
    assert not (build / "libewrtc_pal_linux.a").exists()
    symbols = subprocess.check_output(["nm", "-u", str(build / "libewrtc.a")], text=True)
    assert "ewrtc_pal_linux" not in symbols and "pthread_" not in symbols
    check_package(build, args.output, ["session", "ewrtc"], session=True, linux=False)
    print("All backend, internal component, public SDK install/consumer and custom PAL checks passed")


if __name__ == "__main__":
    main()
