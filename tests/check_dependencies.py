#!/usr/bin/env python3
"""Check local-source failures and libjuice option isolation without downloading."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run(*args, success=True):
    result = subprocess.run([str(arg) for arg in args], capture_output=True, text=True)
    output = result.stdout + result.stderr
    assert (result.returncode == 0) == success, output
    return output


def main():
    with tempfile.TemporaryDirectory(prefix="ewrtc-dependencies-") as tmp:
        tmp = Path(tmp)
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
    print("Dependency source, revision, legacy override and option isolation checks passed")


if __name__ == "__main__":
    main()
