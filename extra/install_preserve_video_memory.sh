#!/bin/sh

script_dir=$(dirname "$0")
cd "$script_dir"

[ $(id -u) -ne 0 ] && echo "You need root privileges to run the install script" && exit 1

install -Dm644 gsr-nvidia.conf /etc/modprobe.d/gsr-nvidia.conf
