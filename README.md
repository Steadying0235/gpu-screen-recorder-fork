# GPU Screen Recorder
This is a screen recorder that has minimal impact on system performance by recording a window using the GPU only,
similar to shadowplay on windows. This is the fastest screen recording tool for Linux.

This screen recorder can be used for recording your desktop offline, for live streaming and for nvidia shadowplay-like instant replay,
where only the last few seconds are saved.

## Note
This software works with x11 and wayland, but when using AMD/Intel or Wayland then only monitors can be recorded.\
If you are using a variable refresh rate monitor on nvidia on x11 then choose to record "screen-direct-force". This will allow variable refresh rate to work when recording fullscreen applications. Note that some applications such as mpv will not work in fullscreen mode. A fix is being developed for this.\
GPU Screen Recorder only supports h264 and hevc codecs at the moment which means that webm files are not supported.\
CPU usage may be higher on wayland than on x11 when using nvidia.
### TEMPORARY ISSUES
1) screen-direct capture has been temporary disabled as it causes issues with stuttering. This might be a nvfbc bug.
2) Recording the monitor on steam deck might fail sometimes. This happens even when using ffmpeg directly. This might be a steam deck driver bug. Recording a single window doesn't have this issue.
3) Videos created on AMD/Intel are in variable framerate format. Use MPV to play such videos, otherwise you might experience stuttering in the video if you are using a buggy video player. Try saving the video into a .mkv file instead when using AMD/Intel, as some software may have better support for .mkv files (such as kdenlive).
### AMD/Intel/Wayland root permission
When recording a window under AMD/Intel no special user permission is required, however when recording a monitor (or when using wayland) the program needs root permission (to access KMS).\
To make this safer, the part that needs root access has been moved to its own executable (to make it as small as possible).\
For you as a user this only means that if you installed GPU Screen Recorder as a flatpak then a prompt asking for root password will show up when you start recording.
# Performance
On a system with a i5 4690k CPU and a GTX 1080 GPU:\
When recording Legend of Zelda Breath of the Wild at 4k, fps drops from 30 to 7 when using OBS Studio + nvenc, however when using this screen recorder the fps remains at 30.\
When recording GTA V at 4k on highest settings, fps drops from 60 to 23 when using obs-nvfbc + nvenc, however when using this screen recorder the fps only drops to 58. The quality is also much better when using gpu screen recorder.\
It is recommended to save the video to a SSD because of the large file size, which a slow HDD might not be fast enough to handle.\
Note that if you have a very powerful CPU and a not so powerful GPU and play a game that is bottlenecked by your GPU and barely uses your CPU then a CPU based screen recording (such as OBS with libx264 instead of nvenc) might perform slightly better than GPU Screen Recorder. At least on NVIDIA.
## Note about optimal performance on NVIDIA
NVIDIA driver has a "feature" (read: bug) where it will downclock memory transfer rate when a program uses cuda (or nvenc, which uses cuda), such as GPU Screen Recorder. To work around this bug, GPU Screen Recorder can overclock your GPU memory transfer rate to it's normal optimal level.\
To enable overclocking for optimal performance use the `-oc` option when running GPU Screen Recorder. You also need to have "Coolbits" NVIDIA X setting set to "12" to enable overclocking. You can automatically add this option if you run `sudo nvidia-xconfig --cool-bits=12` and then reboot your computer.\
Note that this only works when Xorg server is running as root, and using this option will only give you a performance boost if the game you are recording is bottlenecked by your GPU.\
Note! use at your own risk!
## Note about optimal performance on AMD/Intel
Performance is the same when recording a single window or the monitor, however in some cases, such as when gpu usage is 100%, the video capture rate might be slower than the games fps when recording a single window instead of a monitor. Recording the monitor instead is recommended in such cases.

# Installation
If you are running an Arch Linux based distro, then you can find gpu screen recorder on aur under the name gpu-screen-recorder-git (`yay -S gpu-screen-recorder-git`).\
If you are running another distro then you can run `sudo ./install.sh`, but you need to manually install the dependencies, as described below.\
You can also install gpu screen recorder ([the gtk gui version](https://git.dec05eba.com/gpu-screen-recorder-gtk/)) from [flathub](https://flathub.org/apps/details/com.dec05eba.gpu_screen_recorder), which is the easiest method
to install GPU Screen Recorder on non-arch based distros.\
Do NOT install the NixOS package for GPU Screen Recorder or any other non official package. They are likely severly outdated with many bugs and only have a fraction of the functionality.\
If you install GPU Screen Recorder flatpak, which is the gtk gui version then you can still run GPU Screen Recorder command line by using the flatpak command option, for example `flatpak run --command=gpu-screen-recorder com.dec05eba.gpu_screen_recorder -w screen -f 60 -o video.mp4`. Note that if you want to record your monitor on AMD/Intel then you need to install the flatpak system-wide (like so: `flatpak install flathub --system com.dec05eba.gpu_screen_recorder`).

# Dependencies
## AMD
`libglvnd (which provides libgl and libegl), mesa, ffmpeg (libavcodec, libavformat, libavutil, libswresample, libavfilter), libx11, libxcomposite, libxrandr, libpulse, libva, libva-mesa-driver, libdrm, libcap, polkit (for pkexec), wayland-client`.
## Intel
`libglvnd (which provides libgl and libegl), mesa, ffmpeg (libavcodec, libavformat, libavutil, libswresample, libavfilter), libx11, libxcomposite, libxrandr, libpulse, libva, libva-intel-driver, libdrm, libcap, polkit (for pkexec), wayland-client`.
## NVIDIA
`libglvnd (which provides libgl and libegl), ffmpeg (libavcodec, libavformat, libavutil, libswresample, libavfilter), libx11, libxcomposite, libxrandr, libpulse, cuda runtime (libcuda.so.1) (libnvidia-compute), nvenc (libnvidia-encode), libva, libdrm, libcap, polkit (for pkexec, only on wayland), wayland-client`. Additionally, you need to have `nvfbc (libnvidia-fbc1)` installed when using nvfbc and `xnvctrl (libxnvctrl0)` when using the `-oc` option.

# How to use
Run `gpu-screen-recorder --help` to see all options.
## Recording
Here is an example of how to record all monitors and the default audio output: `gpu-screen-recorder -w screen -f 60 -a "$(pactl get-default-sink).monitor" -o ~/Videos/test_video.mp4` then stop the screen recorder with `Ctrl+C`, which will also save the recording. You can record a single monitor if you change `-w screen` to the name of a monitor, which you can find if you run the `xrandr`. An example of a monitor name is HDMI-1.
## Streaming
Streaming works the same as recording, but the `-o` argument should be path to the live streaming service you want to use (including your live streaming key). Take a look at scripts/twitch-stream.sh to see an example of how to stream to twitch.
## Replay mode
Run `gpu-screen-recorder` with the `-c mp4` and `-r` option, for example: `gpu-screen-recorder -w screen -f 60 -r 30 -c mp4 -o ~/Videos`. Note that in this case, `-o` should point to a directory (that exists).
To save a video in replay mode, you need to send signal SIGUSR1 to gpu screen recorder. You can do this by running `killall -SIGUSR1 gpu-screen-recorder`.
To stop recording, send SIGINT to gpu screen recorder. You can do this by running `killall gpu-screen-recorder` or pressing `Ctrl-C` in the terminal that runs gpu screen recorder.
## Finding audio device name
You can find the default output audio device (headset, speakers (in other words, desktop audio)) with the command `pactl get-default-sink`. Add `monitor` to the end of that to use that as an audio input in gpu screen recorder.\
You can find the default input audio device (microphone) with the command `pactl get-default-source`. This input should not have `monitor` added to the end when used in gpu screen recorder.\
Example of recording both desktop audio and microphone: `gpu-screen-recorder -w screen -f 60 -a "$(pactl get-default-sink).monitor" -a "$(pactl get-default-source)" -o ~/Videos/test_video.mp4`.\
A name (that is visible to pipewire) can be given to an audio input device by prefixing the audio input with `<name>/`, for example `dummy/$(pactl get-default-sink).monitor`.\
Note that if you use multiple audio inputs then they are each recorded into separate audio tracks in the video file. If you want to merge multiple audio inputs into one audio track then separate the audio inputs by "|" in one -a argument,
for example `-a "$(pactl get-default-sink).monitor|$(pactl get-default-source)"`.

There is also a gui for the gpu screen recorder called [gpu-screen-recorder-gtk](https://git.dec05eba.com/gpu-screen-recorder-gtk/).
## Simple way to run replay without gui
Run the script `scripts/start-replay.sh` to start replay and then `scripts/save-replay.sh` to save a replay and `scripts/stop-replay.sh` to stop the replay. The videos are saved to `$HOME/Videos`.
You can use these scripts to start replay at system startup if you add `scripts/start-replay.sh` to startup (this can be done differently depending on your desktop environment / window manager) and then go into
hotkey settings on your system and choose a hotkey to run the script `scripts/save-replay.sh`. Modify `scripts/start-replay.sh` if you want to use other replay options.


If you are running a distro that uses systemd then you can use the systemd service in `extra/gpu-screen-recorder.service` instead.
Copy `extra/gpu-screen-recorder.service` to a location where systemd can find it, for example: `$HOME/.config/systemd/user` and then enable and start it with: `systemctl enable --now --user gpu-screen-recorder`. Copying the systemd service file is not needed if you installed GPU Screen Recorder from AUR as this is done automatically.
You can then use the same `scripts/save-replay.sh` script to save a replay. The systemd service is configured with the file `$HOME/.config/gpu-screen-recorder.env` (create it if it doesn't exist).
You can see which variables that you can use by looking at the gpu-screen-recorder.service file. In general you only need to set the `WINDOW` variable to make it work.
Restart the systemd service after modifying that configuration file. By default it saves videos in `$HOME/Videos`.\
If you are using a NVIDIA GPU then it's recommended to set PreserveVideoMemoryAllocations=1 as mentioned in the section below.

## Issues
### NVIDIA
Nvidia drivers have an issue where CUDA breaks if CUDA is running when suspend/hibernation happens, and it remains broken until you reload the nvidia driver. To fix this, either disable suspend or tell the NVIDIA driver to preserve video memory on suspend/hibernate by using the `NVreg_PreserveVideoMemoryAllocations=1` option. You can run `sudo extra/install_preserve_video_memory.sh` to automatically add that option to your system.

# Demo
[![Click here to watch a demo video on youtube](https://img.youtube.com/vi/n5tm0g01n6A/0.jpg)](https://www.youtube.com/watch?v=n5tm0g01n6A)

# FAQ
## How is this different from using OBS with nvenc?
OBS only uses the gpu for video encoding, but the window image that is encoded is copied from the GPU to the CPU and then back to the GPU (video encoding unit). These operations are very slow and causes all of the fps drops when using OBS. OBS only uses the GPU efficiently on Windows 10 and Nvidia.\
This gpu screen recorder keeps the window image on the GPU and sends it directly to the video encoding unit on the GPU by using CUDA. This means that CPU usage remains at around 0% when using this screen recorder.
## How is this different from using OBS NvFBC plugin?
The plugin does everything on the GPU and gives the texture to OBS, but OBS does not know how to use the texture directly on the GPU so it copies the texture to the CPU and then back to the GPU (video encoding unit). These operations are very slow and causes a lot of fps drops unless you have a fast CPU. This is especially noticable when recording at higher resolutions than 1080p.
## How is this different from using FFMPEG with x11grab and nvenc?
FFMPEG only uses the GPU with CUDA when doing transcoding from an input video to an output video, and not when recording the screen when using x11grab. So FFMPEG has the same fps drop issues that OBS has.
## It tells me that my AMD/Intel GPU is not supported or that my GPU doesn't support h264/hevc, but that's not true!
Some linux distros (such as manjaro) disable hardware accelerated h264/hevc on AMD/Intel because of "patent license issues". If you are using an arch-based distro then you can install mesa-git instead of mesa and if you are using another distro then you may have to switch to a better distro.

# Donations
If you want to donate you can donate via bitcoin or monero.
* Bitcoin: bc1qqvuqnwrdyppf707ge27fqz2n9y9gu7lf5ypyuf
* Monero: 4An9kp2qW1C9Gah7ewv4JzcNFQ5TAX7ineGCqXWK6vQnhsGGcRpNgcn8r9EC3tMcgY7vqCKs3nSRXhejMHBaGvFdN2egYet

# TODO
* Dynamically change bitrate/resolution to match desired fps. This would be helpful when streaming for example, where the encode output speed also depends on upload speed to the streaming service.
* Show cursor when recording a window. Currently the cursor is only visible when recording a monitor.
* Implement opengl injection to capture texture. This fixes VRR without having to use NvFBC direct capture.
* Always use direct capture with NvFBC once the capture issue in mpv fullscreen has been resolved (maybe detect if direct capture fails in nvfbc and switch to non-direct recording. NvFBC says if direct capture fails).
