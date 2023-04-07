#!/bin/sh

script_dir=$(dirname "$0")
cd "$script_dir"

[ $(id -u) -ne 0 ] && echo "You need root privileges to run the install script" && exit 1

set -e
apt-get -y install build-essential\
	libswresample-dev libavformat-dev libavcodec-dev libavutil-dev libavfilter-dev\
	libglvnd-dev libx11-dev libxcomposite-dev libxrandr-dev\
	libpulse-dev libva2 libxnvctrl0 libnvidia-compute libnvidia-encode libnvidia-fbc1\
	libdrm-dev libcap-dev

./install.sh
