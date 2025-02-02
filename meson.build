project('gpu-screen-recorder', ['c', 'cpp'], version : '5.0.1', default_options : ['warning_level=2'])

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
    'src/capture/kms.c',
    'src/encoder/video/video.c',
    'src/encoder/video/nvenc.c',
    'src/encoder/video/vaapi.c',
    'src/encoder/video/vulkan.c',
    'src/encoder/video/software.c',
    'src/codec_query/nvenc.c',
    'src/codec_query/vaapi.c',
    'src/codec_query/vulkan.c',
    'src/window/window.c',
    'src/window/window_x11.c',
    'src/window/window_wayland.c',
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
    'src/damage.c',
    'src/sound.cpp',
    'src/main.cpp',
]

dep = [
    dependency('threads'),
    dependency('libavcodec'),
    dependency('libavformat'),
    dependency('libavutil'),
    dependency('x11'),
    dependency('xcomposite'),
    dependency('xrandr'),
    dependency('xfixes'),
    dependency('xdamage'),
    dependency('libpulse'),
    dependency('libswresample'),
    dependency('libavfilter'),
    dependency('libva'),
    dependency('libva-drm'),
    dependency('libcap'),
    dependency('libdrm'),
    dependency('wayland-egl'),
    dependency('wayland-client'),
]

uses_pipewire = false

if get_option('portal') == true
    src += [
        'src/capture/portal.c',
        'src/dbus.c',
        'src/pipewire_video.c',
    ]
    dep += dependency('dbus-1')
    add_project_arguments('-DGSR_PORTAL', language : ['c', 'cpp'])
    uses_pipewire = true
endif

if get_option('app_audio') == true
    src += [
        'src/pipewire_audio.c',
    ]
    add_project_arguments('-DGSR_APP_AUDIO', language : ['c', 'cpp'])
    uses_pipewire = true
endif

if uses_pipewire == true
    dep += [
        dependency('libpipewire-0.3'),
        dependency('libspa-0.2'),
    ]
endif

add_project_arguments('-DGSR_VERSION="' + meson.project_version() + '"', language: ['c', 'cpp'])

executable('gsr-kms-server', 'kms/server/kms_server.c', dependencies : dependency('libdrm'), c_args : '-fstack-protector-all', install : true)
executable('gpu-screen-recorder', src, dependencies : dep, install : true)

if get_option('systemd') == true
    install_data(files('extra/gpu-screen-recorder.service'), install_dir : 'lib/systemd/user')
endif

if get_option('capabilities') == true
    meson.add_install_script('extra/meson_post_install.sh')
endif

if get_option('nvidia_suspend_fix') == true
    install_data(files('extra/gsr-nvidia.conf'), install_dir : 'lib/modprobe.d')
endif
