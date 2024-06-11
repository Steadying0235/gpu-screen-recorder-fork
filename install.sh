#!/bin/sh -e

script_dir=$(dirname "$0")
cd "$script_dir"

[ $(id -u) -ne 0 ] && echo "You need root privileges to run the install script" && exit 1

test -d build || meson setup build
meson configure --buildtype=release build
ninja -C build install

echo "Successfully installed gpu-screen-recorder"
