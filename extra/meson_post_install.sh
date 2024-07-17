#!/bin/sh

/sbin/setcap cap_sys_admin+ep ${MESON_INSTALL_DESTDIR_PREFIX}/bin/gsr-kms-server \
    || echo "\n!!! Please re-run install as root\n"

# Cant do this because it breaks desktop portal (create session)!!!.
# For some reason the desktop portal tries to access /proc/gpu-screen-recorder-pid/root from the portal process
# which doesn't work because for some reason CAP_SYS_NICE on a program makes /proc/self/root not readable by other processes.
#/sbin/setcap cap_sys_nice+ep ${MESON_INSTALL_DESTDIR_PREFIX}/bin/gpu-screen-recorder
