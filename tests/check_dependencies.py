#!/usr/bin/env python3
"""Check dependency sources, version metadata and option isolation without downloads."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run(*args, success=True, env=None):
    result = subprocess.run([str(arg) for arg in args], capture_output=True, text=True, env=env)
    output = result.stdout + result.stderr
    assert (result.returncode == 0) == success, output
    return output


def check_opus_without_tags(tmp):
    # Match CI's shallow submodule checkout without relying on remote access or
    # changing the developer's tags. Exercise the public dependency build entrypoint.
    checkout = tmp / "tagless-checkout"
    source = checkout / "third_party/opus"
    run("git", "init", checkout)
    run("git", "clone", "--depth", "1", "--no-tags",
        (ROOT / "third_party/opus").as_uri(), source)
    run("git", "-C", checkout, "submodule", "add",
        (ROOT / "third_party/opus").as_uri(), "third_party/opus")
    run("git", "-C", checkout, "submodule", "absorbgitdirs")
    assert (source / ".git").is_file()
    assert run("git", "-C", source, "rev-parse", "--is-shallow-repository").strip() == "true"
    assert not run("git", "-C", source, "tag", "--list").strip()
    shutil.copytree(ROOT / "tools", checkout / "tools")
    prefix = tmp / "opus-install"
    env = dict(os.environ, EWRTC_DEPS_BUILD_DIR=str(tmp / "opus-build"),
               PKG_CONFIG_LIBDIR=str(prefix / "lib/pkgconfig"), PKG_CONFIG_PATH="")
    run("bash", checkout / "tools/build_dependencies.sh", prefix, "opus", env=env)
    version = run("pkg-config", "--modversion", "opus", env=env).strip()
    assert version == "1.6.1", f"Tagless checkout produced Opus version {version}"
    # Use the same strict discovery module as the examples in verify_builds.py.
    consumer = tmp / "opus-consumer"
    consumer.mkdir()
    (consumer / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.20)\nproject(opus_consumer C)\n"
        "set(EWRTC_ENFORCE_DEPENDENCY_LOCK ON)\n"
        f'include("{ROOT / "cmake/dependencies/Opus.cmake"}")\n'
        "add_executable(consumer main.c)\n"
        "target_link_libraries(consumer PRIVATE PkgConfig::OPUS)\n")
    (consumer / "main.c").write_text(
        '#include <opus.h>\n#include <string.h>\n'
        'int main(void) { return strcmp(opus_get_version_string(), "libopus 1.6.1"); }\n')
    run("cmake", "-S", consumer, "-B", consumer / "build", env=env)
    run("cmake", "--build", consumer / "build", env=env)
    run(consumer / "build/consumer")
    assert not run("git", "-C", source, "status", "--porcelain").strip()


def main():
    with tempfile.TemporaryDirectory(prefix="ewrtc-dependencies-") as tmp:
        tmp = Path(tmp)
        check_opus_without_tags(tmp)
        options = ["-DEWRTC_COMPONENTS=ice", "-DEWRTC_WITH_NATIVE_ICE=OFF",
                   "-DEWRTC_WITH_LIBJUICE=ON", "-DEWRTC_BUILD_TESTS=OFF"]
        output = run("cmake", "-S", ROOT, "-B", tmp / "missing", *options,
                     f"-DEWRTC_LIBJUICE_SOURCE_DIR={tmp / 'absent'}", success=False)
        assert "libjuice source is missing" in output, output
        assert not (tmp / "missing/_deps").exists(), "Missing sources must not trigger a fetch"

        # A real but wrong checkout must be rejected before executing its CMake.
        wrong = tmp / "wrong-source"
        run("git", "init", wrong)
        (wrong / "CMakeLists.txt").write_text('message(FATAL_ERROR "unexpected upstream execution")\n')
        run("git", "-C", wrong, "add", "CMakeLists.txt")
        run("git", "-C", wrong, "-c", "user.name=Dependency test",
            "-c", "user.email=test@example.invalid", "-c", "commit.gpgsign=false",
            "commit", "-m", "Fixture")
        output = run("cmake", "-S", ROOT, "-B", tmp / "mismatch", *options,
                     "-DEWRTC_ENFORCE_DEPENDENCY_LOCK=ON",
                     f"-DEWRTC_LIBJUICE_SOURCE_DIR={wrong}", success=False)
        assert "libjuice must match" in output, output

        # Consumer cache values must survive upstream's generic option names.
        build = tmp / "isolated"
        run("cmake", "-S", ROOT, "-B", build, *options,
            "-DEWRTC_ENFORCE_DEPENDENCY_LOCK=ON", "-DBUILD_SHARED_LIBS=ON",
            "-DNO_SERVER=OFF", "-DNO_TESTS=OFF")
        cache = (build / "CMakeCache.txt").read_text()
        for name, value in (("BUILD_SHARED_LIBS", "ON"), ("NO_SERVER", "OFF"), ("NO_TESTS", "OFF")):
            assert f"{name}:UNINITIALIZED={value}" in cache or f"{name}:BOOL={value}" in cache, cache
        run("cmake", "--build", build, "--target", "ewrtc_ice", "--parallel", "2")

        # The legacy source override must still take precedence over the default.
        output = run("cmake", "-S", ROOT, "-B", tmp / "legacy", *options,
                     f"-DFETCHCONTENT_SOURCE_DIR_LIBJUICE={tmp / 'absent'}", success=False)
        assert "libjuice source is missing" in output, output

        # Protocol-only consumers do not need any initialized dependency source.
        run("cmake", "-S", ROOT, "-B", tmp / "protocol", "-DEWRTC_COMPONENTS=sdp",
            "-DEWRTC_BUILD_TESTS=OFF", f"-DEWRTC_LIBJUICE_SOURCE_DIR={tmp / 'absent'}")
        run("cmake", "--build", tmp / "protocol", "--parallel", "2")
    print("Dependency source, revision, tagless Opus, legacy override and option isolation checks passed")


if __name__ == "__main__":
    main()
