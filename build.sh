#!/bin/sh -e

script_dir=$(dirname "$0")
cd "$script_dir"

CC=${CC:-gcc}
CXX=${CXX:-g++}

opts="-O2 -g0 -DNDEBUG -Wall -Wextra -Wshadow"
[ -n "$DEBUG" ] && opts="-O0 -g3 -Wall -Wextra -Wshadow";

build_gsr_kms_server() {
    # TODO: -fcf-protection=full, not supported on arm
    extra_opts="-fstack-protector-all"
    dependencies="libdrm"
    includes="$(pkg-config --cflags $dependencies)"
    libs="$(pkg-config --libs $dependencies) -ldl"
    $CC -c kms/server/kms_server.c $opts $extra_opts $includes
    $CC -o gsr-kms-server kms_server.o $libs $opts $extra_opts
}

build_gsr() {
    dependencies="libavcodec libavformat libavutil x11 xcomposite xrandr xfixes libpulse libswresample libavfilter libva libcap libdrm wayland-egl wayland-client"
    includes="$(pkg-config --cflags $dependencies)"
    libs="$(pkg-config --libs $dependencies) -ldl -pthread -lm"
    $CC -c src/capture/capture.c $opts $includes
    $CC -c src/capture/nvfbc.c $opts $includes
    $CC -c src/capture/xcomposite.c $opts $includes
    $CC -c src/capture/xcomposite_cuda.c $opts $includes
    $CC -c src/capture/xcomposite_vaapi.c $opts $includes
    $CC -c src/capture/kms_vaapi.c $opts $includes
    $CC -c src/capture/kms_cuda.c $opts $includes
    $CC -c src/capture/kms.c $opts $includes
    $CC -c kms/client/kms_client.c $opts $includes
    $CC -c src/egl.c $opts $includes
    $CC -c src/cuda.c $opts $includes
    $CC -c src/xnvctrl.c $opts $includes
    $CC -c src/overclock.c $opts $includes
    $CC -c src/window_texture.c $opts $includes
    $CC -c src/shader.c $opts $includes
    $CC -c src/color_conversion.c $opts $includes
    $CC -c src/utils.c $opts $includes
    $CC -c src/library_loader.c $opts $includes
    $CC -c src/cursor.c $opts $includes
    $CXX -c src/sound.cpp $opts $includes
    $CXX -c src/main.cpp $opts $includes
    $CXX -o gpu-screen-recorder capture.o nvfbc.o kms_client.o egl.o cuda.o xnvctrl.o overclock.o window_texture.o shader.o \
        color_conversion.o utils.o library_loader.o cursor.o xcomposite.o xcomposite_cuda.o xcomposite_vaapi.o kms_vaapi.o kms_cuda.o kms.o sound.o main.o $libs $opts
}

build_gsr_kms_server
build_gsr
echo "Successfully built gpu-screen-recorder"
