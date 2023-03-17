#!/bin/sh

script_dir=$(dirname "$0")
cd "$script_dir"

[ $(id -u) -ne 0 ] && echo "You need root privileges to run the install script" && exit 1

for xorg_conf_d in "/etc/X11/xorg.conf.d" "/usr/share/X11/xorg.conf.d" "/usr/lib/X11/xorg.conf.d"; do
    [ -d "$xorg_conf_d" ] && install -Dm644 "88-gsr-coolbits.conf" "$xorg_conf_d/88-gsr-coolbits.conf"
done