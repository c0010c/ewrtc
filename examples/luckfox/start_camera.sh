#!/bin/sh
# Invoked by RkLunch.sh after vendor kernel modules and userdata are ready.
set -eu
camera_dir=/userdata/ewrtc
test -s "$camera_dir/camera.ini"
test -s "$camera_dir/libewrtc_idr.so"
export LD_LIBRARY_PATH=/oem/usr/lib
export LD_PRELOAD="$camera_dir/libewrtc_idr.so"
exec /oem/usr/bin/rkipc -c "$camera_dir/camera.ini" -a /oem/usr/share/iqfiles \
    >/tmp/ewrtc-camera.log 2>&1 </dev/null
