#!/usr/bin/env python3
"""Check public SDK, internal component and platform boundaries."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
PLATFORM = re.compile(r"#\s*include\s*[<\"](?:pthread|poll|semaphore|unistd|fcntl|errno|netdb|ifaddrs|arpa/|netinet/|sys/|time\.h)")
CALL = re.compile(r"\b(?:pthread_\w+|getrandom|getaddrinfo|getifaddrs|clock_gettime|gettimeofday|nanosleep|sleep|select|poll|epoll_\w+|gmtime_r|strdup|strtok_r|strcasecmp|strncasecmp|malloc|calloc|realloc|free|socket|sendto|recvfrom)\s*\(")
INCLUDE = re.compile(r'#\s*include\s*[<"]([^>"]+)[>"]')
DEPS = {
    "common": set(), "pal": {"common"}, "platform": {"common", "pal"},
    "crypto": {"common"}, "sdp": {"common", "pal"}, "stun": {"common", "pal", "crypto"},
    "turn": {"common", "pal", "crypto", "stun"}, "ice": {"common", "pal", "crypto", "stun", "turn"},
    "dtls": {"common", "pal"}, "srtp": {"common", "pal"}, "rtp": {"common"},
    "media": {"common", "pal", "rtp"},
    "session": {"common", "pal", "crypto", "sdp", "ice", "dtls", "srtp", "rtp", "media"},
}
BACKENDS = {"openssl/": {"src/crypto/openssl.c", "src/dtls/openssl.c"},
            "mbedtls/": {"src/crypto/mbedtls.c", "src/dtls/mbedtls.c"},
            "juice/": {"src/ice/juice.c"}, "srtp2/": {"src/srtp/srtp.c"}}
errors = []
for path in list((ROOT / "src").rglob("*.c")) + list((ROOT / "src").rglob("*.h")) + list((ROOT / "include").rglob("*.h")):
    rel = path.relative_to(ROOT).as_posix()
    text = path.read_text()
    if rel == "src/platform/linux.c":
        continue
    for n, line in enumerate(text.splitlines(), 1):
        inc = INCLUDE.search(line)
        if inc:
            header = inc.group(1)
            if rel.startswith("src/"):
                component = path.relative_to(ROOT / "src").parts[0]
                resolved = (path.parent / header).resolve()
                if header == "ewrtc.h" and component != "session":
                    errors.append(f"{rel}:{n}: lower layer includes application API")
                dependency = None
                if "/" in header and header.split("/")[0] in DEPS:
                    dependency = header.split("/")[0]
                    if header != f"{dependency}/{dependency}.h":
                        errors.append(f"{rel}:{n}: cross-component private include: {header}")
                elif resolved.is_file() and resolved.is_relative_to(ROOT / "src") and resolved.parent != path.parent:
                    errors.append(f"{rel}:{n}: cross-component private include: {header}")
                elif header.startswith("ewrtc/"):
                    dependency = header.split("/")[1].split(".")[0]
                    if dependency == "backends":
                        dependency = "common"
                if dependency and dependency != component and dependency not in DEPS[component]:
                    errors.append(f"{rel}:{n}: forbidden component dependency: {header}")
                if "platform/" in header and component != "platform":
                    errors.append(f"{rel}:{n}: portable source depends on platform provider")
                for prefix, allowed in BACKENDS.items():
                    if header.startswith(prefix) and rel not in allowed:
                        errors.append(f"{rel}:{n}: third-party include outside adapter: {header}")
            else:
                if "platform/" not in rel and "platform/" in header:
                    errors.append(f"{rel}:{n}: portable header depends on platform provider")
                if '"' in line:
                    candidates = [(path.parent / header).resolve(), (ROOT / "include" / header).resolve()]
                    if not any(p.is_file() and p.is_relative_to(ROOT / "include") for p in candidates):
                        errors.append(f"{rel}:{n}: public header requires non-public header: {header}")
        if PLATFORM.search(line) and not (rel == "src/dtls/openssl.c" and "sys/time.h" in line):
            errors.append(f"{rel}:{n}: platform header: {line}")
        if CALL.search(line):
            errors.append(f"{rel}:{n}: direct platform/allocation call: {line}")
        if rel.startswith("src/") and not rel.startswith("src/session/") and "ewrtc_session" in line:
            errors.append(f"{rel}:{n}: lower layer depends on Session")
        if rel.startswith("include/") and re.search(r"#include [<\"](?:openssl/|mbedtls/|srtp2/|juice/)", line):
            errors.append(f"{rel}:{n}: public third-party dependency")
# An explicit allowlist prevents accidental protocol header installation.
public_headers = {"ewrtc.h", "ewrtc/common.h", "ewrtc/backends.h", "ewrtc/pal.h", "ewrtc/platform/linux.h"}
public_headers.update(f"ewrtc/pal/{name}.h" for name in ("memory", "clock", "random", "threads", "network", "events", "log"))
actual_headers = {p.relative_to(ROOT / "include").as_posix() for p in (ROOT / "include").rglob("*.h")}
if actual_headers != public_headers:
    errors.append(f"Public header set changed: {actual_headers ^ public_headers}")
assert not (ROOT / "src/internal.h").exists()
if errors:
    raise SystemExit("\n".join(errors))
print("Architecture boundaries passed")
