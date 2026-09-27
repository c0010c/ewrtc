#!/usr/bin/env python3
"""Run the device SDK over SSH; only signaling records traverse this pipe.

Pass this executable as signaling.py --demo. Media travels directly from the
board to Chrome over UDP, not through SSH or the host signaling server.
"""
import os
import shlex
import sys
from datetime import datetime, timezone


def main():
    host = os.environ.get("EWRTC_DEVICE_HOST")
    if not host:
        raise SystemExit("Set EWRTC_DEVICE_HOST to your SSH device host (see device.env.example)")
    directory = os.environ.get("EWRTC_DEVICE_DIR", "/userdata/ewrtc")
    program = os.environ.get("EWRTC_DEVICE_PROGRAM", "ewrtc_demo")
    command = "cd /tmp && exec " + shlex.join(
        [directory + "/" + program, *sys.argv[1:]])
    if program == "ewrtc_camera":
        # This board has no persistent RTC. A cold boot at 1970 prevents DTLS
        # certificate creation. Sync over the authenticated SSH link, keeping
        # date's output off the multiplexed signaling stdout.
        utc = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S")
        command = "date -u -s " + shlex.quote(utc) + " >&2 && " + command
    ssh = ["ssh", "-T", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5",
           "-o", "ServerAliveInterval=5", "-o", "ServerAliveCountMax=2"]
    identity = os.environ.get("EWRTC_SSH_KEY")
    if identity:
        ssh += ["-i", identity]
    os.execvp("ssh", [*ssh, host, command])


if __name__ == "__main__":
    main()
