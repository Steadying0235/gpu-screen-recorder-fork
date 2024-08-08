#!/bin/sh

# Needed to remove password prompt when recording a monitor on amd/intel or nvidia wayland
/usr/sbin/setcap cap_sys_admin+ep ${MESON_INSTALL_DESTDIR_PREFIX}/bin/gsr-kms-server \
    || echo "\n!!! Please re-run install as root\n"

# Cant do this because it breaks desktop portal (create session)!!!.
# For some reason the desktop portal tries to access /proc/gpu-screen-recorder-pid/root from the portal process
# which doesn't work because for some reason CAP_SYS_NICE on a program makes /proc/self/root not readable by other processes.
# This is needed to allow gpu screen recorder to run faster than a heavy games fps, for example when trying to record at 60 fps
# but the game drops to 45 fps in some place. That would also make gpu screen recorder drop to 45 fps unless this setcap is used.
#/usr/sbin/setcap cap_sys_nice+ep ${MESON_INSTALL_DESTDIR_PREFIX}/bin/gpu-screen-recorder
