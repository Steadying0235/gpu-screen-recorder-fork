#!/bin/sh

[ $(id -u) -ne 0 ] && echo "You need root privileges to run the uninstall script" && exit 1

rm -f "/usr/bin/gsr-kms-server"
rm -f "/usr/bin/gpu-screen-recorder"
rm -f "/usr/lib/systemd/user/gpu-screen-recorder.service"

echo "Successfully uninstalled gpu-screen-recorder"