project('gpu-screen-recorder', ['c', 'cpp'], version : '3.8.0', default_options : ['warning_level=2'])

add_project_arguments('-Wshadow', language : ['c', 'cpp'])
if get_option('buildtype') == 'debug'
    add_project_arguments('-g3', language : ['c', 'cpp'])
elif get_option('buildtype') == 'release'
    add_project_arguments('-DNDEBUG', language : ['c', 'cpp'])
endif

src = [
    'kms/client/kms_client.c',
    'src/capture/capture.c',
    'src/capture/nvfbc.c',
    'src/capture/xcomposite.c',
    'src/capture/xcomposite_cuda.c',
    'src/capture/xcomposite_vaapi.c',
    'src/capture/xcomposite_software.c',
    'src/capture/kms_cuda.c',
    'src/capture/kms_vaapi.c',
    'src/capture/kms_software.c',
    'src/capture/kms.c',
    'src/egl.c',
    'src/cuda.c',
    'src/xnvctrl.c',
    'src/overclock.c',
    'src/window_texture.c',
    'src/shader.c',
    'src/color_conversion.c',
    'src/utils.c',
    'src/library_loader.c',
    'src/cursor.c',
    'src/sound.cpp',
    'src/main.cpp',
]

dep = [
    dependency('libavcodec'),
    dependency('libavformat'),
    dependency('libavutil'),
    dependency('x11'),
    dependency('xcomposite'),
    dependency('xrandr'),
    dependency('xfixes'),
    dependency('xdamage'),
    dependency('xi'),
    dependency('libpulse'),
    dependency('libswresample'),
    dependency('libavfilter'),
    dependency('libva'),
    dependency('libcap'),
    dependency('libdrm'),
    dependency('wayland-egl'),
    dependency('wayland-client'),
]

executable('gsr-kms-server', 'kms/server/kms_server.c', dependencies : dependency('libdrm'), c_args : '-fstack-protector-all', install : true)
executable('gpu-screen-recorder', src, dependencies : dep, install : true)

if get_option('systemd') == true
    install_data(files('extra/gpu-screen-recorder.service'), install_dir : '/usr/lib/systemd/user')
endif

if get_option('capabilities') == true
    meson.add_install_script('extra/meson_post_install.sh')
endif
