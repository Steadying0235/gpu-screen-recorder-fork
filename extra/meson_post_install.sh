#!/bin/sh

setcap cap_sys_admin+ep ${MESON_INSTALL_DESTDIR_PREFIX}/bin/gsr-kms-server \
    || echo "\n!!! Please re-run install as root\n"
setcap cap_sys_nice+ep ${MESON_INSTALL_DESTDIR_PREFIX}/bin/gpu-screen-recorder
