#!/bin/sh -e

build_gsr_kms_server() {
    dependencies="libdrm"
    includes="$(pkg-config --cflags $dependencies)"
    libs="$(pkg-config --libs $dependencies) -ldl"
    opts="-O2 -g0 -DNDEBUG"
    gcc -c kms/server/kms_server.c $opts $includes
    gcc -o gsr-kms-server -O2 kms_server.o -s $libs
}

build_gsr() {
    dependencies="libavcodec libavformat libavutil x11 xcomposite xrandr libpulse libswresample libavfilter libva libcap"
    includes="$(pkg-config --cflags $dependencies)"
    libs="$(pkg-config --libs $dependencies) -ldl -pthread -lm"
    opts="-O2 -g0 -DNDEBUG"
    gcc -c src/capture/capture.c $opts $includes
    gcc -c src/capture/nvfbc.c $opts $includes
    gcc -c src/capture/xcomposite_cuda.c $opts $includes
    gcc -c src/capture/xcomposite_vaapi.c $opts $includes
    gcc -c src/capture/kms_vaapi.c $opts $includes
    gcc -c kms/client/kms_client.c $opts $includes
    gcc -c src/egl.c $opts $includes
    gcc -c src/cuda.c $opts $includes
    gcc -c src/xnvctrl.c $opts $includes
    gcc -c src/overclock.c $opts $includes
    gcc -c src/window_texture.c $opts $includes
    gcc -c src/utils.c $opts $includes
    gcc -c src/library_loader.c $opts $includes
    g++ -c src/sound.cpp $opts $includes
    g++ -c src/main.cpp $opts $includes
    g++ -o gpu-screen-recorder -O2 capture.o nvfbc.o kms_client.o egl.o cuda.o xnvctrl.o overclock.o window_texture.o utils.o library_loader.o xcomposite_cuda.o xcomposite_vaapi.o kms_vaapi.o sound.o main.o -s $libs
}

build_gsr_kms_server
build_gsr
echo "Successfully built gpu-screen-recorder"