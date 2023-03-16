#!/bin/sh -e

#libdrm
dependencies="libavcodec libavformat libavutil x11 xcomposite xrandr libpulse libswresample libavfilter"
includes="$(pkg-config --cflags $dependencies)"
libs="$(pkg-config --libs $dependencies) -ldl -pthread -lm"
opts="-O2 -g0 -DNDEBUG"
gcc -c src/capture/capture.c $opts $includes
gcc -c src/capture/nvfbc.c $opts $includes
gcc -c src/capture/xcomposite_cuda.c $opts $includes
gcc -c src/capture/xcomposite_drm.c $opts $includes
gcc -c src/egl.c $opts $includes
gcc -c src/cuda.c $opts $includes
gcc -c src/vaapi.c $opts $includes
gcc -c src/window_texture.c $opts $includes
gcc -c src/time.c $opts $includes
g++ -c src/sound.cpp $opts $includes
g++ -c src/main.cpp $opts $includes
g++ -o gpu-screen-recorder -O2 capture.o nvfbc.o egl.o cuda.o vaapi.o window_texture.o time.o xcomposite_cuda.o xcomposite_drm.o sound.o main.o -s $libs
echo "Successfully built gpu-screen-recorder"
